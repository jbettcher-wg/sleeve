// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace Sleeve::ElfInspect {

enum class FileKind {
  Unknown,
  NotElf,
  AArch64_Exec,
  AArch64_Dyn,      // PIE executable or shared library
  Foreign_PPC64LE,
  Foreign_X86_64,
  Foreign_Other,
  AppImage,
  DebArchive,
  TarArchive,
};

struct ProbeResult {
  FileKind kind {FileKind::Unknown};
  uint16_t machine {0};
  uint16_t type {0};
  bool has_interp {false};
  bool is_appimage {false};
  uint64_t appimage_offset {0};
};

struct ElfDetails {
  ProbeResult probe;
  std::string interpreter;
  std::vector<std::string> needed_libs;
  // DT_SONAME, "" when the object does not carry one.
  std::string soname;
  // DT_RPATH and DT_RUNPATH, already split on ':'. Tokens ($ORIGIN, $LIB, $PLATFORM)
  // are left as they appear; expanding them needs the path of the object doing the
  // loading, which is the caller's business.
  std::vector<std::string> rpath;
  std::vector<std::string> runpath;
  uint64_t file_size {0};
};

// Fast 64-byte probe without logging or full parsing
ProbeResult ProbeFile(const std::string& path);

// Checks if a probed binary matches the active backend's target architecture
bool IsTargetBinary(const ProbeResult& probe);

// Detailed inspection for binaries of the active backend's guest architecture
// (reads PT_INTERP, DT_NEEDED, DT_SONAME, DT_RPATH, DT_RUNPATH). The parse itself is
// plain ELF64.
std::optional<ElfDetails> InspectTarget(const std::string& path);

// The same parse without the architecture gate. The dependency walk needs it: a library
// found in the rootfs is read for its own DT_NEEDED whatever its e_machine turns out to
// be, and reporting the machine is the point when it turns out to be the wrong one.
std::optional<ElfDetails> InspectElf64(const std::string& path);

} // namespace Sleeve::ElfInspect
