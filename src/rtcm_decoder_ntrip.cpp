/*
 * RTCM3 Decoder over direct NTRIP caster connection
 * Connects to one mountpoint and decodes RTCM frames in real time.
 */

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using boost::asio::ip::tcp;
namespace asio = boost::asio;

static std::atomic<bool> g_running{true};

/*
 * Handler de señales POSIX (SIGINT/SIGTERM).
 * Solo marca parada global para cerrar de forma controlada.
 */
static void on_signal(int) { g_running = false; }

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Codifica credenciales user:pass en Base64 para Authorization Basic.
 */
static std::string base64_encode(const std::string& in)
{
    std::string out;
    out.reserve((in.size() * 4) / 3 + 4);
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

/*
 * Calcula CRC24Q para validar integridad de tramas RTCM3.
 * Debe aplicarse sobre cabecera RTCM + payload (sin incluir CRC).
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
     * Inicializa lectura bit a bit sobre un buffer de bytes.
     */
    BitReader(const uint8_t* data, size_t bytes)
        : data_(data), total_bits_(bytes * 8), bitpos_(0) {}

    /*
     * Lee n bits sin signo (MSB-first) y avanza posición interna.
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
     * Lee n bits con signo en complemento a dos y avanza posición.
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
     * Salta n bits del stream, usado para campos reservados/no usados.
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

/* Cuenta satélites presentes en máscara MSM (64 bits). */
static int popcount64(uint64_t v) { return __builtin_popcountll(v); }
/* Cuenta señales presentes en máscara MSM (32 bits). */
static int popcount32(uint32_t v) { return __builtin_popcount(v); }

/* Devuelve timestamp local HH:MM:SS para líneas de log. */
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
 * Decodifica texto ASCII de longitud fija desde el payload RTCM.
 * Sustituye bytes no imprimibles por '.'.
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
     * Dispatcher principal por tipo RTCM:
     * lee los 12 bits de message type y llama al parser específico.
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
    static constexpr double kLightSpeed = 299792458.0;

    struct SigInfo {
        std::string label;
        double freq_hz = 0.0;
    };

    static char constellation_code(int type)
    {
        if (type >= 1070 && type < 1080) return 'G';
        if (type >= 1080 && type < 1090) return 'R';
        if (type >= 1090 && type < 1100) return 'E';
        if (type >= 1100 && type < 1110) return 'S';
        if (type >= 1110 && type < 1120) return 'J';
        if (type >= 1120 && type < 1130) return 'C';
        return '?';
    }

    static SigInfo signal_info(char c, int sig_id)
    {
        switch (c) {
            case 'G':  // GPS
                switch (sig_id) {
                    case 2: return {"L1", 1575.42e6};
                    case 3: return {"L1", 1575.42e6};
                    case 8: return {"L2", 1227.60e6};
                    case 9: return {"L2", 1227.60e6};
                    case 10: return {"L2", 1227.60e6};
                    case 15: return {"L5", 1176.45e6};
                    case 16: return {"L5", 1176.45e6};
                    case 17: return {"L5", 1176.45e6};
                    default: break;
                }
                break;
            case 'R':  // GLONASS (aprox. frecuencias nominales)
                switch (sig_id) {
                    case 2: return {"G1", 1602.0e6};
                    case 8: return {"G2", 1246.0e6};
                    case 14: return {"G3", 1202.025e6};
                    default: break;
                }
                break;
            case 'E':  // Galileo
                switch (sig_id) {
                    case 2: return {"E1", 1575.42e6};
                    case 8: return {"E6", 1278.75e6};
                    case 14: return {"E5b", 1207.14e6};
                    case 16: return {"E5a", 1176.45e6};
                    case 22: return {"E5", 1191.795e6};
                    default: break;
                }
                break;
            case 'C':  // BeiDou
                switch (sig_id) {
                    case 2: return {"B1", 1561.098e6};
                    case 8: return {"B3", 1268.52e6};
                    case 14: return {"B2", 1207.14e6};
                    case 16: return {"B2a", 1176.45e6};
                    case 22: return {"B2ab", 1191.795e6};
                    default: break;
                }
                break;
            case 'J':  // QZSS
                switch (sig_id) {
                    case 2: return {"L1", 1575.42e6};
                    case 8: return {"L2", 1227.60e6};
                    case 15: return {"L5", 1176.45e6};
                    case 22: return {"L6", 1278.75e6};
                    default: break;
                }
                break;
            default:
                break;
        }
        return {"SIG" + std::to_string(sig_id), 0.0};
    }

    static bool is_invalid_signed(int64_t v, int bits)
    {
        return v == -(1LL << (bits - 1));
    }

    static std::string fmt_prn(char c, int prn)
    {
        std::ostringstream oss;
        oss << c << std::setw(2) << std::setfill('0') << prn << std::setfill(' ');
        return oss.str();
    }

    static std::string fmt_double(double v, int prec)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(prec) << v;
        return oss.str();
    }

    /*
     * Diccionario de nombres legibles para tipos RTCM frecuentes.
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
     * RTCM 1005/1006: posición de estación (ECEF) e información ARP.
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
     * RTCM 1007/1008: descriptor de antena y serial.
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
     * RTCM 1033: identificación completa de antena + receptor.
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
     * RTCM 1013: anuncios de mensajes del sistema y sus intervalos.
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
     * RTCM 1019: efemérides GPS (resumen de campos principales).
     */
    static bool decode_1019(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type != 1019) return false;
        uint64_t sat = 0, week = 0, ura = 0, code = 0, iode = 0;
        int64_t idot = 0;
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
     * RTCM 1020: efemérides GLONASS (resumen operativo).
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
     * RTCM 1045/1046: efemérides Galileo (resumen operativo).
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
     * RTCM 1230: sesgos GLONASS code-phase.
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
     * Decodificación MSM genérica para niveles 4..7:
     * - cabecera MSM
     * - máscaras de satélites/señales/celdas
     * - bloques de observables por satélite y por celda
     */
    static bool decode_msm(int type, BitReader& br, std::ostringstream& oss)
    {
        if (type < 1071 || type > 1127) return false;
        const int mod = type % 10;
        if (mod < 4 || mod > 7) return false;
        const bool hi_res = (mod == 6 || mod == 7);
        const bool with_phase_rate = (mod == 5 || mod == 7);
        const char constel = constellation_code(type);

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

        std::vector<int> sat_ids;
        sat_ids.reserve(nsat);
        for (int i = 0; i < 64; ++i) {
            if ((sat_mask >> (63 - i)) & 1ULL) sat_ids.push_back(i + 1);
        }

        std::vector<int> sig_ids;
        sig_ids.reserve(nsig);
        for (int i = 0; i < 32; ++i) {
            if ((sig_mask >> (31 - i)) & 1ULL) sig_ids.push_back(i + 1);
        }

        std::vector<uint8_t> cell_present(static_cast<size_t>(max_cells), 0);
        int ncells = 0;
        for (int i = 0; i < max_cells; ++i) {
            uint64_t bit = 0;
            if (!br.getU(1, bit)) {
                oss << " decode_error=invalid_msm_cell_mask";
                return true;
            }
            cell_present[static_cast<size_t>(i)] = static_cast<uint8_t>(bit);
            ncells += static_cast<int>(bit);
        }

        std::vector<uint64_t> rough_ms_whole(static_cast<size_t>(nsat), 0);
        std::vector<uint64_t> rough_mod_1_1024(static_cast<size_t>(nsat), 0);

        for (int i = 0; i < nsat; ++i) {
            uint64_t u = 0;
            int64_t s = 0;
            if (!br.getU(8, u)) {
                oss << " decode_error=truncated_msm_sat_data";
                return true;
            }
            rough_ms_whole[static_cast<size_t>(i)] = u;
            if (with_phase_rate) {
                if (!br.getU(4, u)) {
                    oss << " decode_error=truncated_msm_sat_ext_info";
                    return true;
                }
            }
            if (!br.getU(10, u)) {
                oss << " decode_error=truncated_msm_rough_range_mod";
                return true;
            }
            rough_mod_1_1024[static_cast<size_t>(i)] = u;
            if (with_phase_rate) {
                if (!br.getS(14, s)) {
                    oss << " decode_error=truncated_msm_rate";
                    return true;
                }
            }
        }

        struct ObsCell {
            int sat_idx = -1;
            int sig_idx = -1;
            int64_t fine_pr = 0;
            int64_t fine_cp = 0;
            uint64_t lock = 0;
            uint64_t half = 0;
            uint64_t cnr_raw = 0;
        };
        std::vector<ObsCell> cells;
        cells.reserve(static_cast<size_t>(ncells));

        for (int si = 0; si < nsat; ++si) {
            for (int gi = 0; gi < nsig; ++gi) {
                const int flat = si * nsig + gi;
                if (!cell_present[static_cast<size_t>(flat)]) continue;

                ObsCell c;
                c.sat_idx = si;
                c.sig_idx = gi;
                uint64_t u = 0;
                int64_t s = 0;

                const int pr_bits = hi_res ? 20 : 15;
                const int cp_bits = hi_res ? 24 : 22;
                const int lock_bits = hi_res ? 10 : 4;
                const int cnr_bits = hi_res ? 10 : 6;

                if (!br.getS(pr_bits, s)) {
                    oss << " decode_error=truncated_msm_cell_data";
                    return true;
                }
                c.fine_pr = s;
                if (!br.getS(cp_bits, s)) {
                    oss << " decode_error=truncated_msm_cell_data";
                    return true;
                }
                c.fine_cp = s;
                if (!br.getU(lock_bits, u)) {
                    oss << " decode_error=truncated_msm_cell_data";
                    return true;
                }
                c.lock = u;
                if (!br.getU(1, u)) {
                    oss << " decode_error=truncated_msm_cell_data";
                    return true;
                }
                c.half = u;
                if (!br.getU(cnr_bits, u)) {
                    oss << " decode_error=truncated_msm_cell_data";
                    return true;
                }
                c.cnr_raw = u;

                if (with_phase_rate) {
                    if (!br.getS(15, s)) {
                        oss << " decode_error=truncated_msm_phase_rate";
                        return true;
                    }
                }

                cells.push_back(c);
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

        const double pr_scale_ms = hi_res ? std::ldexp(1.0, -29) : std::ldexp(1.0, -24);
        const double cp_scale_ms = hi_res ? std::ldexp(1.0, -31) : std::ldexp(1.0, -29);
        const double cnr_scale = hi_res ? 0.0625 : 1.0;
        const int pr_bits = hi_res ? 20 : 15;
        const int cp_bits = hi_res ? 24 : 22;

        int last_sat = -1;
        for (const auto& c : cells) {
            const int sat = sat_ids[static_cast<size_t>(c.sat_idx)];
            const int sig_id = sig_ids[static_cast<size_t>(c.sig_idx)];
            const SigInfo sig = signal_info(constel, sig_id);

            const uint64_t rough_whole = rough_ms_whole[static_cast<size_t>(c.sat_idx)];
            const uint64_t rough_mod = rough_mod_1_1024[static_cast<size_t>(c.sat_idx)];
            const bool rough_valid = (rough_whole != 255U);

            const double rough_ms = static_cast<double>(rough_whole) +
                                    static_cast<double>(rough_mod) / 1024.0;
            const bool pr_valid = rough_valid && !is_invalid_signed(c.fine_pr, pr_bits);
            const bool cp_valid = rough_valid && !is_invalid_signed(c.fine_cp, cp_bits);

            double pr_m = 0.0;
            if (pr_valid) {
                const double pr_ms = rough_ms + static_cast<double>(c.fine_pr) * pr_scale_ms;
                pr_m = pr_ms * 1e-3 * kLightSpeed;
            }

            double cp_cycles = 0.0;
            if (cp_valid && sig.freq_hz > 0.0) {
                const double cp_ms = rough_ms + static_cast<double>(c.fine_cp) * cp_scale_ms;
                const double cp_m = cp_ms * 1e-3 * kLightSpeed;
                cp_cycles = cp_m * sig.freq_hz / kLightSpeed;
            }

            const double snr_dbhz = static_cast<double>(c.cnr_raw) * cnr_scale;

            if (sat != last_sat) {
                oss << "\n  PRN " << fmt_prn(constel, sat) << ":";
                last_sat = sat;
            }

            oss << "\n    " << sig.label
                << "=" << (pr_valid ? fmt_double(pr_m, 3) : std::string("N/A")) << " m"
                << " / " << ((cp_valid && sig.freq_hz > 0.0) ? fmt_double(cp_cycles, 3) : std::string("N/A")) << " cycles"
                << " / SNR " << fmt_double(snr_dbhz, 2) << " dBHz"
                << " / Lock " << c.lock << (c.lock > 0 ? " (LOCKED)" : " (UNLOCKED)")
                << (c.half ? " / HalfCycle=1" : "");
        }
        return true;
    }
};

class RtcmStreamFramer {
public:
    /*
     * Reensambla tramas RTCM desde bytes de red:
     * detecta preámbulo 0xD3, calcula longitud, valida CRC y decodifica.
     */
    void push(const char* data, size_t len, RtcmMessageDecoder& decoder, const std::string& tag)
    {
        if (len == 0) return;
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
                print_line(tag, summary);
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

    /* Métrica acumulada de tramas válidas. */
    uint64_t frames_ok() const { return frames_ok_; }
    /* Métrica acumulada de tramas descartadas por CRC inválido. */
    uint64_t frames_bad_crc() const { return frames_bad_crc_; }

private:
    /*
     * Salida sincronizada por consola para evitar líneas mezcladas.
     */
    static void print_line(const std::string& tag, const std::string& summary)
    {
        static std::mutex out_mtx;
        std::lock_guard<std::mutex> lk(out_mtx);
        std::cout << "[" << now_hms() << "] " << tag << " " << summary << "\n";
    }

    static constexpr size_t max_buffer_ = 1024 * 1024;
    std::vector<char> buf_;
    uint64_t frames_ok_ = 0;
    uint64_t frames_bad_crc_ = 0;
};

class NtripDecoderClient : public std::enable_shared_from_this<NtripDecoderClient> {
public:
    /*
     * Construye cliente NTRIP directo con destino y credenciales.
     * Prepara buffers y etiqueta de logging.
     */
    NtripDecoderClient(asio::io_context& io,
                       std::string host,
                       int port,
                       std::string mountpoint,
                       std::string user,
                       std::string pass)
        : resolver_(io),
          socket_(io),
          host_(std::move(host)),
          port_(port),
          mountpoint_(std::move(mountpoint)),
          user_(std::move(user)),
          pass_(std::move(pass))
    {
        read_buf_.resize(8192);
        tag_ = host_ + ":" + std::to_string(port_) + "/" + mountpoint_;
    }

    /*
     * Inicia ciclo de conexión asíncrona al caster.
     */
    void start() { connect(); }

private:
    /*
     * Resuelve host + conecta socket TCP. Si falla, agenda reconexión.
     */
    void connect()
    {
        if (!g_running.load()) return;
        auto self = shared_from_this();
        resolver_.async_resolve(host_, std::to_string(port_),
            [this, self](const boost::system::error_code& ec, tcp::resolver::results_type eps) {
                if (ec) {
                    log("resolve failed: " + ec.message());
                    reconnect();
                    return;
                }
                asio::async_connect(socket_, eps,
                    [this, self](const boost::system::error_code& ec2, const tcp::endpoint&) {
                        if (ec2) {
                            log("connect failed: " + ec2.message());
                            reconnect();
                            return;
                        }
                        delay_s_ = 1;
                        response_header_.clear();
                        header_done_ = false;
                        send_request();
                    });
            });
    }

    /*
     * Envía GET NTRIP con headers necesarios y Basic Auth.
     * Si el envío es correcto, entra al bucle de lectura.
     */
    void send_request()
    {
        const std::string auth = user_ + ":" + pass_;
        const std::string encoded = base64_encode(auth);
        request_ =
            "GET /" + mountpoint_ + " HTTP/1.1\r\n"
            "Host: " + host_ + ":" + std::to_string(port_) + "\r\n"
            "User-Agent: RTCM-Direct-Decoder/1.0\r\n"
            "Ntrip-Version: Ntrip/2.0\r\n"
            "Authorization: Basic " + encoded + "\r\n"
            "Connection: keep-alive\r\n\r\n";

        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(request_),
            [this, self](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    log("request write failed: " + ec.message());
                    reconnect();
                    return;
                }
                read_loop();
            });
    }

    /*
     * Bucle asíncrono de lectura del socket TCP.
     * Cada bloque leído se procesa en on_bytes.
     */
    void read_loop()
    {
        if (!g_running.load()) return;
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(read_buf_),
            [this, self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) {
                    if (g_running.load()) {
                        log("read failed: " + ec.message());
                        reconnect();
                    }
                    return;
                }
                on_bytes(read_buf_.data(), n);
                read_loop();
            });
    }

    /*
     * Procesa bytes entrantes:
     * - primero acumula/parsing de cabecera HTTP/ICY
     * - después alimenta directamente el framer RTCM.
     */
    void on_bytes(const char* data, size_t n)
    {
        if (!header_done_) {
            response_header_.append(data, n);
            const size_t pos = response_header_.find("\r\n\r\n");
            if (pos == std::string::npos) return;

            std::string header = response_header_.substr(0, pos + 4);
            if (!is_ok_response(header)) {
                log("caster rejected request: " + first_line(header));
                reconnect();
                return;
            }

            log("stream started: " + first_line(header));
            header_done_ = true;
            const size_t body_start = pos + 4;
            if (body_start < response_header_.size()) {
                framer_.push(response_header_.data() + body_start,
                             response_header_.size() - body_start,
                             decoder_, tag_);
            }
            response_header_.clear();
            return;
        }

        framer_.push(data, n, decoder_, tag_);
    }

    /*
     * Valida línea de estado del caster. Acepta respuestas 200
     * en formato ICY o HTTP/1.0/1.1.
     */
    bool is_ok_response(const std::string& header) const
    {
        if (header.rfind("ICY 200", 0) == 0) return true;
        if (header.rfind("HTTP/1.0 200", 0) == 0) return true;
        if (header.rfind("HTTP/1.1 200", 0) == 0) return true;
        return false;
    }

    /*
     * Extrae primera línea de cabecera para logging.
     */
    static std::string first_line(const std::string& header)
    {
        const size_t p = header.find("\r\n");
        if (p == std::string::npos) return header;
        return header.substr(0, p);
    }

    /*
     * Reinicia socket y programa reconexión con backoff exponencial
     * acotado (máximo 30s).
     */
    void reconnect()
    {
        boost::system::error_code ignored;
        socket_.close(ignored);
        if (!g_running.load()) return;

        delay_s_ = std::min(delay_s_ * 2, 30);
        log("reconnect in " + std::to_string(delay_s_) + "s");
        timer_ = std::make_unique<asio::steady_timer>(
            socket_.get_executor(), std::chrono::seconds(delay_s_));
        auto self = shared_from_this();
        timer_->async_wait([this, self](const boost::system::error_code&) { connect(); });
    }

    /*
     * Logger del cliente con timestamp y etiqueta del mountpoint.
     */
    void log(const std::string& msg) const
    {
        static std::mutex out_mtx;
        std::lock_guard<std::mutex> lk(out_mtx);
        std::cout << "[" << now_hms() << "] [" << tag_ << "] " << msg << "\n";
    }

    tcp::resolver resolver_;
    tcp::socket socket_;
    std::vector<char> read_buf_;
    std::string request_;
    std::string response_header_;
    bool header_done_ = false;

    std::string host_;
    int port_;
    std::string mountpoint_;
    std::string user_;
    std::string pass_;
    std::string tag_;

    int delay_s_ = 1;
    std::unique_ptr<asio::steady_timer> timer_;

    RtcmMessageDecoder decoder_;
    RtcmStreamFramer framer_;
};

/*
 * Punto de entrada:
 * recibe host/port/mountpoint/user/pass, inicia cliente asíncrono
 * y mantiene proceso vivo hasta señal de parada.
 */
int main(int argc, char* argv[])
{
    if (argc != 6) {
        std::cerr << "Usage: ./rtcm_decoder_ntrip <host> <port> <mountpoint> <user> <pass>\n";
        std::cerr << "Example: ./rtcm_decoder_ntrip 192.148.213.42 2102 ABAN3M martin-gnss Martin-gnss1\n";
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const std::string host = argv[1];
    const int port = std::stoi(argv[2]);
    const std::string mountpoint = argv[3];
    const std::string user = argv[4];
    const std::string pass = argv[5];

    asio::io_context io;
    auto client = std::make_shared<NtripDecoderClient>(io, host, port, mountpoint, user, pass);
    client->start();

    std::thread io_thread([&io]() { io.run(); });

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    io.stop();
    io_thread.join();
    return 0;
}
