// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include "ElfInspect.h"

namespace Sleeve::Shapes {

enum class ShapeType {
  Electron,
  Gecko,
  Game,
  Runtime,
  Pacman,
  Cli,
  Unknown,
};

std::string ShapeToString(ShapeType shape);
ShapeType StringToShape(const std::string& str);

struct AppCandidate {
  std::string name;
  std::string title;
  ShapeType shape {ShapeType::Unknown};
  std::string dir;          // Root directory of the app
  std::string exe_path;     // Path to main executable
  std::string version;
  std::string icon_path;
  std::string wmclass;
  std::string categories;
  std::string mimetypes;
  std::string exec_field {"%F"};
  std::string cwd {"launcher"}; // "launcher" or "install"
  std::vector<std::string> default_args;
  std::map<std::string, std::string> default_env;
  std::map<std::string, bool> presets;
  bool desktop_enabled {true};
  std::string pacman_package;
  std::string rootfs_base;
};

// Detect shape and details from a directory containing AArch64 executable(s)
std::optional<AppCandidate> DetectDirectoryShape(const std::string& dirPath, const std::vector<std::string>& aarch64Binaries);

// Detect shape from a lone AArch64 binary
std::optional<AppCandidate> DetectBinaryShape(const std::string& binaryPath, const ElfInspect::ElfDetails& details);

// Detect shape from a pacman installed package directory in an overlay
std::optional<AppCandidate> DetectPacmanPackage(const std::string& overlayRoot, const std::string& pkgDescPath);

} // namespace Sleeve::Shapes
