// SPDX-License-Identifier: MIT
#include "Paths.h"
#include "Backend.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <climits>
#include <filesystem>

namespace Sleeve::Paths {

namespace fs = std::filesystem;

std::string ExpandUser(const std::string& path) {
  if (path.empty()) return path;
  const char* home = std::getenv("HOME");
  if (!home) return path;

  if (path[0] == '~') {
    if (path.size() == 1) return home;
    if (path[1] == '/') return std::string(home) + path.substr(1);
  }
  if (path.rfind("${HOME}", 0) == 0) {
    return std::string(home) + path.substr(7);
  }
  if (path.rfind("$HOME", 0) == 0) {
    return std::string(home) + path.substr(5);
  }
  return path;
}

std::string ContractUser(const std::string& path) {
  const char* home = std::getenv("HOME");
  if (!home || home[0] == '\0') return path;
  std::string homeStr = home;
  if (path == homeStr) return "~";
  if (path.rfind(homeStr + "/", 0) == 0) {
    return "~" + path.substr(homeStr.size());
  }
  return path;
}

std::string GetConfigDir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  std::string base = (xdg && xdg[0]) ? xdg : ((home ? std::string(home) : "") + "/.config");
  return base + "/" + Backend::GetActiveBackend().configSubdir;
}

std::string GetDataDir() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  std::string base = (xdg && xdg[0]) ? xdg : ((home ? std::string(home) : "") + "/.local/share");
  return base + "/" + Backend::GetActiveBackend().dataSubdir;
}

std::string GetCacheDir() {
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");
  std::string base = (xdg && xdg[0]) ? xdg : ((home ? std::string(home) : "") + "/.cache");
  return base + "/" + Backend::GetActiveBackend().dataSubdir;
}

std::string GetRecordDir() {
  return GetConfigDir() + "/sleeve/apps";
}

std::string GetAppConfigDir() {
  return GetConfigDir() + "/AppConfig";
}

std::string GetUserConfigPath() {
  return GetConfigDir() + "/Config.json";
}

std::string GetSettingsPath() {
  return GetConfigDir() + "/sleeve/settings.json";
}

std::string GetAppsDir(const std::string& configuredAppsDir) {
  if (!configuredAppsDir.empty()) {
    return ExpandUser(configuredAppsDir);
  }
  const char* home = std::getenv("HOME");
  if (home) {
    std::string devDir = std::string(home) + "/Development";
    if (fs::is_directory(devDir)) {
      return devDir;
    }
    return std::string(home) + "/.local/opt";
  }
  return "/opt";
}

std::string GetStableVersion() {
  const char* home = std::getenv("HOME");
  if (home) {
    std::string vpath = std::string(home) + "/.local/opt/powerarm-stable/VERSION";
    std::ifstream f(vpath);
    if (f.is_open()) {
      std::string ver;
      if (std::getline(f, ver)) {
        while (!ver.empty() && (ver.back() == '\r' || ver.back() == '\n' || ver.back() == ' ')) {
          ver.pop_back();
        }
        return ver;
      }
    }
  }
  return "";
}

std::string GetBinfmtStatus() {
  std::ifstream f("/proc/sys/fs/binfmt_misc/POWERarm-aarch64");
  if (f.is_open()) {
    std::string line;
    while (std::getline(f, line)) {
      if (line.find("enabled") != std::string::npos) {
        return "OK (binfmt registered)";
      }
    }
    return "disabled";
  }

  std::ifstream general("/proc/sys/fs/binfmt_misc/status");
  if (general.is_open()) {
    return "available (POWERarm not registered)";
  }
  return "binfmt_misc not mounted";
}

std::string GetScriptDir() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    std::string selfDir = fs::path(buf).parent_path().string();
    std::string candidate = selfDir + "/../Data/rootfs";
    if (fs::is_directory(candidate)) {
      return candidate;
    }
  }

  std::string dataRootfs = GetDataDir() + "/rootfs";
  if (fs::is_directory(dataRootfs)) {
    return dataRootfs;
  }

  if (fs::is_directory("/usr/share/powerarm/rootfs")) {
    return "/usr/share/powerarm/rootfs";
  }

  return "";
}

} // namespace Sleeve::Paths
