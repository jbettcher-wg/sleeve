// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <optional>
#include "Record.h"

namespace Sleeve::Generate {

struct GeneratedOutputs {
  std::string launcher_path;
  std::string launcher_content;

  bool has_desktop {false};
  std::string desktop_path;
  std::string desktop_content;

  bool has_appconfig {true};
  std::string appconfig_path;
  std::string appconfig_content;
};

// Generate launcher script and desktop entry from record
GeneratedOutputs GenerateFiles(const Record::AppRecord& record);

// Import a foreign launcher into an AppRecord
std::optional<Record::AppRecord> ImportForeignLauncher(const std::string& launcherPath, const std::string& defaultName = "");

} // namespace Sleeve::Generate
