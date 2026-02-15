/*
 * RTCM3 Decoder over NATS
 * Subscribes to NTRIP.<mountpoint> and decodes RTCM frames in real time.
 */

#include <nats/nats.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static std::atomic<bool> g_running{true};

/*
 * Handler de señales POSIX (SIGINT/SIGTERM).
 * No cierra recursos directamente: solo marca la bandera global para
 * que el bucle principal termine de forma ordenada.
 */
static void on_signal(int) { g_running = false; }

/*
 * Calcula CRC24Q sobre una trama RTCM.
 * - data: bytes desde el preámbulo (0xD3) hasta el final del payload.
 * - len: cantidad de bytes a validar.
 * Devuelve el CRC de 24 bits usado por RTCM3.
 */
static uint32_t crc24q(const uint8_t* data, size_t len)
{
    static constexpr uint32_t poly = 0x1864CFB;
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 16;
        for (int b = 0; b < 8; ++b) {
            crc <<= 1;
            if (crc & 0x1000000U) crc ^= poly;
        }
    }
    return crc & 0xFFFFFFU;
}

class BitReader {
public:
    /*
     * Inicializa lector a nivel de bits sobre un buffer continuo.
     * La lectura se hace MSB-first, tal como define RTCM.
     */
    BitReader(const uint8_t* data, size_t bytes)
        : data_(data), total_bits_(bytes * 8), bitpos_(0) {}

    /*
     * Lee n bits sin signo y avanza el cursor.
     * Devuelve false si no hay suficientes bits disponibles.
     */
    bool getU(int n, uint64_t& out) {
        if (n < 0 || n > 64) return false;
        if (bitpos_ + static_cast<size_t>(n) > total_bits_) return false;
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            const size_t idx = bitpos_ + static_cast<size_t>(i);
            const uint8_t bit = (data_[idx / 8] >> (7 - (idx % 8))) & 0x01;
            v = (v << 1) | bit;
        }
        bitpos_ += static_cast<size_t>(n);
        out = v;
        return true;
    }

    /*
     * Lee n bits con signo en complemento a dos y avanza el cursor.
     * Internamente reutiliza getU y luego hace sign-extension.
     */
    bool getS(int n, int64_t& out) {
        uint64_t u = 0;
        if (!getU(n, u)) return false;
        if (n == 0) {
            out = 0;
            return true;
        }
        if ((u >> (n - 1)) & 1U) {
            const uint64_t mask = (~0ULL) << n;
            u |= mask;
        }
        out = static_cast<int64_t>(u);
        return true;
    }

    /*
     * Omite n bits del stream sin leerlos explícitamente.
     * Útil para campos reservados o no relevantes para el resumen.
     */
    bool skip(size_t nbits) {
        if (bitpos_ + nbits > total_bits_) return false;
        bitpos_ += nbits;
        return true;
    }

private:
    const uint8_t* data_;
    size_t total_bits_;
    size_t bitpos_;
};

/* Cuenta bits activos en una máscara de 64 bits (satélites MSM). */
static int popcount64(uint64_t v) { return __builtin_popcountll(v); }
/* Cuenta bits activos en una máscara de 32 bits (señales MSM). */
static int popcount32(uint32_t v) { return __builtin_popcount(v); }

/* Devuelve hora local en formato HH:MM:SS para logging de consola. */
static std::string now_hms()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

/*
 * Lee una cadena ASCII codificada como bytes en el payload RTCM.
 * Caracteres no imprimibles se sustituyen por '.' para evitar ruido.
 */
static std::string read_ascii(BitReader& br, size_t chars)
{
    std::string out;
    out.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        uint64_t c = 0;
        if (!br.getU(8, c)) break;
        char ch = static_cast<char>(c);
        out.push_back(std::isprint(static_cast<unsigned char>(ch)) ? ch : '.');
    }
    return out;
}

class RtcmMessageDecoder {
public:
    /*
     * Decodificador principal de payload RTCM (sin cabecera ni CRC).
     * Extrae tipo de mensaje (12 bits) y delega en un parser específico
     * cuando existe soporte; si no, devuelve resumen genérico.
     */
    std::string decode(const uint8_t* payload, size_t len)
    {
        if (len < 2) return "payload_too_short";

        BitReader br(payload, len);
        uint64_t msg_type = 0;
        if (!br.getU(12, msg_type)) return "cannot_read_message_type";

        std::ostringstream oss;
        const int type = static_cast<int>(msg_type);
        oss << "type=" << type;
        const std::string name = type_name(type);
        if (!name.empty()) oss << " (" << name << ")";
        oss << " len=" << len;

        if (decode_1005_1006(type, br, oss)) return oss.str();
        if (decode_1007_1008(type, br, oss)) return oss.str();
        if (decode_1033(type, br, oss)) return oss.str();
        if (decode_1013(type, br, oss)) return oss.str();
        if (decode_1019(type, br, oss)) return oss.str();
        if (decode_1020(type, br, oss)) return oss.str();
        if (decode_1045_1046(type, br, oss)) return oss.str();
        if (decode_1230(type, br, oss)) return oss.str();
        if (decode_msm(type, br, oss)) return oss.str();

        return oss.str();
    }

private:
    /*
     * Traduce número de tipo RTCM a nombre legible para salida humana.
     * Si el tipo no está en el mapa, devuelve cadena vacía.
     */
    static std::string type_name(int t)
    {
        static const std::unordered_map<int, std::string> names = {
            {1005, "Stationary RTK ARP"},
            {1006, "Stationary RTK ARP + Antenna Height"},
            {1007, "Antenna Descriptor"},
            {1008, "Antenna Descriptor + Serial"},
            {1013, "System Parameters"},
            {1019, "GPS Ephemeris"},
            {1020, "GLONASS Ephemeris"},
            {1033, "Receiver/Antenna Descriptor"},
            {1045, "Galileo F/NAV Ephemeris"},
            {1046, "Galileo I/NAV Ephemeris"},
            {1074, "GPS MSM4"}, {1075, "GPS MSM5"},
            {1076, "GPS MSM6"}, {1077, "GPS MSM7"},
            {1084, "GLONASS MSM4"}, {1085, "GLONASS MSM5"},
            {1086, "GLONASS MSM6"}, {1087, "GLONASS MSM7"},
            {1094, "Galileo MSM4"}, {1095, "Galileo MSM5"},
            {1096, "Galileo MSM6"}, {1097, "Galileo MSM7"},
            {1104, "SBAS MSM4"}, {1105, "SBAS MSM5"},
            {1106, "SBAS MSM6"}, {1107, "SBAS MSM7"},
            {1114, "QZSS MSM4"}, {1115, "QZSS MSM5"},
            {1116, "QZSS MSM6"}, {1117, "QZSS MSM7"},
            {1124, "BeiDou MSM4"}, {1125, "BeiDou MSM5"},
            {1126, "BeiDou MSM6"}, {1127, "BeiDou MSM7"},
            {1230, "GLONASS Code-Phase Bias"}
        };
        auto it = names.find(t);
        return it == names.end() ? "" : it->second;
    }

    /*
     * Decodifica RTCM 1005/1006:
     * referencia de estación y coordenadas ECEF, con altura de antena
     * adicional en 1006.
     */
    static bool decode_1005_1006(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1005 && type != 1006) return false;
        uint64_t stid = 0, itrf = 0, gps = 0, glo = 0, gal = 0, ref = 0, quarter = 0;
        uint64_t single = 0, reserved = 0, h = 0;
        int64_t x = 0, y = 0, z = 0;
        if (!br.getU(12, stid) || !br.getU(6, itrf) || !br.getU(1, gps) || !br.getU(1, glo) ||
            !br.getU(1, gal) || !br.getU(1, ref) || !br.getS(38, x) || !br.getU(1, single) ||
            !br.getU(1, reserved) || !br.getS(38, y) || !br.getU(2, quarter) || !br.getS(38, z)) {
            oss << " decode_error=invalid_1005_1006";
            return true;
        }
        const double scale = 0.0001;
        oss << " staid=" << stid
            << " itrf=" << itrf
            << " x=" << (x * scale)
            << " y=" << (y * scale)
            << " z=" << (z * scale)
            << " gps=" << gps << " glo=" << glo << " gal=" << gal;
        if (type == 1006) {
            if (!br.getU(16, h)) {
                oss << " decode_error=missing_antenna_height";
                return true;
            }
            oss << " ant_h=" << (static_cast<double>(h) * scale);
        }
        return true;
    }

    /*
     * Decodifica RTCM 1007/1008:
     * descriptor de antena y, en 1008, número de serie.
     */
    static bool decode_1007_1008(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1007 && type != 1008) return false;
        uint64_t stid = 0, ndesc = 0, setup = 0, nser = 0;
        if (!br.getU(12, stid) || !br.getU(8, ndesc)) {
            oss << " decode_error=invalid_1007_1008";
            return true;
        }
        std::string desc = read_ascii(br, static_cast<size_t>(ndesc));
        if (!br.getU(8, setup)) {
            oss << " decode_error=missing_setup_id";
            return true;
        }
        oss << " staid=" << stid << " ant=\"" << desc << "\" setup=" << setup;
        if (type == 1008) {
            if (!br.getU(8, nser)) {
                oss << " decode_error=missing_serial_length";
                return true;
            }
            std::string serial = read_ascii(br, static_cast<size_t>(nser));
            oss << " serial=\"" << serial << "\"";
        }
        return true;
    }

    /*
     * Decodifica RTCM 1033:
     * identifica antena y receptor (modelo, firmware y serial).
     */
    static bool decode_1033(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1033) return false;
        uint64_t stid = 0, n = 0, setup = 0;
        if (!br.getU(12, stid) || !br.getU(8, n)) {
            oss << " decode_error=invalid_1033";
            return true;
        }
        std::string ant_desc = read_ascii(br, static_cast<size_t>(n));
        uint64_t n_ant_sn = 0, n_rx_type = 0, n_rx_fw = 0, n_rx_sn = 0;
        if (!br.getU(8, setup) || !br.getU(8, n_ant_sn)) {
            oss << " decode_error=invalid_1033_fields";
            return true;
        }
        std::string ant_sn = read_ascii(br, static_cast<size_t>(n_ant_sn));
        if (!br.getU(8, n_rx_type)) {
            oss << " decode_error=invalid_1033_rx_type";
            return true;
        }
        std::string rx_type = read_ascii(br, static_cast<size_t>(n_rx_type));
        if (!br.getU(8, n_rx_fw)) {
            oss << " decode_error=invalid_1033_rx_fw";
            return true;
        }
        std::string rx_fw = read_ascii(br, static_cast<size_t>(n_rx_fw));
        if (!br.getU(8, n_rx_sn)) {
            oss << " decode_error=invalid_1033_rx_sn";
            return true;
        }
        std::string rx_sn = read_ascii(br, static_cast<size_t>(n_rx_sn));
        oss << " staid=" << stid
            << " ant=\"" << ant_desc << "\""
            << " ant_sn=\"" << ant_sn << "\""
            << " rx=\"" << rx_type << "\""
            << " fw=\"" << rx_fw << "\""
            << " rx_sn=\"" << rx_sn << "\""
            << " setup=" << setup;
        return true;
    }

    /*
     * Decodifica RTCM 1013:
     * parámetros de sistema y anuncios de periodicidad de mensajes.
     */
    static bool decode_1013(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1013) return false;
        uint64_t stid = 0, mjd = 0, sod = 0, nmsg = 0;
        if (!br.getU(12, stid) || !br.getU(16, mjd) || !br.getU(17, sod) || !br.getU(5, nmsg)) {
            oss << " decode_error=invalid_1013";
            return true;
        }
        oss << " staid=" << stid << " mjd=" << mjd << " sod=" << sod << " announcements=" << nmsg;
        for (uint64_t i = 0; i < nmsg; ++i) {
            uint64_t msg = 0, sync = 0, interval = 0;
            if (!br.getU(12, msg) || !br.getU(1, sync) || !br.getU(16, interval)) {
                oss << " decode_error=truncated_1013_announcements";
                return true;
            }
        }
        return true;
    }

    /*
     * Decodifica RTCM 1019 (efemérides GPS) en campos clave resumidos.
     */
    static bool decode_1019(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1019) return false;
        uint64_t sat = 0, week = 0, ura = 0, code = 0;
        int64_t idot = 0;
        uint64_t iode = 0;
        if (!br.getU(6, sat) || !br.getU(10, week) || !br.getU(4, ura) ||
            !br.getU(2, code) || !br.getS(14, idot) || !br.getU(8, iode)) {
            oss << " decode_error=invalid_1019";
            return true;
        }
        oss << " sat=G" << sat
            << " week=" << week
            << " ura=" << ura
            << " codeL2=" << code
            << " iode=" << iode
            << " idot_raw=" << idot;
        return true;
    }

    /*
     * Decodifica RTCM 1020 (efemérides GLONASS) en campos clave resumidos.
     */
    static bool decode_1020(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1020) return false;
        uint64_t sat = 0, freq = 0, tk_h = 0, tk_m = 0, tk_s = 0;
        if (!br.getU(6, sat) || !br.getU(5, freq) || !br.getU(5, tk_h) ||
            !br.getU(6, tk_m) || !br.getU(1, tk_s)) {
            oss << " decode_error=invalid_1020";
            return true;
        }
        oss << " sat=R" << sat
            << " freq_ch=" << static_cast<int>(freq) - 7
            << " tk=" << tk_h << ":" << tk_m << ":" << (tk_s * 30);
        return true;
    }

    /*
     * Decodifica RTCM 1045/1046 (efemérides Galileo) en campos clave.
     */
    static bool decode_1045_1046(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1045 && type != 1046) return false;
        uint64_t sat = 0, week = 0, iode = 0, sisa = 0;
        if (!br.getU(6, sat) || !br.getU(12, week) || !br.getU(10, iode) || !br.getU(8, sisa)) {
            oss << " decode_error=invalid_1045_1046";
            return true;
        }
        oss << " sat=E" << sat << " week=" << week << " iode=" << iode << " sisa=" << sisa;
        return true;
    }

    /*
     * Decodifica RTCM 1230:
     * sesgos GLONASS code-phase (información de calibración inter-frecuencia).
     */
    static bool decode_1230(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1230) return false;
        uint64_t stid = 0, ind = 0, mask = 0;
        if (!br.getU(12, stid) || !br.getU(1, ind) || !br.getU(3, mask)) {
            oss << " decode_error=invalid_1230";
            return true;
        }
        oss << " staid=" << stid << " code_phase_mask=" << mask << " ext=" << ind;
        return true;
    }

    /*
     * Decodifica familia MSM (107x..112x, niveles 4-7).
     * Extrae cabecera, máscaras sat/señal/celda y recorre bloques de datos
     * para validar consistencia y reportar conteos observables.
     */
    static bool decode_msm(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type < 1071 || type > 1127) return false;
        const int mod = type % 10;
        if (mod < 4 || mod > 7) return false;

        uint64_t stid = 0, tow = 0, mm = 0, iods = 0, clk_steer = 0, ext_clk = 0, smooth = 0, smooth_int = 0;
        uint64_t sat_mask = 0, sig_mask = 0;
        if (!br.getU(12, stid) || !br.getU(30, tow) || !br.getU(1, mm) || !br.getU(3, iods) ||
            !br.skip(7) || !br.getU(2, clk_steer) || !br.getU(2, ext_clk) ||
            !br.getU(1, smooth) || !br.getU(3, smooth_int) || !br.getU(64, sat_mask) ||
            !br.getU(32, sig_mask)) {
            oss << " decode_error=invalid_msm_header";
            return true;
        }

        const int nsat = popcount64(sat_mask);
        const int nsig = popcount32(static_cast<uint32_t>(sig_mask));
        const int max_cells = nsat * nsig;
        int ncells = 0;
        for (int i = 0; i < max_cells; ++i) {
            uint64_t bit = 0;
            if (!br.getU(1, bit)) {
                oss << " decode_error=invalid_msm_cell_mask";
                return true;
            }
            ncells += static_cast<int>(bit);
        }

        for (int i = 0; i < nsat; ++i) {
            uint64_t u = 0;
            int64_t s = 0;
            if (!br.getU(8, u)) {
                oss << " decode_error=truncated_msm_sat_data";
                return true;
            }
            if (mod == 5 || mod == 7) {
                if (!br.getU(4, u)) {
                    oss << " decode_error=truncated_msm_sat_ext_info";
                    return true;
                }
            }
            if (!br.getU(10, u)) {
                oss << " decode_error=truncated_msm_rough_range_mod";
                return true;
            }
            if (mod == 5 || mod == 7) {
                if (!br.getS(14, s)) {
                    oss << " decode_error=truncated_msm_rate";
                    return true;
                }
            }
        }

        for (int i = 0; i < ncells; ++i) {
            uint64_t u = 0;
            int64_t s = 0;
            const bool hi_res = (mod == 6 || mod == 7);
            const int pr_bits = hi_res ? 20 : 15;
            const int cp_bits = hi_res ? 24 : 22;
            const int lock_bits = hi_res ? 10 : 4;
            const int cnr_bits = hi_res ? 10 : 6;
            if (!br.getS(pr_bits, s) || !br.getS(cp_bits, s) || !br.getU(lock_bits, u) ||
                !br.getU(1, u) || !br.getU(cnr_bits, u)) {
                oss << " decode_error=truncated_msm_cell_data";
                return true;
            }
            if (mod == 5 || mod == 7) {
                if (!br.getS(15, s)) {
                    oss << " decode_error=truncated_msm_phase_rate";
                    return true;
                }
            }
        }

        oss << " staid=" << stid
            << " tow=" << tow
            << " nsat=" << nsat
            << " nsig=" << nsig
            << " ncells=" << ncells
            << " multiple=" << mm
            << " smoothing=" << smooth
            << " smooth_int=" << smooth_int
            << " clk=" << clk_steer
            << " extclk=" << ext_clk;
        return true;
    }
};

class RtcmStreamFramer {
public:
    /*
     * Ingresa bytes arbitrarios del stream (NATS puede fragmentar mensajes).
     * Re-sincroniza por preámbulo 0xD3, reconstruye tramas completas,
     * valida CRC24Q y envía payload válido al decodificador.
     */
    void push(const char* data, int len, RtcmMessageDecoder& decoder, const std::string& subject)
    {
        if (len <= 0) return;
        buf_.insert(buf_.end(), data, data + len);

        size_t i = 0;
        while (true) {
            while (i < buf_.size() && static_cast<uint8_t>(buf_[i]) != 0xD3) ++i;
            if (i + 3 > buf_.size()) break;

            const uint8_t* p = reinterpret_cast<const uint8_t*>(buf_.data() + i);
            const size_t payload_len = ((static_cast<size_t>(p[1]) & 0x03U) << 8) | p[2];
            const size_t frame_len = 3 + payload_len + 3;
            if (i + frame_len > buf_.size()) break;

            const uint32_t got_crc =
                (static_cast<uint32_t>(p[3 + payload_len]) << 16) |
                (static_cast<uint32_t>(p[3 + payload_len + 1]) << 8) |
                static_cast<uint32_t>(p[3 + payload_len + 2]);
            const uint32_t calc_crc = crc24q(p, 3 + payload_len);

            if (got_crc == calc_crc) {
                ++frames_ok_;
                std::string summary = decoder.decode(p + 3, payload_len);
                print_line(subject, summary);
                i += frame_len;
            } else {
                ++frames_bad_crc_;
                ++i;
            }
        }

        if (i > 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(i));
        if (buf_.size() > max_buffer_) {
            buf_.erase(buf_.begin(), buf_.end() - static_cast<std::ptrdiff_t>(max_buffer_));
        }
    }

    /* Total de tramas RTCM válidas (CRC correcto). */
    uint64_t frames_ok() const { return frames_ok_; }
    /* Total de candidatos descartados por CRC incorrecto. */
    uint64_t frames_bad_crc() const { return frames_bad_crc_; }

private:
    /*
     * Emite una línea atómica en consola con timestamp + subject + resumen.
     * Usa mutex estático para evitar mezcla de texto entre callbacks.
     */
    static void print_line(const std::string& subject, const std::string& summary)
    {
        static std::mutex out_mtx;
        std::lock_guard<std::mutex> lk(out_mtx);
        std::cout << "[" << now_hms() << "] " << subject << " " << summary << "\n";
    }

    static constexpr size_t max_buffer_ = 1024 * 1024;
    std::vector<char> buf_;
    uint64_t frames_ok_ = 0;
    uint64_t frames_bad_crc_ = 0;
};

struct AppCtx {
    std::string subject;
    RtcmMessageDecoder decoder;
    RtcmStreamFramer framer;
};

/*
 * Callback de suscripción NATS.
 * Toma payload binario recibido y lo pasa al framer RTCM del contexto.
 */
static void on_nats_msg(natsConnection*, natsSubscription*, natsMsg* msg, void* closure)
{
    AppCtx* ctx = static_cast<AppCtx*>(closure);
    const char* data = natsMsg_GetData(msg);
    const int len = natsMsg_GetDataLength(msg);
    ctx->framer.push(data, len, ctx->decoder, ctx->subject);
    natsMsg_Destroy(msg);
}

/*
 * Divide una lista CSV simple de servidores NATS.
 * Ignora entradas vacías para tolerar comas repetidas.
 */
static std::vector<std::string> split_csv(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        std::string item = s.substr(start, comma - start);
        if (!item.empty()) out.push_back(item);
        start = comma + 1;
    }
    return out;
}

/*
 * Punto de entrada:
 * - valida argumentos
 * - configura y conecta NATS
 * - se suscribe al subject NTRIP.<mountpoint>
 * - procesa flujo hasta Ctrl+C
 * - reporta estadísticas finales.
 */
int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: ./rtcm_decoder_nats <mountpoint> [nats_servers_csv]\n";
        std::cerr << "Example: ./rtcm_decoder_nats ABAN3M nats://127.0.0.1:4222,nats://127.0.0.1:4223\n";
        return 1;
    }

    const std::string mountpoint = argv[1];
    const std::string subject = "NTRIP." + mountpoint;
    const std::string servers = (argc == 3)
        ? argv[2]
        : "nats://127.0.0.1:4222,nats://127.0.0.1:4223";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    natsOptions* opts = nullptr;
    natsConnection* nc = nullptr;
    natsSubscription* sub = nullptr;

    natsStatus s = natsOptions_Create(&opts);
    if (s != NATS_OK) {
        std::cerr << "natsOptions_Create failed: " << natsStatus_GetText(s) << "\n";
        return 1;
    }

    std::vector<std::string> server_list = split_csv(servers);
    if (server_list.empty()) {
        std::cerr << "No valid NATS servers provided.\n";
        natsOptions_Destroy(opts);
        return 1;
    }
    std::vector<const char*> server_ptrs;
    server_ptrs.reserve(server_list.size());
    for (const auto& sv : server_list) server_ptrs.push_back(sv.c_str());
    s = natsOptions_SetServers(opts, server_ptrs.data(), static_cast<int>(server_ptrs.size()));
    if (s != NATS_OK) {
        std::cerr << "natsOptions_SetServers failed: " << natsStatus_GetText(s) << "\n";
        natsOptions_Destroy(opts);
        return 1;
    }

    s = natsConnection_Connect(&nc, opts);
    if (s != NATS_OK) {
        std::cerr << "NATS connect failed: " << natsStatus_GetText(s) << "\n";
        natsOptions_Destroy(opts);
        return 1;
    }

    AppCtx ctx;
    ctx.subject = subject;
    s = natsConnection_Subscribe(&sub, nc, subject.c_str(), on_nats_msg, &ctx);
    if (s != NATS_OK) {
        std::cerr << "Subscribe failed on " << subject << ": " << natsStatus_GetText(s) << "\n";
        natsConnection_Destroy(nc);
        natsOptions_Destroy(opts);
        return 1;
    }

    std::cout << "Subscribed to " << subject << "\n";
    std::cout << "Press Ctrl+C to stop.\n";

    while (g_running.load()) {
        nats_Sleep(250);
    }

    std::cout << "Stopping... frames_ok=" << ctx.framer.frames_ok()
              << " bad_crc=" << ctx.framer.frames_bad_crc() << "\n";

    natsSubscription_Destroy(sub);
    natsConnection_Destroy(nc);
    natsOptions_Destroy(opts);
    nats_Close();
    return 0;
}
