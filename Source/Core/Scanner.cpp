// SPDX-License-Identifier: MIT
#include "Scanner.h"
#include "ElfInspect.h"
#include "Paths.h"
#include "Shapes.h"
#include "Backend.h"

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
                                   std::map<std::string, std::string>& binaryToOrigin,
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
    if (ElfInspect::IsTargetBinary(probe)) {
      auto details = ElfInspect::InspectTarget(pathStr);
      ElfInspect::ElfDetails det;
      if (details) {
        det = *details;
      } else {
        det.probe = probe;
        det.file_size = fs::file_size(entry.path(), ec);
      }
      std::string parentDir = entry.path().parent_path().string();
      dirToAArch64Binaries[parentDir].push_back(pathStr);
      binaryToOrigin[pathStr] = rootDir;
      loneBinaries.push_back({pathStr, det});
    } else {
      switch (probe.kind) {
        case ElfInspect::FileKind::Foreign_PPC64LE:
          stats.ppc64le_skipped++;
          break;
        case ElfInspect::FileKind::Foreign_X86_64:
          stats.x86_64_skipped++;
          break;
        default:
          stats.other_skipped++;
          break;
      }
    }

    iter.increment(ec);
  }
}

std::string OverlayOrigin(const std::string& overlayName) {
  return "overlay:" + overlayName;
}

size_t DedupeByResolvedTarget(std::vector<Shapes::AppCandidate>& apps) {
  // How well a candidate's name describes the binary it points at. Two packages claiming
  // one executable is not a tie to break at random: `pinentry` describes `pinentry-qt`,
  // `gnupg` does not.
  auto nameRank = [](const Shapes::AppCandidate& a) {
    std::string exeName = fs::path(a.exe_path).filename().string();
    if (a.name.empty()) return 4;
    if (exeName == a.name) return 0;
    if (exeName.rfind(a.name, 0) == 0) return 1;
    if (exeName.find(a.name) != std::string::npos) return 2;
    return 3;
  };

  std::map<std::string, size_t> byTarget; // resolved exe -> index into kept
  std::vector<Shapes::AppCandidate> kept;
  size_t dropped = 0;

  for (const auto& app : apps) {
    std::error_code ec;
    std::string resolved = fs::weakly_canonical(fs::path(app.exe_path), ec).string();
    if (ec || resolved.empty()) resolved = app.exe_path;

    auto it = byTarget.find(resolved);
    if (it == byTarget.end()) {
      byTarget[resolved] = kept.size();
      kept.push_back(app);
      continue;
    }

    dropped++;
    if (nameRank(app) < nameRank(kept[it->second])) {
      kept[it->second] = app;
    }
  }

  apps.swap(kept);
  return dropped;
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
              cand->origin = OverlayOrigin(baseName);
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
      if (line.find("POWERARM_ROOTFS=") != std::string::npos || line.find("FEX_ROOTFS=") != std::string::npos) {
        auto eq = line.find('=');
        rootfs = line.substr(eq + 1);
        if (rootfs.rfind("${", 0) == 0) {
          auto col = rootfs.find(":-");
          if (col != std::string::npos) {
            rootfs = rootfs.substr(col + 2);
            if (!rootfs.empty() && rootfs.back() == '}') rootfs.pop_back();
          }
        }
      }
      if (line.find("steam.sh") != std::string::npos) {
        execLine = Paths::ExpandUser("~/.local/share/Steam/steam.sh");
      } else if (line.rfind("exec ", 0) == 0) {
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
        if (ElfInspect::IsTargetBinary(probe) || targetExe.find("steam.sh") != std::string::npos) {
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
  // A caller who names directories asked about those directories. Folding the standard
  // locations into that answer presents rows from somewhere else as the result.
  result.explicit_dirs = !opts.search_dirs.empty();
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
  std::map<std::string, std::string> binaryToOrigin;
  std::vector<std::pair<std::string, ElfInspect::ElfDetails>> loneBinaries;

  for (const auto& d : opts.search_dirs) {
    std::string expanded = Paths::ExpandUser(d);
    result.searched_dirs.push_back(expanded);
    ScanDirectoryRecursive(expanded, opts, dirToBinaries, binaryToOrigin, loneBinaries, result.stats);
  }

  // Collapse subdirectories into application root directories
  // E.g., ~/Development/vscode-arm64/1.138.0/bin and ~/Development/vscode-arm64/1.138.0
  std::map<std::string, std::vector<std::string>> appDirs;
  std::map<std::string, std::string> appRootVersionedPath;
  for (const auto& [dir, bins] : dirToBinaries) {
    std::string appRoot = dir;
    // Check if dir ends in /bin or /bin/arm64 or /bin/x64
    if (fs::path(dir).filename() == "bin") {
      appRoot = fs::path(dir).parent_path().string();
    } else if ((fs::path(dir).filename() == "arm64" || fs::path(dir).filename() == "x64") && fs::path(dir).parent_path().filename() == "bin") {
      appRoot = fs::path(dir).parent_path().parent_path().string();
    } else if (fs::path(dir).parent_path().filename() == "Steam" || fs::path(dir).parent_path().parent_path().filename() == "Steam") {
      appRoot = Paths::ExpandUser("~/.local/share/Steam");
    }

    // The walk never follows symlinks, so it always lands on the versioned directory.
    // Report the stable alias when one points at it: the version directory is renamed on
    // every update, and a launcher pinned to it breaks silently.
    std::string versionedRoot = appRoot;
    std::string stableRoot = Paths::PreferStableSymlinkPath(appRoot);

    for (const auto& b : bins) {
      std::string binPath = b;
      if (stableRoot != versionedRoot && binPath.rfind(versionedRoot + "/", 0) == 0) {
        binPath = stableRoot + binPath.substr(versionedRoot.size());
      }
      appDirs[stableRoot].push_back(binPath);
      if (binaryToOrigin.count(b)) {
        binaryToOrigin[binPath] = binaryToOrigin[b];
      }
    }
    appRootVersionedPath[stableRoot] = versionedRoot;
  }

  std::set<std::string> detectedExes;

  for (const auto& [appRoot, bins] : appDirs) {
    auto cand = Shapes::DetectDirectoryShape(appRoot, bins);
    if (cand) {
      if (cand->version.empty() && appRootVersionedPath.count(appRoot)) {
        cand->version = Shapes::ExtractVersionFromPath(appRootVersionedPath.at(appRoot));
      }
      if (binaryToOrigin.count(cand->exe_path)) {
        cand->origin = binaryToOrigin.at(cand->exe_path);
      } else if (!bins.empty() && binaryToOrigin.count(bins.front())) {
        cand->origin = binaryToOrigin.at(bins.front());
      }
      result.apps.push_back(*cand);
      detectedExes.insert(cand->exe_path);
    }
  }

  // Also check lone binaries (like claude CLI standalone in ~/.local/share/claude/versions/...)
  for (const auto& [binPath, details] : loneBinaries) {
    if (!detectedExes.count(binPath)) {
      auto cand = Shapes::DetectBinaryShape(binPath, details);
      if (cand && cand->shape == Shapes::ShapeType::Runtime) {
        if (binaryToOrigin.count(binPath)) {
          cand->origin = binaryToOrigin.at(binPath);
        }
        result.apps.push_back(*cand);
        detectedExes.insert(binPath);
      }
    }
  }

  if (opts.scan_overlays && !result.explicit_dirs) {
    ScanOverlays(opts, result.apps, result.stats);
  }

  if (opts.scan_bin_dir && !result.explicit_dirs) {
    ScanBinDirectory(result.foreign_launchers);
  }

  // One binary is one app, however many places named it.
  result.stats.duplicates_collapsed = DedupeByResolvedTarget(result.apps);

  auto endTime = std::chrono::steady_clock::now();
  result.stats.elapsed_seconds = std::chrono::duration<double>(endTime - startTime).count();

  return result;
}

} // namespace Sleeve::Scanner
