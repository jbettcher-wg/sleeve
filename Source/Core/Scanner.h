// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include <set>
#include "Shapes.h"

namespace Sleeve::Scanner {

struct ScanStats {
  size_t files_probed {0};
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
};

struct ScanOptions {
  std::vector<std::string> search_dirs;
  std::set<std::string> prune_names {".git", "node_modules", "RootFS", "build", "build-powerarm", "build-powerarm-tests", "cache", ".cache", "tmp"};
  int max_depth {4};
  bool scan_overlays {true};
  bool scan_bin_dir {true};
};

ScanResult RunScan(const ScanOptions& options);

} // namespace Sleeve::Scanner
