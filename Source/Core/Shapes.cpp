// SPDX-License-Identifier: MIT
#include "Shapes.h"
#include "Paths.h"
#include "Backend.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

namespace Sleeve::Shapes {

namespace fs = std::filesystem;

std::string ShapeToString(ShapeType shape) {
  switch (shape) {
    case ShapeType::Electron: return "electron";
    case ShapeType::Gecko:    return "gecko";
    case ShapeType::Game:     return "game";
    case ShapeType::Runtime:  return "runtime";
    case ShapeType::Pacman:   return "pacman";
    case ShapeType::Cli:      return "cli";
    default:                  return "unknown";
  }
}

ShapeType StringToShape(const std::string& str) {
  if (str == "electron") return ShapeType::Electron;
  if (str == "gecko")    return ShapeType::Gecko;
  if (str == "game")     return ShapeType::Game;
  if (str == "runtime")  return ShapeType::Runtime;
  if (str == "pacman")   return ShapeType::Pacman;
  if (str == "cli")      return ShapeType::Cli;
  return ShapeType::Unknown;
}

static std::string ExtractJsonString(const std::string& content, const std::string& key) {
  std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]+)\"");
  std::smatch m;
  if (std::regex_search(content, m, re)) {
    return m[1].str();
  }
  return "";
}

static std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string ExtractIniValue(const std::string& content, const std::string& section, const std::string& key) {
  std::istringstream stream(content);
  std::string line;
  bool inSection = false;
  while (std::getline(stream, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line[0] == '[' && line.back() == ']') {
      std::string s = line.substr(1, line.size() - 2);
      inSection = (s == section);
      continue;
    }
    if (inSection) {
      auto eq = line.find('=');
      if (eq != std::string::npos) {
        std::string k = line.substr(0, eq);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k == key) {
          std::string v = line.substr(eq + 1);
          size_t start = v.find_first_not_of(" \t");
          if (start != std::string::npos) v = v.substr(start);
          return v;
        }
      }
    }
  }
  return "";
}

std::string ExtractVersionFromPath(const std::string& path) {
  std::regex verRe(R"((\d+\.\d+(\.\d+)*))");
  std::smatch m;
  if (std::regex_search(path, m, verRe)) {
    return m[1].str();
  }
  return "";
}

std::optional<AppCandidate> DetectDirectoryShape(const std::string& dirPath, const std::vector<std::string>& aarch64Binaries) {
  if (aarch64Binaries.empty()) {
    return std::nullopt;
  }

  const auto& backend = Backend::GetActiveBackend();
  AppCandidate cand;
  cand.dir = dirPath;
  cand.version = ExtractVersionFromPath(dirPath);

  // 0. Steam Detection
  bool hasSteamSh = fs::exists(dirPath + "/steam.sh");
  bool hasSteam32 = fs::exists(dirPath + "/ubuntu12_32/steam");
  bool hasSteamWeb = fs::exists(dirPath + "/ubuntu12_64/steamwebhelper");
  if (hasSteamSh || hasSteam32 || hasSteamWeb) {
    cand.shape = ShapeType::Game;
    cand.name = "steam";
    cand.title = "Steam";
    cand.exe_path = hasSteamSh ? (dirPath + "/steam.sh") : (dirPath + "/ubuntu12_32/steam");
    cand.default_args = {"-tcp"};
    cand.default_env["FEX_HOSTPAGEMODE"] = "force";
    cand.desktop_enabled = true;
    cand.wmclass = "Steam";
    cand.exec_field = "%U";

    std::vector<std::string> steamIcons = {
      dirPath + "/public/steam_tray.ico",
      "/usr/share/pixmaps/steam.png",
      "/usr/share/icons/hicolor/256x256/apps/steam.png",
      "/usr/share/icons/hicolor/48x48/apps/steam.png"
    };
    for (const auto& ic : steamIcons) {
      std::error_code ec;
      if (fs::exists(ic, ec)) {
        cand.icon_path = ic;
        break;
      }
    }
    return cand;
  }

  // 1. Electron Detection
  bool hasCrashpad = fs::exists(dirPath + "/chrome_crashpad_handler");
  bool hasV8 = fs::exists(dirPath + "/v8_context_snapshot.bin");
  bool hasPackageJson = fs::exists(dirPath + "/resources/app/package.json");
  bool hasProductJson = fs::exists(dirPath + "/resources/app/product.json");

  if (hasCrashpad || hasV8 || hasPackageJson || hasProductJson) {
    cand.shape = ShapeType::Electron;
    cand.default_args = {"--no-sandbox"};
    cand.default_env["ELECTRON_OZONE_PLATFORM_HINT"] = "auto";
    cand.presets["electron-no-sandbox"] = true;
    cand.exec_field = "%F";
    cand.desktop_enabled = true;

    std::string appName;
    std::string nameShort;
    std::string productContent;

    if (hasProductJson) {
      productContent = ReadFile(dirPath + "/resources/app/product.json");
      nameShort = ExtractJsonString(productContent, "nameShort");
      appName = ExtractJsonString(productContent, "applicationName");
      if (cand.version.empty()) {
        cand.version = ExtractJsonString(productContent, "ideVersion");
      }
    }

    if (hasPackageJson) {
      std::string pkgContent = ReadFile(dirPath + "/resources/app/package.json");
      if (appName.empty()) {
        appName = ExtractJsonString(pkgContent, "name");
      }
      if (cand.version.empty()) {
        cand.version = ExtractJsonString(pkgContent, "version");
      }
    }

    if (!appName.empty()) {
      cand.name = appName;
    } else {
      // Fallback to directory name
      cand.name = fs::path(dirPath).filename().string();
      if (cand.name.rfind("-arm64") != std::string::npos) {
        cand.name = cand.name.substr(0, cand.name.rfind("-arm64"));
      }
    }

    if (!nameShort.empty()) {
      cand.title = nameShort;
      cand.wmclass = nameShort;
    } else {
      cand.title = cand.name;
      if (!cand.title.empty()) cand.title[0] = std::toupper(cand.title[0]);
      cand.wmclass = cand.title;
    }

    // Pick main executable
    std::string binScript = dirPath + "/bin/" + cand.name;
    if (fs::exists(binScript)) {
      cand.exe_path = binScript;
    } else {
      for (const auto& b : aarch64Binaries) {
        std::string fname = fs::path(b).filename().string();
        if (fname == cand.name || fname == "code" || fname == "antigravity-ide") {
          cand.exe_path = b;
          break;
        }
      }
      if (cand.exe_path.empty()) {
        cand.exe_path = aarch64Binaries.front();
      }
    }

    // Pick icon
    std::vector<std::string> iconCandidates = {
      dirPath + "/resources/app/resources/linux/" + cand.name + ".png",
      dirPath + "/resources/app/resources/linux/code.png",
      dirPath + "/resources/linux/code.png",
      dirPath + "/resources/app/resources/linux/icon.png",
    };
    for (const auto& ic : iconCandidates) {
      if (fs::exists(ic)) {
        cand.icon_path = ic;
        break;
      }
    }
    cand.categories = "Development;IDE;";
    return cand;
  }

  // 2. Gecko Detection
  bool hasAppIni = fs::exists(dirPath + "/application.ini");
  bool hasLibXul = fs::exists(dirPath + "/libxul.so");
  if (hasAppIni || hasLibXul) {
    cand.shape = ShapeType::Gecko;
    cand.desktop_enabled = true;
    cand.exec_field = "%u";
    cand.default_env["MOZ_ENABLE_WAYLAND"] = "1";

    if (hasAppIni) {
      std::string iniContent = ReadFile(dirPath + "/application.ini");
      std::string name = ExtractIniValue(iniContent, "App", "Name");
      std::string ver = ExtractIniValue(iniContent, "App", "Version");
      if (!name.empty()) {
        cand.title = name;
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        cand.name = lowerName;
        cand.wmclass = lowerName;
      }
      if (!ver.empty() && cand.version.empty()) {
        cand.version = ver;
      }
    }
    if (cand.name.empty()) {
      cand.name = "firefox";
      cand.title = "Firefox";
      cand.wmclass = "firefox";
    }

    // Store the raw word. Quoting is the generator's job -- an argument that carries its
    // own quotes either reaches argv with them attached or gets quoted twice.
    cand.default_args = {"--profile", "$HOME/.mozilla/" + cand.name + "-" + std::string(backend.archName)};

    // Pick main executable
    for (const auto& b : aarch64Binaries) {
      std::string fname = fs::path(b).filename().string();
      if (fname == cand.name || fname == "firefox") {
        cand.exe_path = b;
        break;
      }
    }
    if (cand.exe_path.empty()) {
      cand.exe_path = aarch64Binaries.front();
    }

    cand.categories = "Network;WebBrowser;";
    return cand;
  }

  // 3. Game Detection
  bool hasGameDir = fs::exists(dirPath + "/data") || fs::exists(dirPath + "/bin/arm64");
  if (hasGameDir) {
    cand.shape = ShapeType::Game;
    cand.desktop_enabled = true;
    cand.cwd = "install";
    cand.categories = "Game;";
    cand.name = fs::path(dirPath).filename().string();
    if (cand.name == "current" || std::regex_match(cand.name, std::regex(R"(\d+(\.\d+)*)"))) {
      std::string parentName = fs::path(dirPath).parent_path().filename().string();
      if (parentName.rfind("-arm64") != std::string::npos) {
        cand.name = parentName.substr(0, parentName.rfind("-arm64"));
      } else {
        cand.name = parentName;
      }
    } else if (cand.name.rfind("-arm64") != std::string::npos) {
      cand.name = cand.name.substr(0, cand.name.rfind("-arm64"));
    }
    cand.title = cand.name;
    if (!cand.title.empty()) cand.title[0] = std::toupper(cand.title[0]);
    cand.wmclass = cand.title;

    for (const auto& b : aarch64Binaries) {
      if (b.find("bin/arm64") != std::string::npos || fs::path(b).filename().string() == cand.name) {
        cand.exe_path = b;
        break;
      }
    }
    if (cand.exe_path.empty()) {
      cand.exe_path = aarch64Binaries.front();
    }

    std::vector<std::string> gameIconCandidates = {
      dirPath + "/data/core/graphics/" + cand.name + ".png",
      dirPath + "/data/" + cand.name + ".png",
      dirPath + "/" + cand.name + ".png"
    };
    for (const auto& ic : gameIconCandidates) {
      if (fs::exists(ic)) {
        cand.icon_path = ic;
        break;
      }
    }
    return cand;
  }

  // 4. Default directory with AArch64 executable
  cand.shape = ShapeType::Cli;
  cand.name = fs::path(dirPath).filename().string();
  cand.title = cand.name;
  cand.exe_path = aarch64Binaries.front();
  cand.desktop_enabled = false;
  return cand;
}

std::optional<AppCandidate> DetectBinaryShape(const std::string& binaryPath, const ElfInspect::ElfDetails& details) {
  AppCandidate cand;
  cand.dir = fs::path(binaryPath).parent_path().string();
  cand.exe_path = binaryPath;
  cand.name = fs::path(binaryPath).filename().string();
  if (std::regex_match(cand.name, std::regex(R"(\d+(\.\d+)*)"))) {
    fs::path p(binaryPath);
    if (p.parent_path().filename() == "versions") {
      cand.name = p.parent_path().parent_path().filename().string();
    }
  }
  cand.title = cand.name;
  if (!cand.title.empty()) cand.title[0] = std::toupper(cand.title[0]);
  cand.version = ExtractVersionFromPath(binaryPath);

  // Check if Runtime (single binary > 30MB, e.g. Claude Code, Bun, Node)
  if (details.file_size > 30 * 1024 * 1024) {
    bool hasGuiLibs = false;
    for (const auto& lib : details.needed_libs) {
      if (lib.find("libX") != std::string::npos || lib.find("libgtk") != std::string::npos ||
          lib.find("libwayland") != std::string::npos || lib.find("libSDL") != std::string::npos) {
        hasGuiLibs = true;
        break;
      }
    }
    if (!hasGuiLibs) {
      cand.shape = ShapeType::Runtime;
      cand.desktop_enabled = false;
      return cand;
    }
  }

  // Check if CLI
  cand.shape = ShapeType::Cli;
  cand.desktop_enabled = false;
  return cand;
}

std::optional<AppCandidate> DetectPacmanPackage(const std::string& overlayRoot, const std::string& pkgDescPath) {
  // Read package desc
  std::string desc = ReadFile(pkgDescPath);
  if (desc.empty()) return std::nullopt;

  std::string pkgName;
  std::string pkgVer;

  std::istringstream stream(desc);
  std::string line;
  while (std::getline(stream, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line == "%NAME%") {
      if (std::getline(stream, pkgName)) {
        while (!pkgName.empty() && (pkgName.back() == '\r' || pkgName.back() == ' ')) pkgName.pop_back();
      }
    } else if (line == "%VERSION%") {
      if (std::getline(stream, pkgVer)) {
        while (!pkgVer.empty() && (pkgVer.back() == '\r' || pkgVer.back() == ' ')) pkgVer.pop_back();
      }
    }
  }

  if (pkgName.empty()) return std::nullopt;

  // Which .desktop file this package ships is not a guess: pacman records every path the
  // package owns next to the desc. Matching on the file name instead made `gnupg` claim
  // `org.gnupg.pinentry-qt.desktop`, which pinentry owns, and produced two records for one
  // binary.
  std::string appDir = overlayRoot + "/usr/share/applications";
  std::error_code ec;
  if (!fs::exists(appDir, ec)) return std::nullopt;

  std::vector<std::string> ownedDesktops;
  std::string filesPath = fs::path(pkgDescPath).parent_path().string() + "/files";
  bool haveFileList = fs::exists(filesPath, ec);
  if (haveFileList) {
    std::string files = ReadFile(filesPath);
    std::istringstream fstream(files);
    std::string fline;
    while (std::getline(fstream, fline)) {
      while (!fline.empty() && (fline.back() == '\r' || fline.back() == ' ')) fline.pop_back();
      if (fline.rfind("usr/share/applications/", 0) != 0) continue;
      if (fline.size() < 8 || fline.compare(fline.size() - 8, 8, ".desktop") != 0) continue;
      std::string full = overlayRoot + "/" + fline;
      if (fs::exists(full, ec)) {
        ownedDesktops.push_back(full);
      }
    }
  } else {
    // No file list (older pacman db): fall back to an exact or reverse-DNS name match only.
    for (const auto& entry : fs::directory_iterator(appDir, ec)) {
      if (entry.path().extension() != ".desktop") continue;
      std::string stem = entry.path().stem().string();
      if (stem == pkgName || (stem.size() > pkgName.size() &&
                              stem.compare(stem.size() - pkgName.size(), pkgName.size(), pkgName) == 0 &&
                              stem[stem.size() - pkgName.size() - 1] == '.')) {
        ownedDesktops.push_back(entry.path().string());
      }
    }
  }

  if (ownedDesktops.empty()) return std::nullopt;

  // A package can ship several entries; take the one named after the package.
  std::sort(ownedDesktops.begin(), ownedDesktops.end());
  std::string foundDesktop = ownedDesktops.front();
  for (const auto& d : ownedDesktops) {
    std::string stem = fs::path(d).stem().string();
    if (stem == pkgName) {
      foundDesktop = d;
      break;
    }
    if (stem.find(pkgName) != std::string::npos && foundDesktop == ownedDesktops.front()) {
      foundDesktop = d;
    }
  }

  // Parse desktop file
  std::string dContent = ReadFile(foundDesktop);
  std::string name = ExtractIniValue(dContent, "Desktop Entry", "Name");
  std::string exec = ExtractIniValue(dContent, "Desktop Entry", "Exec");
  std::string icon = ExtractIniValue(dContent, "Desktop Entry", "Icon");
  std::string cat = ExtractIniValue(dContent, "Desktop Entry", "Categories");
  std::string wm = ExtractIniValue(dContent, "Desktop Entry", "StartupWMClass");
  std::string mime = ExtractIniValue(dContent, "Desktop Entry", "MimeType");

  AppCandidate cand;
  cand.shape = ShapeType::Pacman;
  cand.name = pkgName;
  cand.title = name.empty() ? pkgName : name;
  cand.version = pkgVer;
  cand.pacman_package = pkgName;
  cand.categories = cat;
  cand.mimetypes = mime;
  cand.wmclass = wm.empty() ? pkgName : wm;
  cand.desktop_enabled = true;

  // Resolve executable path inside overlay or rootfs
  std::string exeCmd = exec;
  size_t sp = exeCmd.find(' ');
  if (sp != std::string::npos) {
    exeCmd = exeCmd.substr(0, sp);
  }
  if (!exeCmd.empty() && exeCmd[0] == '/') {
    cand.exe_path = overlayRoot + exeCmd;
  } else {
    cand.exe_path = overlayRoot + "/usr/bin/" + exeCmd;
  }

  if (!icon.empty()) {
    if (icon[0] == '/') {
      cand.icon_path = overlayRoot + icon;
    } else {
      // Look in hicolor or pixmaps
      std::string px = overlayRoot + "/usr/share/pixmaps/" + icon + ".png";
      if (fs::exists(px)) {
        cand.icon_path = px;
      } else {
        cand.icon_path = icon;
      }
    }
  }

  return cand;
}

} // namespace Sleeve::Shapes
