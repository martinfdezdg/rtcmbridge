#include "rtcmbridge/core/mountpoint_config.hpp"
#include "rtcmbridge/core/ntrip_client.hpp"
#include "rtcmbridge/core/rtcm_frame.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace rtcmbridge;

static std::atomic<bool> g_running{true};
static std::mutex g_log_mtx;

/* Handler POSIX para parada ordenada del proceso (Ctrl+C / SIGTERM). */
static void on_signal(int)
{
    g_running = false;
}

struct AppConfig {
    std::string mountpoint_filter;
    std::string mountpoints_file = "mountpoints.conf";
    std::string out_dir = "./data";
    std::string station = "STATION";
    std::string convbin;
    std::string rinex_version = "3.04";
};

struct WorkerResult {
    std::string mountpoint;
    int rc = 0;
    size_t frames_ok = 0;
    size_t frames_bad_crc = 0;
    std::uint64_t bytes_written = 0;
};

/* Timestamp UTC usado para nombrar artefactos de salida. */
static std::string now_stamp_utc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

/* Escape simple de comillas para construir comandos shell con rutas seguras. */
static std::string sh_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

/* Logger serializado para evitar mezcla de líneas entre hilos. */
static void log_line(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::cout << msg << "\n";
}

/* Muestra ayuda de uso del binario. */
static void print_usage()
{
    std::cerr << "Usage: rtcm_rinex_recorder "
              << "[--mountpoint=<mp>] [--mountpoints-file=mountpoints.conf] "
              << "[--out-dir=./data] [--station=STATION] "
              << "[--convbin=/path/convbin] [--rinex-version=3.04]\n";
}

/*
 * Parser de argumentos estilo --key=value.
 * mountpoint es opcional: si se omite, procesa todos los del conf.
 */
static bool parse_args(int argc, char* argv[], AppConfig& cfg)
{
    std::map<std::string, std::string> kv;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") return false;
        if (arg.rfind("--", 0) != 0) continue;
        const size_t eq = arg.find('=');
        if (eq == std::string::npos) {
            kv[arg.substr(2)] = "1";
        } else {
            kv[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
        }
    }

    auto get = [&](const std::string& k, std::string& out) {
        const auto it = kv.find(k);
        if (it == kv.end()) return false;
        out = it->second;
        return true;
    };

    get("mountpoint", cfg.mountpoint_filter);  // optional filter
    get("mountpoints-file", cfg.mountpoints_file);
    get("out-dir", cfg.out_dir);
    get("station", cfg.station);
    get("convbin", cfg.convbin);
    get("rinex-version", cfg.rinex_version);
    return true;
}

/* Ejecuta el pipeline completo para un mountpoint: captura RTCM y opcional convbin. */
static WorkerResult run_worker(const NtripConfig& ntrip,
                               const AppConfig& cfg,
                               const std::string& stamp,
                               const bool multi_mode)
{
    WorkerResult out;
    out.mountpoint = ntrip.mountpoint;

    const std::string station_prefix =
        (cfg.station == "STATION") ? ntrip.mountpoint
                                   : (multi_mode ? (cfg.station + "_" + ntrip.mountpoint) : cfg.station);

    const fs::path rtcm_path = fs::path(cfg.out_dir) / (station_prefix + "_" + stamp + ".rtcm3");
    const fs::path obs_path = fs::path(cfg.out_dir) / (station_prefix + "_" + stamp + ".obs");
    const fs::path nav_path = fs::path(cfg.out_dir) / (station_prefix + "_" + stamp + ".nav");

    std::ofstream rtcm_file(rtcm_path, std::ios::binary);
    if (!rtcm_file) {
        log_line("[recorder][" + ntrip.mountpoint + "] cannot open output file: " + rtcm_path.string());
        out.rc = 2;
        return out;
    }

    RtcmFrameParser parser;
    NtripStreamClient client;
    log_line("[recorder][" + ntrip.mountpoint + "] streaming from " + ntrip.host + "/" + ntrip.mountpoint +
             " into " + rtcm_path.string());

    out.rc = client.run(
        ntrip,
        [&](const uint8_t* data, size_t len) {
            parser.push(data, len, [&](const uint8_t* frame, size_t frame_len) {
                rtcm_file.write(reinterpret_cast<const char*>(frame),
                                static_cast<std::streamsize>(frame_len));
                out.bytes_written += frame_len;
            });
            return g_running.load();
        },
        g_running,
        [&](const std::string& m) { log_line("[recorder][" + ntrip.mountpoint + "] " + m); });

    rtcm_file.close();
    out.frames_ok = parser.frames_ok();
    out.frames_bad_crc = parser.frames_bad_crc();

    if (!cfg.convbin.empty()) {
        const std::string cmd =
            sh_quote(cfg.convbin) + " -r rtcm3 -v " + sh_quote(cfg.rinex_version) +
            " -o " + sh_quote(obs_path.string()) +
            " -n " + sh_quote(nav_path.string()) +
            " " + sh_quote(rtcm_path.string());
        const int conv_rc = std::system(cmd.c_str());
        if (conv_rc == 0) {
            log_line("[recorder][" + ntrip.mountpoint + "] RINEX generated: " + obs_path.string() +
                     " and " + nav_path.string());
        } else {
            log_line("[recorder][" + ntrip.mountpoint + "] convbin failed with code " +
                     std::to_string(conv_rc));
            out.rc = (out.rc == 0) ? 3 : out.rc;
        }
    }

    return out;
}

/*
 * Entry point:
 * - carga mountpoints del conf
 * - aplica filtro opcional
 * - lanza un worker por mountpoint en paralelo
 * - agrega estadísticas finales
 */
int main(int argc, char* argv[])
{
    AppConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage();
        return 1;
    }

    const auto all_cfgs = load_all_ntrip_configs_from_mountpoints(cfg.mountpoints_file);
    if (all_cfgs.empty()) {
        std::cerr << "No valid mountpoints in " << cfg.mountpoints_file << "\n";
        return 1;
    }

    std::vector<NtripConfig> selected;
    selected.reserve(all_cfgs.size());
    for (const auto& c : all_cfgs) {
        if (cfg.mountpoint_filter.empty() || c.mountpoint == cfg.mountpoint_filter) {
            selected.push_back(c);
        }
    }

    if (selected.empty()) {
        std::cerr << "Mountpoint '" << cfg.mountpoint_filter << "' not found in "
                  << cfg.mountpoints_file << "\n";
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fs::create_directories(cfg.out_dir);

    const std::string stamp = now_stamp_utc();
    const bool multi_mode = selected.size() > 1;

    std::vector<WorkerResult> results(selected.size());
    std::vector<std::thread> threads;
    threads.reserve(selected.size());

    log_line("[recorder] starting workers: " + std::to_string(selected.size()));
    for (size_t i = 0; i < selected.size(); ++i) {
        threads.emplace_back([&, i]() { results[i] = run_worker(selected[i], cfg, stamp, multi_mode); });
    }

    for (auto& t : threads) t.join();

    int global_rc = 0;
    for (const auto& r : results) {
        log_line("[recorder][" + r.mountpoint + "] stopped frames_ok=" + std::to_string(r.frames_ok) +
                 " bad_crc=" + std::to_string(r.frames_bad_crc) +
                 " bytes=" + std::to_string(r.bytes_written));
        if (r.rc != 0) global_rc = r.rc;
    }

    return global_rc;
}
