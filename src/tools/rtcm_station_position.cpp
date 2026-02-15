#include "rtcmbridge/core/mountpoint_config.hpp"
#include "rtcmbridge/core/ntrip_client.hpp"
#include "rtcmbridge/core/rtcm_1005.hpp"
#include "rtcmbridge/core/rtcm_frame.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
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
    std::string mode = "rtcm";
    std::string out_dir = "./data";
    std::string station = "STATION";

    std::string solver_cmd;
    int solve_interval_sec = 300;
    int min_data_sec = 1800;
    int progress_interval_sec = 60;
};

struct RunningPositionStats {
    size_t n = 0;
    double mean_x = 0.0;
    double mean_y = 0.0;
    double mean_z = 0.0;
    double m2_x = 0.0;
    double m2_y = 0.0;
    double m2_z = 0.0;

    void add(double x, double y, double z)
    {
        ++n;

        const double dx = x - mean_x;
        mean_x += dx / static_cast<double>(n);
        m2_x += dx * (x - mean_x);

        const double dy = y - mean_y;
        mean_y += dy / static_cast<double>(n);
        m2_y += dy * (y - mean_y);

        const double dz = z - mean_z;
        mean_z += dz / static_cast<double>(n);
        m2_z += dz * (z - mean_z);
    }

    double sigma_3d() const
    {
        if (n <= 1) return 9999.0;
        const double sx = std::sqrt(m2_x / static_cast<double>(n - 1));
        const double sy = std::sqrt(m2_y / static_cast<double>(n - 1));
        const double sz = std::sqrt(m2_z / static_cast<double>(n - 1));
        return std::sqrt((sx * sx + sy * sy + sz * sz) / 3.0);
    }
};

struct WorkerResult {
    std::string mountpoint;
    int rc = 0;
    size_t frames_ok = 0;
    size_t frames_bad_crc = 0;
    size_t solutions = 0;
    double final_sigma3d = 0.0;
};

/* Timestamp UTC usado para etiquetar outputs por sesión. */
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

/* Escape simple de comillas para argumentos interpolados en comandos shell. */
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

/* Reemplazo de placeholders en solver_cmd: {rtcm}, {solution}, {workdir}. */
static std::string replace_all(std::string s, const std::string& what, const std::string& with)
{
    size_t p = 0;
    while ((p = s.find(what, p)) != std::string::npos) {
        s.replace(p, what.size(), with);
        p += with.size();
    }
    return s;
}

/* Lee la última solución válida emitida por el solver externo en formato X Y Z SIGMA. */
static std::optional<std::array<double, 4>> read_solver_output(const fs::path& path)
{
    std::ifstream in(path);
    if (!in) return std::nullopt;

    std::string line;
    std::optional<std::array<double, 4>> last;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::array<double, 4> v{};
        if (iss >> v[0] >> v[1] >> v[2] >> v[3]) last = v;
    }
    return last;
}

/* Logger serializado para no mezclar líneas de varios mountpoints/hilos. */
static void log_line(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::cout << msg << "\n";
}

/* Muestra ayuda de uso del binario. */
static void print_usage()
{
    std::cerr << "Usage: rtcm_station_position "
              << "[--mountpoint=<mp>] [--mountpoints-file=mountpoints.conf] "
              << "[--mode=rtcm|ppp] [--out-dir=./data] [--station=STATION] "
              << "[--solver-cmd='... {rtcm} ... {solution} ... {workdir} ...'] "
              << "[--solve-interval-sec=300] [--min-data-sec=1800] "
              << "[--progress-interval-sec=60]\n";
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
    get("mode", cfg.mode);
    get("out-dir", cfg.out_dir);
    get("station", cfg.station);

    std::string s;
    get("solver-cmd", cfg.solver_cmd);
    if (get("solve-interval-sec", s)) cfg.solve_interval_sec = std::stoi(s);
    if (get("min-data-sec", s)) cfg.min_data_sec = std::stoi(s);
    if (get("progress-interval-sec", s)) cfg.progress_interval_sec = std::stoi(s);

    return true;
}

/*
 * Ejecuta posicionamiento para un mountpoint.
 * - Modo rtcm: usa mensajes 1005/1006 del stream.
 * - Modo ppp: lanza solver externo de forma periódica.
 */
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
    const fs::path solver_out = fs::path(cfg.out_dir) / (station_prefix + "_" + stamp + ".ppp.sol");
    const fs::path solver_workdir = fs::path(cfg.out_dir) / (station_prefix + "_" + stamp + "_ppp_work");
    fs::create_directories(solver_workdir);

    std::ofstream rtcm_file(rtcm_path, std::ios::binary);
    if (!rtcm_file) {
        log_line("[station][" + ntrip.mountpoint + "] cannot open output file: " + rtcm_path.string());
        out.rc = 4;
        return out;
    }

    RunningPositionStats stats;
    RtcmFrameParser parser;
    std::mutex stats_mtx;
    std::atomic<bool> first_solution_announced{false};
    double best_sigma3d = std::numeric_limits<double>::infinity();

    auto maybe_add_solution = [&](double x, double y, double z, const std::string& source) {
        std::lock_guard<std::mutex> lk(stats_mtx);
        if (stats.n >= 5) {
            const double dx = x - stats.mean_x;
            const double dy = y - stats.mean_y;
            const double dz = z - stats.mean_z;
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 2.0) {
                log_line("[station][" + ntrip.mountpoint + "] reject " + source +
                         " outlier dist=" + std::to_string(dist) + " m");
                return;
            }
        }

        stats.add(x, y, z);
        const double s3d = stats.sigma_3d();
        out.solutions = stats.n;
        out.final_sigma3d = s3d;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4)
            << "[station][" << ntrip.mountpoint << "] "
            << "n=" << stats.n
            << " source=" << source
            << " mean_ecef_m=(" << stats.mean_x << ", " << stats.mean_y << ", " << stats.mean_z << ")"
            << " sigma3d=" << s3d << " m";
        log_line(oss.str());

        if (!first_solution_announced.exchange(true)) {
            log_line("[station][" + ntrip.mountpoint + "] first solution ready");
        }
        if (s3d < best_sigma3d) {
            best_sigma3d = s3d;
            std::ostringstream best;
            best << std::fixed << std::setprecision(4)
                 << "[station][" << ntrip.mountpoint << "] best accuracy so far sigma3d="
                 << best_sigma3d << " m";
            log_line(best.str());
        }
    };

    std::thread ppp_thread;
    if (cfg.mode == "ppp") {
        ppp_thread = std::thread([&]() {
            const auto started = std::chrono::steady_clock::now();
            auto last_try = std::chrono::steady_clock::now();
            auto last_progress = std::chrono::steady_clock::now();

            while (g_running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const auto now = std::chrono::steady_clock::now();
                const auto age_s = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
                const auto since_try_s = std::chrono::duration_cast<std::chrono::seconds>(now - last_try).count();
                const auto since_progress_s =
                    std::chrono::duration_cast<std::chrono::seconds>(now - last_progress).count();

                if (cfg.progress_interval_sec > 0 && since_progress_s >= cfg.progress_interval_sec) {
                    last_progress = now;
                    std::error_code ec{};
                    const auto size = fs::file_size(rtcm_path, ec);
                    log_line("[station][" + ntrip.mountpoint + "] [ppp] elapsed=" + std::to_string(age_s) +
                             "s rtcm_bytes=" + std::to_string(ec ? 0 : size) +
                             " next_solve_in=" +
                             std::to_string(std::max(0, cfg.solve_interval_sec - static_cast<int>(since_try_s))) +
                             "s");
                }

                if (age_s < cfg.min_data_sec) continue;
                if (since_try_s < cfg.solve_interval_sec) continue;
                last_try = now;

                std::string cmd = cfg.solver_cmd;
                cmd = replace_all(cmd, "{rtcm}", sh_quote(rtcm_path.string()));
                cmd = replace_all(cmd, "{solution}", sh_quote(solver_out.string()));
                cmd = replace_all(cmd, "{workdir}", sh_quote(solver_workdir.string()));

                const int solver_rc = std::system(cmd.c_str());
                if (solver_rc != 0) {
                    log_line("[station][" + ntrip.mountpoint + "] [ppp] solver exited with code " +
                             std::to_string(solver_rc));
                    continue;
                }

                const auto sol = read_solver_output(solver_out);
                if (!sol) {
                    log_line("[station][" + ntrip.mountpoint + "] [ppp] no valid solution in " +
                             solver_out.string());
                    continue;
                }
                maybe_add_solution((*sol)[0], (*sol)[1], (*sol)[2], "ppp");
            }
        });
    }

    NtripStreamClient client;
    log_line("[station][" + ntrip.mountpoint + "] streaming mode=" + cfg.mode +
             " rtcm_file=" + rtcm_path.string());
    out.rc = client.run(
        ntrip,
        [&](const uint8_t* data, size_t len) {
            parser.push(data, len, [&](const uint8_t* frame, size_t frame_len) {
                rtcm_file.write(reinterpret_cast<const char*>(frame),
                                static_cast<std::streamsize>(frame_len));

                if (cfg.mode == "rtcm") {
                    const uint8_t* payload = frame + 3;
                    const size_t payload_len = frame_len - 6;
                    const auto pos = decode_station_position_1005_1006(payload, payload_len);
                    if (pos) {
                        maybe_add_solution(pos->x_m, pos->y_m, pos->z_m,
                                           "rtcm" + std::to_string(pos->message_type));
                    }
                }
            });
            return g_running.load();
        },
        g_running,
        [&](const std::string& m) { log_line("[station][" + ntrip.mountpoint + "] " + m); });

    if (ppp_thread.joinable()) ppp_thread.join();
    rtcm_file.close();

    out.frames_ok = parser.frames_ok();
    out.frames_bad_crc = parser.frames_bad_crc();
    return out;
}

/*
 * Entry point:
 * - carga mountpoints del conf
 * - aplica filtro opcional
 * - lanza workers concurrentes (uno por mountpoint)
 * - reporta resumen final
 */
int main(int argc, char* argv[])
{
    AppConfig cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage();
        return 1;
    }

    if (cfg.mode != "rtcm" && cfg.mode != "ppp") {
        std::cerr << "Invalid mode '" << cfg.mode << "'. Use --mode=rtcm|ppp\n";
        return 1;
    }
    if (cfg.mode == "ppp" && cfg.solver_cmd.empty()) {
        std::cerr << "PPP mode requires --solver-cmd\n";
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

    log_line("[station] starting workers: " + std::to_string(selected.size()) +
             " mode=" + cfg.mode);
    for (size_t i = 0; i < selected.size(); ++i) {
        threads.emplace_back([&, i]() { results[i] = run_worker(selected[i], cfg, stamp, multi_mode); });
    }

    for (auto& t : threads) t.join();
    g_running = false;

    int global_rc = 0;
    for (const auto& r : results) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4)
            << "[station][" << r.mountpoint << "] stopped"
            << " frames_ok=" << r.frames_ok
            << " bad_crc=" << r.frames_bad_crc
            << " solutions=" << r.solutions
            << " sigma3d=" << r.final_sigma3d << " m";
        log_line(oss.str());
        if (r.rc != 0) global_rc = r.rc;
    }
    return global_rc;
}
