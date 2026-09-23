// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include <set>
#include "Shapes.h"

namespace Sleeve::Scanner {

struct ScanStats {
  size_t files_probed {0};
  size_t duplicates_collapsed {0};
  size_t ppc64le_skipped {0};
  size_t x86_64_skipped {0};
  size_t other_skipped {0};
  size_t dirs_pruned {0};
  double elapsed_seconds {0.0};
};

struct ForeignLauncher {
  std::string name;
  std::string path;
  std::string target_exe;
  std::string rootfs;
};

struct ScanResult {
  std::vector<Shapes::AppCandidate> apps;
  std::vector<ForeignLauncher> foreign_launchers;
  ScanStats stats;

  // The roots that were actually walked, and whether the caller named them.
  std::vector<std::string> searched_dirs;
  bool explicit_dirs {false};
};

struct ScanOptions {
  std::vector<std::string> search_dirs;
  std::set<std::string> prune_names {".git", "node_modules", "RootFS", "build", "build-powerarm", "build-powerarm-tests", "cache", ".cache", "tmp"};
  int max_depth {4};
  bool scan_overlays {true};
  bool scan_bin_dir {true};
};

// Origin string used for apps found in a rootfs overlay rather than a searched directory.
std::string OverlayOrigin(const std::string& overlayName);

// Collapses candidates that resolve to the same executable, keeping the one whose name
// best describes that executable. Returns the number of rows dropped.
size_t DedupeByResolvedTarget(std::vector<Shapes::AppCandidate>& apps);

ScanResult RunScan(const ScanOptions& options);

} // namespace Sleeve::Scanner
