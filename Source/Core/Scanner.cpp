// SPDX-License-Identifier: MIT
#include "Scanner.h"
#include "ElfInspect.h"
#include "Paths.h"
#include "Shapes.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <map>
#include <algorithm>

namespace Sleeve::Scanner {

namespace fs = std::filesystem;

static bool IsPruned(const std::string& name, const std::set<std::string>& prune_names) {
  if (name.empty()) return false;
  if (name[0] == '.') return true;
  if (name == "Trash") return true;
  if (prune_names.count(name)) return true;
  if (name.rfind("build", 0) == 0) return true;
  return false;
}

static void ScanDirectoryRecursive(const std::string& rootDir, const ScanOptions& options,
                                   std::map<std::string, std::vector<std::string>>& dirToAArch64Binaries,
                                   std::vector<std::pair<std::string, ElfInspect::ElfDetails>>& loneBinaries,
                                   ScanStats& stats) {
  std::error_code ec;
  if (!fs::exists(rootDir, ec) || !fs::is_directory(rootDir, ec)) {
    return;
  }

  auto iter = fs::recursive_directory_iterator(rootDir, fs::directory_options::skip_permission_denied, ec);
  auto end = fs::recursive_directory_iterator();

  while (iter != end) {
    if (ec) {
      iter.increment(ec);
      continue;
    }

    if (iter.depth() > options.max_depth) {
      iter.pop();
      continue;
    }

    const auto& entry = *iter;
    std::string filename = entry.path().filename().string();

    if (entry.is_directory(ec)) {
      if (IsPruned(filename, options.prune_names) || (iter.depth() > 0 && fs::exists(entry.path() / ".git", ec))) {
        stats.dirs_pruned++;
        iter.disable_recursion_pending();
      }
      iter.increment(ec);
      continue;
    }

    if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) {
      iter.increment(ec);
      continue;
    }

    auto perms = entry.status(ec).permissions();
    bool is_exec = ((perms & fs::perms::owner_exec) != fs::perms::none) ||
                   ((perms & fs::perms::group_exec) != fs::perms::none) ||
                   ((perms & fs::perms::others_exec) != fs::perms::none);

    bool is_archive = (filename.size() > 7 && filename.rfind(".tar.gz") == filename.size() - 7) ||
                      (filename.size() > 7 && filename.rfind(".tar.xz") == filename.size() - 7) ||
                      (filename.size() > 8 && filename.rfind(".tar.zst") == filename.size() - 8) ||
                      (filename.size() > 4 && filename.rfind(".deb") == filename.size() - 4) ||
                      (filename.size() > 9 && filename.rfind(".AppImage") == filename.size() - 9);

    if (!is_exec && !is_archive) {
      iter.increment(ec);
      continue;
    }

    // Probe file
    std::string pathStr = entry.path().string();
    stats.files_probed++;

    auto probe = ElfInspect::ProbeFile(pathStr);
    switch (probe.kind) {
      case ElfInspect::FileKind::Foreign_PPC64LE:
        stats.ppc64le_skipped++;
        break;
      case ElfInspect::FileKind::Foreign_X86_64:
        stats.x86_64_skipped++;
        break;
      case ElfInspect::FileKind::Foreign_Other:
      case ElfInspect::FileKind::NotElf:
      case ElfInspect::FileKind::Unknown:
        stats.other_skipped++;
        break;
      case ElfInspect::FileKind::AArch64_Exec:
      case ElfInspect::FileKind::AArch64_Dyn: {
        auto details = ElfInspect::InspectAArch64(pathStr);
        if (details) {
          // If it has an interpreter or is an executable or standalone runtime
          if (details->probe.has_interp || details->probe.type == 2 /* ET_EXEC */ || details->file_size > 20 * 1024 * 1024) {
            std::string parentDir = entry.path().parent_path().string();
            dirToAArch64Binaries[parentDir].push_back(pathStr);
            loneBinaries.push_back({pathStr, *details});
          }
        }
        break;
      }
      default:
        break;
    }

    iter.increment(ec);
  }
}

static void ScanOverlays(const ScanOptions& options, std::vector<Shapes::AppCandidate>& apps, ScanStats& stats) {
  std::string dataDir = Paths::GetDataDir();
  std::string rootfsDir = dataDir + "/RootFS";

  std::error_code ec;
  if (!fs::exists(rootfsDir, ec)) return;

  for (const auto& entry : fs::directory_iterator(rootfsDir, ec)) {
    if (!entry.is_directory(ec)) continue;
    std::string dirName = entry.path().filename().string();
    if (dirName.rfind("-overlay") != std::string::npos) {
      std::string baseName = dirName.substr(0, dirName.rfind("-overlay"));
      std::string pacmanLocal = entry.path().string() + "/var/lib/pacman/local";
      if (fs::exists(pacmanLocal, ec)) {
        for (const auto& pkgEntry : fs::directory_iterator(pacmanLocal, ec)) {
          if (!pkgEntry.is_directory(ec)) continue;
          std::string descPath = pkgEntry.path().string() + "/desc";
          if (fs::exists(descPath, ec)) {
            auto cand = Shapes::DetectPacmanPackage(entry.path().string(), descPath);
            if (cand) {
              cand->rootfs_base = rootfsDir + "/" + baseName;
              apps.push_back(*cand);
            }
          }
        }
      }
    }
  }
}

static void ScanBinDirectory(std::vector<ForeignLauncher>& foreignLaunchers) {
  const char* home = std::getenv("HOME");
  if (!home) return;

  std::string binDir = std::string(home) + "/.local/bin";
  std::error_code ec;
  if (!fs::exists(binDir, ec)) return;

  for (const auto& entry : fs::directory_iterator(binDir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string path = entry.path().string();
    std::string name = entry.path().filename().string();

    std::ifstream f(path);
    if (!f.is_open()) continue;

    std::string line;
    bool isManaged = false;
    std::string execLine;
    std::string rootfs;

    while (std::getline(f, line)) {
      if (line.find("generated by sleeve") != std::string::npos ||
          line.find("generated by armory") != std::string::npos) {
        isManaged = true;
        break;
      }
      if (line.find("POWERARM_ROOTFS=") != std::string::npos) {
        auto eq = line.find('=');
        rootfs = line.substr(eq + 1);
      }
      if (line.rfind("exec ", 0) == 0) {
        execLine = line.substr(5);
      }
    }

    if (!isManaged && !execLine.empty()) {
      // Find executable in execLine
      std::string targetExe;
      if (execLine[0] == '"') {
        size_t nextQ = execLine.find('"', 1);
        if (nextQ != std::string::npos) {
          targetExe = execLine.substr(1, nextQ - 1);
        }
      } else {
        size_t sp = execLine.find(' ');
        targetExe = (sp != std::string::npos) ? execLine.substr(0, sp) : execLine;
      }

      targetExe = Paths::ExpandUser(targetExe);
      if (fs::exists(targetExe, ec)) {
        auto probe = ElfInspect::ProbeFile(targetExe);
        if (probe.kind == ElfInspect::FileKind::AArch64_Exec || probe.kind == ElfInspect::FileKind::AArch64_Dyn) {
          foreignLaunchers.push_back({name, path, targetExe, rootfs});
        }
      }
    }
  }
}

ScanResult RunScan(const ScanOptions& options) {
  ScanResult result;
  auto startTime = std::chrono::steady_clock::now();

  ScanOptions opts = options;
  if (opts.search_dirs.empty()) {
    const char* home = std::getenv("HOME");
    if (home) {
      opts.search_dirs = {
        std::string(home) + "/Downloads",
        std::string(home) + "/Development",
        std::string(home) + "/.local/opt",
        std::string(home) + "/.local/share",
        "/opt"
      };
      std::string agyIde = std::string(home) + "/Antigravity IDE";
      std::error_code ec;
      if (fs::exists(agyIde, ec)) {
        opts.search_dirs.push_back(agyIde);
      }
    } else {
      opts.search_dirs = {"/opt"};
    }
  }

  std::map<std::string, std::vector<std::string>> dirToBinaries;
  std::vector<std::pair<std::string, ElfInspect::ElfDetails>> loneBinaries;

  for (const auto& d : opts.search_dirs) {
    ScanDirectoryRecursive(Paths::ExpandUser(d), opts, dirToBinaries, loneBinaries, result.stats);
  }

  // Collapse subdirectories into application root directories
  // E.g., ~/Development/vscode-arm64/1.138.0/bin and ~/Development/vscode-arm64/1.138.0
  std::map<std::string, std::vector<std::string>> appDirs;
  for (const auto& [dir, bins] : dirToBinaries) {
    std::string appRoot = dir;
    // Check if dir ends in /bin or /bin/arm64
    if (fs::path(dir).filename() == "bin") {
      appRoot = fs::path(dir).parent_path().string();
    } else if (fs::path(dir).filename() == "arm64" && fs::path(dir).parent_path().filename() == "bin") {
      appRoot = fs::path(dir).parent_path().parent_path().string();
    }
    for (const auto& b : bins) {
      appDirs[appRoot].push_back(b);
    }
  }

  std::set<std::string> detectedExes;

  for (const auto& [appRoot, bins] : appDirs) {
    auto cand = Shapes::DetectDirectoryShape(appRoot, bins);
    if (cand) {
      result.apps.push_back(*cand);
      detectedExes.insert(cand->exe_path);
    }
  }

  // Also check lone binaries (like claude CLI standalone in ~/.local/share/claude/versions/...)
  for (const auto& [binPath, details] : loneBinaries) {
    if (!detectedExes.count(binPath)) {
      auto cand = Shapes::DetectBinaryShape(binPath, details);
      if (cand && cand->shape == Shapes::ShapeType::Runtime) {
        result.apps.push_back(*cand);
        detectedExes.insert(binPath);
      }
    }
  }

  if (opts.scan_overlays) {
    ScanOverlays(opts, result.apps, result.stats);
  }

  if (opts.scan_bin_dir) {
    ScanBinDirectory(result.foreign_launchers);
  }

  auto endTime = std::chrono::steady_clock::now();
  result.stats.elapsed_seconds = std::chrono::duration<double>(endTime - startTime).count();

  return result;
}

} // namespace Sleeve::Scanner
