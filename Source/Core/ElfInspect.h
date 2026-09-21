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
  uint64_t file_size {0};
};

// Fast 64-byte probe without logging or full parsing
ProbeResult ProbeFile(const std::string& path);

// Detailed inspection for AArch64 binaries (reads PT_INTERP, DT_NEEDED)
std::optional<ElfDetails> InspectAArch64(const std::string& path);

} // namespace Sleeve::ElfInspect
