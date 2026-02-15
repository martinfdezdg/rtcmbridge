#include "rtcmbridge/core/mountpoint_config.hpp"

#include <fstream>
#include <cctype>
#include <string>
#include <vector>

namespace rtcmbridge {

namespace {

/* Elimina espacios al inicio/fin para tolerar líneas con formato irregular. */
std::string trim_copy(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

/*
 * Parseador de una línea mountpoint.conf en formato:
 * user:pass host:port/mountpoint
 */
bool parse_line(const std::string& line, NtripConfig& cfg)
{
    const std::string t = trim_copy(line);
    if (t.empty() || t[0] == '#') return false;

    const size_t sp = t.find(' ');
    const size_t cp = t.find(':');
    if (sp == std::string::npos || cp == std::string::npos || cp >= sp) return false;

    const size_t slash = t.find('/', sp + 1U);
    if (slash == std::string::npos) return false;

    const std::string hostport = t.substr(sp + 1U, slash - sp - 1U);
    const size_t colon = hostport.find(':');
    if (colon == std::string::npos) return false;

    cfg.user = t.substr(0, cp);
    cfg.pass = t.substr(cp + 1U, sp - cp - 1U);
    cfg.host = hostport.substr(0, colon);
    try {
        cfg.port = std::stoi(hostport.substr(colon + 1U));
    } catch (...) {
        return false;
    }
    cfg.mountpoint = trim_copy(t.substr(slash + 1U));
    return !cfg.user.empty() && !cfg.host.empty() && !cfg.mountpoint.empty();
}

}  // namespace

/* Carga todos los mountpoints válidos definidos en el fichero de configuración. */
std::vector<NtripConfig> load_all_ntrip_configs_from_mountpoints(const std::string& file_path)
{
    std::vector<NtripConfig> out;
    std::ifstream in(file_path);
    if (!in) return out;

    std::string line;
    while (std::getline(in, line)) {
        NtripConfig cfg;
        if (!parse_line(line, cfg)) continue;
        out.push_back(std::move(cfg));
    }
    return out;
}

/* Busca un mountpoint concreto en el fichero de configuración. */
std::optional<NtripConfig> load_ntrip_config_from_mountpoints(
    const std::string& file_path,
    const std::string& mountpoint)
{
    for (auto& cfg : load_all_ntrip_configs_from_mountpoints(file_path)) {
        if (cfg.mountpoint == mountpoint) return cfg;
    }

    return std::nullopt;
}

}  // namespace rtcmbridge
