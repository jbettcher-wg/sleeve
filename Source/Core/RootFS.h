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

  // ELF e_machine of the rootfs's own userspace, 0 when it could not be established.
  uint16_t elf_machine {0};
  // True when elf_machine was read and matched the active backend's guest architecture.
  bool arch_verified {false};

  std::vector<std::string> used_by_apps;
};

struct DiscoveryResult {
  std::string env_rootfs;
  std::string config_default_rootfs;
  std::vector<RootFSInfo> rootfses;
};

struct ResolvedRootFS {
  bool ok {false};
  std::string name;
  std::string path;
  std::string error;
};

DiscoveryResult DiscoverRootFSes();

// Reads the ELF machine type of a rootfs tree's own userspace (/usr/bin/env, /bin/sh, ...),
// following symlinks the way the guest would: an absolute link target is re-rooted at the
// rootfs, so a host binary is never mistaken for a guest one. Returns 0 when it cannot be
// established cheaply (a squashfs image, an empty tree, an unreadable file).
uint16_t DetectRootFSMachine(const std::string& rootfsPath);

// Resolves a name or path to a rootfs usable by the active backend. Fails rather than
// silently handing back something the emulator would fall off of into host binaries.
ResolvedRootFS ResolveRootFSName(const std::string& nameOrPath);

std::string FormatBytes(uint64_t bytes);

} // namespace Sleeve::RootFS
