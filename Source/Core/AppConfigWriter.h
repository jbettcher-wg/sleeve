// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include "Record.h"

namespace Sleeve::AppConfigWriter {

// Validate if an emulator config option name is valid
bool IsValidConfigOption(std::string_view name);

// Generate merged JSON content for an app's AppConfig/<name>.json
// Preserves non-managed keys in "Config" and preserves all other top-level objects
std::string GenerateAppConfigContent(const Record::AppRecord& record, const std::string& existingContent = "");

// Write or preview the AppConfig for an app
// Returns true on success. If dryRun is true, does not write to disk.
// If outDiff is provided, writes unified diff of changes.
bool WriteAppConfig(Record::AppRecord& record, bool dryRun = false, std::string* outDiff = nullptr);

// Generate updated Config.json content updating RootFS
std::string GenerateUserConfigRootFS(const std::string& newRootfs, const std::string& existingContent = "");

// Write or preview updated Config.json
bool SetUserConfigRootFS(const std::string& newRootfs, bool dryRun = false, std::string* outDiff = nullptr);

} // namespace Sleeve::AppConfigWriter
