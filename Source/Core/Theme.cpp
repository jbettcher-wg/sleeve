// SPDX-License-Identifier: MIT
#include "Theme.h"
#include "Paths.h"
#include "Process.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <map>
#include <regex>

namespace Sleeve::Theme {

namespace fs = std::filesystem;

std::string ColorToHex(const RGBColor& c) {
  std::ostringstream ss;
  ss << "#" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c.r)
     << std::setw(2) << std::setfill('0') << static_cast<int>(c.g)
     << std::setw(2) << std::setfill('0') << static_cast<int>(c.b);
  return ss.str();
}

std::optional<RGBColor> ParseHex(const std::string& hex) {
  std::string s = hex;
  if (!s.empty() && s[0] == '#') s = s.substr(1);
  if (s.size() != 6) return std::nullopt;

  try {
    unsigned long val = std::stoul(s, nullptr, 16);
    RGBColor c;
    c.r = static_cast<uint8_t>((val >> 16) & 0xFF);
    c.g = static_cast<uint8_t>((val >> 8) & 0xFF);
    c.b = static_cast<uint8_t>(val & 0xFF);
    return c;
  } catch (...) {
    return std::nullopt;
  }
}

static std::string ReadThemeName() {
  const char* home = std::getenv("HOME");
  if (!home) return "terminal";
  std::string p = std::string(home) + "/.local/state/omarchy/current/theme.name";
  std::ifstream f(p);
  if (f.is_open()) {
    std::string name;
    if (std::getline(f, name)) {
      while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ')) {
        name.pop_back();
      }
      return name;
    }
  }
  return "terminal";
}

static std::map<std::string, std::string> ParseColorsTomlDirect(const std::string& path) {
  std::map<std::string, std::string> map;
  std::ifstream f(path);
  if (!f.is_open()) return map;

  std::string line;
  std::regex re(R"(([a-zA-Z0-9_]+)\s*=\s*["'](#?[a-zA-Z0-9]+)["'])");
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;

    std::smatch m;
    if (std::regex_search(line, m, re)) {
      map[m[1].str()] = m[2].str();
    }
  }
  return map;
}

Palette ResolvePalette(const std::string& explicitThemePath) {
  Palette pal;

  std::string colorsToml;
  if (!explicitThemePath.empty()) {
    colorsToml = explicitThemePath;
    pal.source_description = "--theme " + explicitThemePath;
  } else {
    const char* envTheme = std::getenv("SLEEVE_THEME");
    if (envTheme && envTheme[0] != '\0') {
      colorsToml = envTheme;
      pal.source_description = "$SLEEVE_THEME";
    } else {
      const char* home = std::getenv("HOME");
      if (home) {
        std::string cand = std::string(home) + "/.local/state/omarchy/current/theme/colors.toml";
        if (fs::exists(cand)) {
          colorsToml = cand;
          pal.theme_name = ReadThemeName();
          pal.source_description = "omarchy (" + pal.theme_name + ")";
        }
      }
    }
  }

  if (colorsToml.empty() || !fs::exists(colorsToml)) {
    pal.source_description = "terminal palette";
    return pal;
  }

  std::map<std::string, std::string> colorMap;

  // Try omarchy-theme-color --file <colorsToml> --all
  auto res = Process::RunCommand({"omarchy-theme-color", "--file", colorsToml, "--all"}, 2.0);
  if (res.exit_code == 0 && !res.stdout_str.empty()) {
    std::istringstream stream(res.stdout_str);
    std::string line;
    while (std::getline(stream, line)) {
      auto tab = line.find('\t');
      if (tab != std::string::npos) {
        colorMap[line.substr(0, tab)] = line.substr(tab + 1);
      }
    }
  } else {
    // Fallback: direct read
    colorMap = ParseColorsTomlDirect(colorsToml);
  }

  // Alias cascade
  // Accent: accent -> color3
  std::string accentHex;
  if (colorMap.count("accent")) accentHex = colorMap["accent"];
  else if (colorMap.count("color3")) accentHex = colorMap["color3"];
  if (!accentHex.empty()) pal.accent = ParseHex(accentHex);

  // Selection BG: selection_background -> selection -> color8 -> color0 -> background
  std::string selBgHex;
  if (colorMap.count("selection_background")) selBgHex = colorMap["selection_background"];
  else if (colorMap.count("selection")) selBgHex = colorMap["selection"];
  else if (colorMap.count("color8")) selBgHex = colorMap["color8"];
  else if (colorMap.count("color0")) selBgHex = colorMap["color0"];
  else if (colorMap.count("background")) selBgHex = colorMap["background"];
  if (!selBgHex.empty()) pal.selection_bg = ParseHex(selBgHex);

  // Selection FG: selection_foreground -> bright_foreground -> color15 -> foreground
  std::string selFgHex;
  if (colorMap.count("selection_foreground")) selFgHex = colorMap["selection_foreground"];
  else if (colorMap.count("bright_foreground")) selFgHex = colorMap["bright_foreground"];
  else if (colorMap.count("color15")) selFgHex = colorMap["color15"];
  else if (colorMap.count("foreground")) selFgHex = colorMap["foreground"];
  if (!selFgHex.empty()) pal.selection_fg = ParseHex(selFgHex);

  return pal;
}

} // namespace Sleeve::Theme
