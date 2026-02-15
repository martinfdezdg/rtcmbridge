#pragma once

#include "rtcmbridge/core/ntrip_client.hpp"

#include <optional>
#include <string>
#include <vector>

namespace rtcmbridge {

std::vector<NtripConfig> load_all_ntrip_configs_from_mountpoints(const std::string& file_path);

std::optional<NtripConfig> load_ntrip_config_from_mountpoints(
    const std::string& file_path,
    const std::string& mountpoint);

}  // namespace rtcmbridge
