// SPDX-License-Identifier: MIT
#include "RootFS.h"
#include "Paths.h"
#include "Record.h"
#include "Backend.h"
#include "ElfInspect.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <set>
#include <tiny-json.h>

namespace Sleeve::RootFS {

namespace fs = std::filesystem;

std::string FormatBytes(uint64_t bytes) {
  std::ostringstream ss;
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    ss << std::fixed << std::setprecision(1) << gb << " GB";
  } else if (bytes >= 1024ULL * 1024ULL) {
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    ss << std::fixed << std::setprecision(0) << mb << " MB";
  } else if (bytes >= 1024ULL) {
    double kb = static_cast<double>(bytes) / 1024.0;
    ss << std::fixed << std::setprecision(0) << kb << " KB";
  } else {
    ss << bytes << " B";
  }
  return ss.str();
}

static uint64_t CalculateDirSize(const std::string& path, size_t maxFiles = 2000) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return 0;
  if (fs::is_regular_file(path, ec)) {
    return fs::file_size(path, ec);
  }

  uint64_t total = 0;
  size_t count = 0;
  auto iter = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
  auto end = fs::recursive_directory_iterator();

  while (iter != end && count < maxFiles) {
    if (!ec && iter->is_regular_file(ec)) {
      total += iter->file_size(ec);
      count++;
    }
    iter.increment(ec);
  }
  return total;
}

// Walks `relative` inside `base` the way the guest kernel would: an absolute symlink target
// is re-rooted at `base` instead of at the host's /, so `/bin/sh -> /usr/bin/bash` resolves to
// the guest's bash and not the host's. Returns "" when it does not resolve inside the tree.
static std::string ResolveInsideRootFS(const std::string& base, const std::string& relative) {
  std::error_code ec;
  fs::path basePath = fs::path(base);
  fs::path result = basePath;

  std::vector<std::string> pending;
  for (const auto& part : fs::path(relative).relative_path()) {
    pending.push_back(part.string());
  }

  int hops = 0;
  while (!pending.empty()) {
    if (++hops > 64) return "";

    std::string part = pending.front();
    pending.erase(pending.begin());
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (result == basePath) return "";
      result = result.parent_path();
      continue;
    }

    result /= part;
    if (!fs::is_symlink(result, ec)) continue;

    fs::path target = fs::read_symlink(result, ec);
    if (ec || target.empty()) return "";

    std::vector<std::string> expanded;
    for (const auto& p : target.relative_path()) {
      expanded.push_back(p.string());
    }
    result = target.is_absolute() ? basePath : result.parent_path();
    pending.insert(pending.begin(), expanded.begin(), expanded.end());
  }

  // Never report a path that climbed back out of the tree.
  std::string resultStr = result.lexically_normal().string();
  std::string baseStr = basePath.lexically_normal().string();
  if (resultStr.rfind(baseStr, 0) != 0) return "";
  return resultStr;
}

uint16_t DetectRootFSMachine(const std::string& rootfsPath) {
  static const char* kProbePaths[] = {
    "/usr/bin/env",
    "/bin/sh",
    "/usr/bin/sh",
    "/bin/bash",
    "/usr/bin/bash",
    "/usr/bin/ls",
  };

  std::error_code ec;
  for (const char* rel : kProbePaths) {
    std::string resolved = ResolveInsideRootFS(rootfsPath, rel);
    if (resolved.empty()) continue;
    if (!fs::is_regular_file(resolved, ec)) continue;
    auto probe = ElfInspect::ProbeFile(resolved);
    if (probe.machine != 0) return probe.machine;
  }
  return 0;
}

static std::string ReadConfigJsonDefaultRootFS() {
  std::string cfgPath = Paths::GetConfigDir() + "/Config.json";
  std::ifstream f(cfgPath);
  if (!f.is_open()) return "";

  std::stringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();
  if (content.empty()) return "";

  std::vector<char> buf(content.begin(), content.end());
  buf.push_back('\0');

  json_t mem[128];
  const json_t* root = json_create(buf.data(), mem, 128);
  if (!root) return "";

  const json_t* cfg = json_getProperty(root, "Config");
  if (!cfg) return "";

  const json_t* rfs = json_getProperty(cfg, "RootFS");
  if (!rfs) return "";

  const char* val = json_getValue(rfs);
  return val ? std::string(val) : "";
}

DiscoveryResult DiscoverRootFSes() {
  DiscoveryResult result;

  const auto& backend = Backend::GetActiveBackend();
  std::string envVarName = backend.envPrefix + "ROOTFS";
  const char* envRfs = std::getenv(envVarName.c_str());
  if (envRfs && envRfs[0] != '\0') {
    result.env_rootfs = envRfs;
  }
  result.config_default_rootfs = ReadConfigJsonDefaultRootFS();

  std::vector<std::string> searchDirs = {
    Paths::GetDataDir() + "/RootFS",
    "/usr/share/" + backend.dataSubdir + "/RootFS"
  };

  // Only this backend's stores. A checkout-local RootFS dir belonging to the *other*
  // backend holds a guest of the wrong architecture, and a rootfs that does not resolve
  // falls back to host binaries instead of failing loudly.
  const char* home = std::getenv("HOME");
  if (home) {
    searchDirs.push_back(std::string(home) + "/Development/" + backend.devSubdir + "/RootFS");
  }

  if (!result.env_rootfs.empty()) {
    std::error_code ec;
    fs::path p = Paths::ExpandUser(result.env_rootfs);
    if (fs::exists(p, ec)) {
      if (fs::is_directory(p, ec)) {
        searchDirs.push_back(p.parent_path().string());
      } else {
        searchDirs.push_back(p.parent_path().string());
      }
    }
  }

  auto records = Record::ListRecords();
  std::map<std::string, std::vector<std::string>> rootfsToApps;
  for (const auto& rec : records) {
    if (!rec.rootfs.empty()) {
      rootfsToApps[rec.rootfs].push_back(rec.name);
      // Also map by basename
      std::string bname = fs::path(rec.rootfs).filename().string();
      rootfsToApps[bname].push_back(rec.name);
    }
  }

  std::set<std::string> seenBases;

  for (const auto& sDir : searchDirs) {
    std::error_code ec;
    if (!fs::exists(sDir, ec) || !fs::is_directory(sDir, ec)) continue;

    for (const auto& entry : fs::directory_iterator(sDir, ec)) {
      std::string filename = entry.path().filename().string();
      if (filename.rfind("-overlay") != std::string::npos) {
        continue;
      }

      std::string baseName = filename;
      if (baseName.size() > 5 && baseName.rfind(".sqsh") == baseName.size() - 5) {
        baseName = baseName.substr(0, baseName.size() - 5);
      }

      std::string fullPath = entry.path().string();
      if (seenBases.count(baseName)) continue;

      // Verify rather than infer from the directory it sits in: a tree whose own
      // userspace is the wrong machine is not a rootfs this backend can run.
      uint16_t machine = DetectRootFSMachine(fullPath);
      if (machine != 0 && machine != backend.elfMachine) continue;

      seenBases.insert(baseName);

      RootFSInfo info;
      info.name = baseName;
      info.base_path = fullPath;
      info.base_size_bytes = CalculateDirSize(fullPath);
      info.elf_machine = machine;
      info.arch_verified = (machine == backend.elfMachine);

      std::string overlayPath = sDir + "/" + baseName + "-overlay";
      if (fs::exists(overlayPath, ec) && fs::is_directory(overlayPath, ec)) {
        info.has_overlay = true;
        info.overlay_path = overlayPath;
        info.overlay_size_bytes = CalculateDirSize(overlayPath);

        std::string pacmanLocal = overlayPath + "/var/lib/pacman/local";
        if (fs::exists(pacmanLocal, ec) && fs::is_directory(pacmanLocal, ec)) {
          size_t pkgs = 0;
          for (const auto& pentry : fs::directory_iterator(pacmanLocal, ec)) {
            if (pentry.is_directory(ec)) {
              pkgs++;
            }
          }
          if (pkgs > 0) {
            info.has_pacman = true;
            info.package_count = pkgs;
          }
        }
      }

      if (!result.env_rootfs.empty()) {
        std::string envNorm = Paths::ExpandUser(result.env_rootfs);
        if (envNorm == fullPath || result.env_rootfs == baseName || result.env_rootfs == filename ||
            envNorm == (sDir + "/" + baseName)) {
          info.is_env_default = true;
        }
      }
      if (!result.config_default_rootfs.empty()) {
        std::string cfgNorm = Paths::ExpandUser(result.config_default_rootfs);
        if (cfgNorm == fullPath || result.config_default_rootfs == baseName || result.config_default_rootfs == filename ||
            cfgNorm == (sDir + "/" + baseName)) {
          info.is_config_default = true;
        }
      }

      if (rootfsToApps.count(fullPath)) {
        info.used_by_apps = rootfsToApps[fullPath];
      } else if (rootfsToApps.count(baseName)) {
        info.used_by_apps = rootfsToApps[baseName];
      } else if (rootfsToApps.count(filename)) {
        info.used_by_apps = rootfsToApps[filename];
      }

      result.rootfses.push_back(info);
    }
  }

  return result;
}

ResolvedRootFS ResolveRootFSName(const std::string& nameOrPath) {
  ResolvedRootFS res;
  res.name = nameOrPath;

  const auto& backend = Backend::GetActiveBackend();
  if (nameOrPath.empty()) {
    res.error = "no rootfs given";
    return res;
  }

  std::string expanded = Paths::ExpandUser(nameOrPath);

  auto discovered = DiscoverRootFSes();
  for (const auto& rfs : discovered.rootfses) {
    if (rfs.name == nameOrPath || rfs.base_path == expanded) {
      res.ok = true;
      res.name = rfs.name;
      res.path = rfs.base_path;
      return res;
    }
  }

  // A path outside the search dirs is still usable, as long as it is the right guest.
  std::error_code ec;
  if (expanded.find('/') != std::string::npos && fs::exists(expanded, ec)) {
    uint16_t machine = DetectRootFSMachine(expanded);
    if (machine != 0 && machine != backend.elfMachine) {
      res.error = expanded + " is a " + std::to_string(machine) +
                  "-machine tree; " + backend.displayName + " runs machine " +
                  std::to_string(backend.elfMachine) + " guests";
      return res;
    }
    res.ok = true;
    res.name = fs::path(expanded).filename().string();
    res.path = expanded;
    return res;
  }

  std::string known;
  for (const auto& rfs : discovered.rootfses) {
    known += (known.empty() ? "" : ", ") + rfs.name;
  }
  res.error = "no rootfs '" + nameOrPath + "' for backend " + backend.displayName +
              (known.empty() ? " (none discovered)" : " (known: " + known + ")");
  return res;
}

} // namespace Sleeve::RootFS
