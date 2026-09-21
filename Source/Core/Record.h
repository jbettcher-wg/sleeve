// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include "Shapes.h"

namespace Sleeve::Record {

struct SourceInfo {
  std::string kind {"dir"}; // "dir", "file", "pacman"
  std::string dir;
  std::string current;
  std::vector<std::string> versions;
  std::string origin;
  std::string pacman_package;
};

struct MangoHudInfo {
  bool enabled {false};
  std::string config {"fps,frametime,cpu_stats,gpu_stats,fex_stats"};
};

struct DesktopInfo {
  bool enabled {true};
  std::string name;
  std::string comment;
  std::string wmclass;
  std::string icon;
  std::string categories;
  std::string mimetypes;
  std::string exec_field {"%F"};
};

struct HealthInfo {
  std::string when;
  std::string mode;
  std::string status;
  std::vector<std::string> unimplemented;
  std::vector<std::string> notes;
};

struct AppRecord {
  int format {1};
  std::string name;
  std::string title;
  std::string shape {"unknown"};
  SourceInfo source;
  std::string exe;
  std::string cwd {"launcher"}; // "launcher" or "install"
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::string rootfs;
  std::optional<std::string> emulator;
  std::map<std::string, std::string> appconfig;
  MangoHudInfo mangohud;
  DesktopInfo desktop;
  std::map<std::string, bool> presets;
  HealthInfo health;
  std::map<std::string, std::string> generated; // path -> sha256

  // Helpers
  std::string GetResolvedExePath() const;
  std::string GetResolvedIconPath() const;
};

struct Settings {
  std::string apps_dir;
  std::vector<std::string> scan_dirs;
  std::string last_theme;
};

AppRecord CreateFromCandidate(const Shapes::AppCandidate& cand, const std::string& defaultRootfs);

std::optional<AppRecord> LoadRecord(const std::string& name);
bool SaveRecord(const AppRecord& record);
std::vector<AppRecord> ListRecords();
bool DeleteRecord(const std::string& name);

Settings LoadSettings();
bool SaveSettings(const Settings& settings);

std::string EmitJson(const AppRecord& record);

} // namespace Sleeve::Record
