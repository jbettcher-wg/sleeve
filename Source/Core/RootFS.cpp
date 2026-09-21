// SPDX-License-Identifier: MIT
#include "RootFS.h"
#include "Paths.h"
#include "Record.h"

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

  const char* envRfs = std::getenv("POWERARM_ROOTFS");
  if (envRfs) {
    result.env_rootfs = envRfs;
  }
  result.config_default_rootfs = ReadConfigJsonDefaultRootFS();

  std::vector<std::string> searchDirs = {
    Paths::GetDataDir() + "/RootFS",
    "/usr/share/powerarm/RootFS"
  };

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

      std::string fullPath = entry.path().string();
      if (seenBases.count(filename)) continue;
      seenBases.insert(filename);

      RootFSInfo info;
      info.name = filename;
      info.base_path = fullPath;
      info.base_size_bytes = CalculateDirSize(fullPath);

      std::string overlayPath = sDir + "/" + filename + "-overlay";
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
        if (result.env_rootfs == fullPath || result.env_rootfs == filename) {
          info.is_env_default = true;
        }
      }
      if (!result.config_default_rootfs.empty()) {
        if (result.config_default_rootfs == fullPath || result.config_default_rootfs == filename) {
          info.is_config_default = true;
        }
      }

      if (rootfsToApps.count(fullPath)) {
        info.used_by_apps = rootfsToApps[fullPath];
      } else if (rootfsToApps.count(filename)) {
        info.used_by_apps = rootfsToApps[filename];
      }

      result.rootfses.push_back(info);
    }
  }

  return result;
}

} // namespace Sleeve::RootFS
