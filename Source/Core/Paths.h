// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <filesystem>

namespace Sleeve::Paths {

std::string GetConfigDir();
std::string GetDataDir();
std::string GetCacheDir();
std::string GetRecordDir();
std::string GetAppConfigDir();
std::string GetUserConfigPath();
std::string GetSettingsPath();
std::string GetAppsDir(const std::string& configuredAppsDir = "");
std::string GetStableVersion();
std::string GetBinfmtStatus();
std::string GetScriptDir();

std::string ExpandUser(const std::string& path);
std::string ContractUser(const std::string& path);

// If a sibling symlink such as `current` resolves to the same place as `path`, returns the
// symlink's path instead. Version directories move on every update; the symlink does not,
// so a launcher that goes through it survives the update.
std::string PreferStableSymlinkPath(const std::string& path);

} // namespace Sleeve::Paths
