// SPDX-License-Identifier: MIT
#pragma once

#include "Process.h"

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

// --------------------------------------------------------------------------
// Is there a usable guest here at all?
//
// A missing rootfs is the same kind of problem as a missing library: an unmet
// precondition that can be named exactly and fixed by one command. It is worth keeping
// apart from the others, because the answers differ -- nothing here builds a rootfs
// itself; POWERarmRootFSFetcher does, and it is driven, not reimplemented.
// --------------------------------------------------------------------------

enum class Readiness {
  Ok,                // the right architecture, with an overlay guest pacman can write to
  Missing,           // no tree at that name or path
  WrongArch,         // a tree whose own userspace is not the guest's machine
  NoOverlay,         // a usable base with no per-user writable layer
  OverlayIncomplete, // an overlay that exists but has no guest pacman in it
};

struct ReadinessReport {
  Readiness state {Readiness::Missing};
  std::string requested;    // the name or path that was asked about
  std::string rootfs_path;  // "" when nothing was found
  std::string overlay_path;
  uint16_t elf_machine {0};
  bool guest_pacman {false};
  std::string summary;      // what is wrong, in one sentence
  std::string fix_hint;     // the command a person should run, "" when there is nothing to run
  // Whether sleeve should offer to run the fix. A missing or wrong-architecture rootfs
  // is worth building; an incomplete overlay is a repair, and repairs are the person's
  // call because the fetcher has to be told which manifest built the base.
  bool auto_fixable {false};
};

// `nameOrPath` empty means "whatever this backend would use by default".
ReadinessReport CheckReadiness(const std::string& nameOrPath);

struct ProvisionOptions {
  // The rootfs name, which is also the directory under <datadir>/RootFS. Empty lets the
  // fetcher use the manifest's own default (ArchLinuxARM-vk for the vk manifest).
  std::string name;
  std::string manifest {"vk"};
  std::string dest;   // "" leaves the destination to the fetcher
  bool force {false}; // replace a non-empty destination
  bool dry_run {false};
  bool set_default {true};
  // Passed straight through as repeated --mirror, in order. Empty leaves the fetcher on
  // its own defaults, which is the only sensible default for sleeve to have.
  std::vector<std::string> mirrors;
};

// ProvisionOptions with the mirror list filled in from the settings file.
ProvisionOptions ProvisionOptionsFromSettings();

// `POWERarmRootFSFetcher build ...`: the base, the per-user overlay and guest pacman, and
// the config write. Pure, so what would be run can be shown and tested without running
// it. -y is always passed: sleeve has already asked, and the fetcher must not then sit
// waiting on a prompt nobody can see.
Process::ProcessOptions BuildProvisionCommand(const ProvisionOptions& options);

// `POWERarmRootFSFetcher overlay ...`: adds the writable layer to a base that already
// exists, which is the repair for an overlay that a half-finished run left behind.
Process::ProcessOptions BuildOverlayCommand(const ProvisionOptions& options);

// Runs BuildProvisionCommand. Downloads roughly 860 MB and writes roughly 1.9 GB, so
// every caller gets consent first.
Process::RunOutcome Provision(const ProvisionOptions& options, const Process::Runner& run = {});

// Runs BuildOverlayCommand.
Process::RunOutcome ProvisionOverlay(const ProvisionOptions& options, const Process::Runner& run = {});

} // namespace Sleeve::RootFS
