// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace Sleeve::RootFS {

struct RootFSInfo {
  std::string name;
  std::string base_path;
  uint64_t base_size_bytes {0};

  bool has_overlay {false};
  std::string overlay_path;
  uint64_t overlay_size_bytes {0};

  bool has_pacman {false};
  size_t package_count {0};

  bool is_env_default {false};
  bool is_config_default {false};

  std::vector<std::string> used_by_apps;
};

struct DiscoveryResult {
  std::string env_rootfs;
  std::string config_default_rootfs;
  std::vector<RootFSInfo> rootfses;
};

DiscoveryResult DiscoverRootFSes();

std::string FormatBytes(uint64_t bytes);

} // namespace Sleeve::RootFS
