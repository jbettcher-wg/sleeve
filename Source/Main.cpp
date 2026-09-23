// SPDX-License-Identifier: MIT
#include "App.h"
#include "Paths.h"
#include "Scanner.h"
#include "Shapes.h"
#include "Record.h"
#include "Generate.h"
#include "FileWriter.h"
#include "Diff.h"
#include "RootFS.h"
#include "Theme.h"
#include "ElfInspect.h"
#include "AppConfigWriter.h"
#include "Health.h"
#include "Backend.h"

#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <map>

namespace fs = std::filesystem;
using namespace Sleeve;

static void PrintHelp() {
  const auto& backend = Backend::GetActiveBackend();
  std::cout << "sleeve - emulated app manager (" << backend.displayName << ")\n\n"
            << "Usage:\n"
            << "  sleeve                               Launch terminal UI\n"
            << "  sleeve scan [DIR...] [--json]        Scan for " << backend.archName << " apps (with DIR: only there)\n"
            << "  sleeve add PATH [--name N] [--yes]   Add an app from directory or binary\n"
            << "  sleeve import PATH [--name N]        Import a foreign launcher into a managed record\n"
            << "  sleeve show NAME [--json]            Show app details\n"
            << "  sleeve list [--json]                 List managed apps\n"
            << "  sleeve set NAME [OPTIONS...]         Configure app settings (rootfs=, cache=, fusion=, startupnotify=, ...)\n"
            << "  sleeve wrap NAME [--dry-run] [--yes] Generate launcher, desktop, and AppConfig files\n"
            << "  sleeve check NAME [--version|--sec N] Run health check on app\n"
            << "  sleeve rootfs list [--json]          List discovered rootfses\n"
            << "  sleeve rootfs use NAME [--dry-run]   Set default RootFS in Config.json\n"
            << "  sleeve theme                         Show resolved theme palette\n\n"
            << "Global options:\n"
            << "  --backend ID                         Target emulator backend (powerarm, fastppcx86)\n"
            << "  --theme FILE                         Path to custom colors.toml\n"
            << "  --json                               Output results in JSON format\n"
            << "  --dry-run                            Show diffs without writing files\n"
            << "  --yes, -y                            Non-interactive execution\n"
            << "  --help, -h                           Show this help message\n";
}

int main(int argc, char** argv) {
  const char* envBackend = std::getenv("SLEEVE_BACKEND");
  if (envBackend && envBackend[0] != '\0') {
    Backend::SetActiveBackend(envBackend);
  }

  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.push_back(argv[i]);
  }

  std::string themeFile;
  bool jsonOutput = false;
  bool dryRun = false;
  bool assumeYes = false;

  std::vector<std::string> filteredArgs;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--help" || args[i] == "-h") {
      PrintHelp();
      return 0;
    } else if (args[i] == "--backend" && i + 1 < args.size()) {
      if (!Backend::SetActiveBackend(args[++i])) {
        std::cerr << "Unknown backend: " << args[i] << " (valid: powerarm, fastppcx86)\n";
        return 1;
      }
    } else if (args[i].rfind("--backend=", 0) == 0) {
      std::string id = args[i].substr(10);
      if (!Backend::SetActiveBackend(id)) {
        std::cerr << "Unknown backend: " << id << " (valid: powerarm, fastppcx86)\n";
        return 1;
      }
    } else if (args[i] == "--json") {
      jsonOutput = true;
    } else if (args[i] == "--dry-run") {
      dryRun = true;
    } else if (args[i] == "--yes" || args[i] == "-y") {
      assumeYes = true;
    } else if (args[i] == "--theme" && i + 1 < args.size()) {
      themeFile = args[++i];
    } else {
      filteredArgs.push_back(args[i]);
    }
  }

  if (filteredArgs.empty()) {
    // Launch TUI
    return Tui::RunTui(themeFile);
  }

  std::string cmd = filteredArgs[0];

  // 1. SCAN
  if (cmd == "scan") {
    Scanner::ScanOptions opts;
    for (size_t i = 1; i < filteredArgs.size(); ++i) {
      opts.search_dirs.push_back(filteredArgs[i]);
    }

    auto result = Scanner::RunScan(opts);

    if (jsonOutput) {
      std::cout << "{\n  \"apps\": [\n";
      for (size_t i = 0; i < result.apps.size(); ++i) {
        const auto& a = result.apps[i];
        std::cout << "    {\n"
                  << "      \"name\": \"" << a.name << "\",\n"
                  << "      \"title\": \"" << a.title << "\",\n"
                  << "      \"shape\": \"" << Shapes::ShapeToString(a.shape) << "\",\n"
                  << "      \"exe\": \"" << a.exe_path << "\",\n"
                  << "      \"origin\": \"" << a.origin << "\",\n"
                  << "      \"version\": \"" << a.version << "\"\n"
                  << "    }" << (i + 1 < result.apps.size() ? "," : "") << "\n";
      }
      std::cout << "  ],\n  \"foreign_launchers\": [\n";
      for (size_t i = 0; i < result.foreign_launchers.size(); ++i) {
        const auto& f = result.foreign_launchers[i];
        std::cout << "    {\n"
                  << "      \"name\": \"" << f.name << "\",\n"
                  << "      \"path\": \"" << f.path << "\",\n"
                  << "      \"target_exe\": \"" << f.target_exe << "\"\n"
                  << "    }" << (i + 1 < result.foreign_launchers.size() ? "," : "") << "\n";
      }
      std::cout << "  ],\n  \"searched_dirs\": [";
      for (size_t i = 0; i < result.searched_dirs.size(); ++i) {
        std::cout << (i > 0 ? ", " : "") << "\"" << result.searched_dirs[i] << "\"";
      }
      std::cout << "],\n  \"explicit_dirs\": " << (result.explicit_dirs ? "true" : "false") << ",\n"
                << "  \"stats\": {\n"
                << "    \"files_probed\": " << result.stats.files_probed << ",\n"
                << "    \"duplicates_collapsed\": " << result.stats.duplicates_collapsed << ",\n"
                << "    \"elapsed_seconds\": " << result.stats.elapsed_seconds << "\n"
                << "  }\n}\n";
    } else {
      std::cout << "Scan found " << result.apps.size() << " apps in "
                << std::fixed << std::setprecision(1) << result.stats.elapsed_seconds << " s ("
                << result.stats.files_probed << " files probed):\n\n";

      // Group by where each row came from, so a row from the overlay is never read as a
      // result of scanning the directory that was asked about.
      std::vector<std::string> originOrder;
      std::map<std::string, std::vector<const Shapes::AppCandidate*>> byOrigin;
      for (const auto& a : result.apps) {
        std::string origin = a.origin.empty() ? std::string("(unknown)") : a.origin;
        if (!byOrigin.count(origin)) originOrder.push_back(origin);
        byOrigin[origin].push_back(&a);
      }

      for (const auto& origin : originOrder) {
        std::cout << "  " << Paths::ContractUser(origin) << ":\n";
        for (const auto* a : byOrigin[origin]) {
          std::cout << "    • " << std::left << std::setw(16) << a->name
                    << std::setw(28) << a->title
                    << std::setw(12) << Shapes::ShapeToString(a->shape)
                    << Paths::ContractUser(a->exe_path) << "\n";
        }
      }

      if (result.apps.empty()) {
        std::cout << "  No " << Backend::GetActiveBackend().archName << " apps found under:\n";
        for (const auto& d : result.searched_dirs) {
          std::cout << "    " << Paths::ContractUser(d) << "\n";
        }
        if (result.explicit_dirs) {
          std::cout << "  (only the directories you named were searched; run 'sleeve scan' with no\n"
                    << "   arguments to also search the standard locations and the rootfs overlays)\n";
        }
      } else if (result.explicit_dirs) {
        std::cout << "\n  Only the directories named on the command line were searched.\n";
      }

      if (!result.foreign_launchers.empty()) {
        std::cout << "\nForeign launchers (~/.local/bin):\n";
        for (const auto& f : result.foreign_launchers) {
          std::cout << "  ? " << std::left << std::setw(16) << f.name
                    << "-> " << Paths::ContractUser(f.target_exe) << "\n";
        }
      }
    }
    return 0;
  }

  // 2. ADD
  if (cmd == "add") {
    if (filteredArgs.size() < 2) {
      std::cerr << "Error: 'sleeve add' requires a path argument\n";
      return 1;
    }
    std::string path = filteredArgs[1];
    std::string customName;
    for (size_t i = 2; i < filteredArgs.size(); ++i) {
      if (filteredArgs[i] == "--name" && i + 1 < filteredArgs.size()) {
        customName = filteredArgs[++i];
      }
    }

    std::string defaultRootfs;
    auto rfs = RootFS::DiscoverRootFSes();
    if (!rfs.rootfses.empty()) {
      defaultRootfs = rfs.rootfses[0].base_path;
    }

    if (fs::is_directory(path)) {
      // Use the same test the scanner uses. The old one required has_interp, which the
      // 64-byte probe never sets, so every position-independent executable -- which is to
      // say almost every modern binary -- was invisible to `add` while `scan` found it.
      std::vector<std::string> binaries;
      std::error_code walkEc;
      for (const auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, walkEc)) {
        std::error_code ec;
        if (!entry.is_regular_file(ec) || ec) continue;
        auto probe = ElfInspect::ProbeFile(entry.path().string());
        if (ElfInspect::IsTargetBinary(probe)) {
          binaries.push_back(entry.path().string());
        }
      }
      // Prefer the stable alias over the version directory, as the scanner does.
      std::string stablePath = Paths::PreferStableSymlinkPath(path);
      if (stablePath != path) {
        for (auto& b : binaries) {
          if (b.rfind(path + "/", 0) == 0) b = stablePath + b.substr(path.size());
        }
        path = stablePath;
      }
      auto cand = Shapes::DetectDirectoryShape(path, binaries);
      if (cand) {
        if (!customName.empty()) cand->name = customName;
        if (!Record::IsValidAppName(cand->name)) {
          std::cerr << "Error: '" << cand->name << "' is not usable as an app name; pass --name\n";
          return 1;
        }
        auto rec = Record::CreateFromCandidate(*cand, defaultRootfs);
        if (!Record::SaveRecord(rec)) {
          std::cerr << "Error: could not write record for '" << rec.name << "'\n";
          return 1;
        }
        std::cout << "Added app '" << rec.name << "' (" << rec.title << ", shape: " << rec.shape << ")\n";
        return 0;
      }
    }

    auto details = ElfInspect::InspectTarget(path);
    if (details) {
      auto cand = Shapes::DetectBinaryShape(path, *details);
      if (cand) {
        if (!customName.empty()) cand->name = customName;
        if (!Record::IsValidAppName(cand->name)) {
          std::cerr << "Error: '" << cand->name << "' is not usable as an app name; pass --name\n";
          return 1;
        }
        auto rec = Record::CreateFromCandidate(*cand, defaultRootfs);
        if (!Record::SaveRecord(rec)) {
          std::cerr << "Error: could not write record for '" << rec.name << "'\n";
          return 1;
        }
        std::cout << "Added binary app '" << rec.name << "' (shape: " << rec.shape << ")\n";
        return 0;
      }
    }

    std::cerr << "Error: could not detect a " << Backend::GetActiveBackend().archName
              << " application at " << path << "\n";
    return 1;
  }

  // 3. IMPORT
  if (cmd == "import") {
    if (filteredArgs.size() < 2) {
      std::cerr << "Error: 'sleeve import' requires launcher path\n";
      return 1;
    }
    std::string launcherPath = filteredArgs[1];
    std::string customName;
    for (size_t i = 2; i < filteredArgs.size(); ++i) {
      if (filteredArgs[i] == "--name" && i + 1 < filteredArgs.size()) {
        customName = filteredArgs[++i];
      }
    }

    auto rec = Generate::ImportForeignLauncher(launcherPath, customName);
    if (!rec) {
      std::cerr << "Error: failed to import launcher from " << launcherPath << "\n";
      return 1;
    }

    Record::SaveRecord(*rec);
    std::cout << "Imported launcher '" << launcherPath << "' as app '" << rec->name
              << "' (" << rec->title << ", shape: " << rec->shape << ")\n";
    return 0;
  }

  // 4. SHOW
  if (cmd == "show") {
    if (filteredArgs.size() < 2) {
      std::cerr << "Error: 'sleeve show' requires app name\n";
      return 1;
    }
    std::string name = filteredArgs[1];
    auto rec = Record::LoadRecord(name);
    if (!rec) {
      std::cerr << "Error: app '" << name << "' not found\n";
      return 1;
    }

    if (jsonOutput) {
      std::cout << Record::EmitJson(*rec);
    } else {
      std::cout << "App: " << rec->name << " (" << rec->title << ")\n"
                << "  Shape:     " << rec->shape << "\n"
                << "  Exe:       " << rec->GetResolvedExePath() << "\n"
                << "  RootFS:    " << (rec->rootfs.empty() ? "(none)" : rec->rootfs) << "\n"
                << "  Args:      " << Record::JoinArgsForEditing(rec->args) << "\n"
                << "  Desktop:   " << (rec->desktop.enabled ? "enabled" : "disabled") << "\n";
      if (rec->desktop.enabled) {
        std::cout << "  WMClass:   " << rec->desktop.wmclass << "\n"
                  << "  StartupNotify: " << (rec->desktop.startup_notify ? "true" : "false") << "\n";
      }
      std::cout << "  AppConfig: ";
      for (const auto& [k, v] : rec->appconfig) {
        std::cout << k << "=" << v << " ";
      }
      std::cout << "\n";
      if (!rec->health.status.empty()) {
        std::cout << "  Health:    " << rec->health.status << " (" << rec->health.when << ")\n";
        for (const auto& n : rec->health.notes) {
          std::cout << "             • " << n << "\n";
        }
      }
    }
    return 0;
  }

  // 5. LIST
  if (cmd == "list") {
    auto records = Record::ListRecords();
    if (jsonOutput) {
      std::cout << "[\n";
      for (size_t i = 0; i < records.size(); ++i) {
        std::cout << "  {\"name\": \"" << records[i].name << "\", \"title\": \"" << records[i].title
                  << "\", \"shape\": \"" << records[i].shape << "\"}"
                  << (i + 1 < records.size() ? "," : "") << "\n";
      }
      std::cout << "]\n";
    } else if (records.empty()) {
      const auto& backend = Backend::GetActiveBackend();
      std::cout << "No apps managed under " << backend.displayName << ".\n"
                << "Records live in " << Paths::ContractUser(Paths::GetRecordDir()) << "; "
                << "run 'sleeve scan' to find apps to add.\n";
    } else {
      for (const auto& r : records) {
        std::cout << std::left << std::setw(16) << r.name
                  << std::setw(28) << r.title
                  << std::setw(12) << r.shape
                  << Paths::ContractUser(r.GetResolvedExePath()) << "\n";
      }
    }
    return 0;
  }

  // 6. SET
  if (cmd == "set") {
    if (filteredArgs.size() < 2) {
      std::cerr << "Error: 'sleeve set' requires app name\n";
      return 1;
    }
    std::string name = filteredArgs[1];
    auto rec = Record::LoadRecord(name);
    if (!rec) {
      std::cerr << "Error: app '" << name << "' not found\n";
      return 1;
    }

    bool wrapAfter = false;
    for (size_t i = 2; i < filteredArgs.size(); ++i) {
      const std::string& opt = filteredArgs[i];
      if (opt == "--wrap") {
        wrapAfter = true;
        continue;
      }
      auto eq = opt.find('=');
      if (eq == std::string::npos) {
        std::cerr << "Warning: ignoring option without '=': " << opt << "\n";
        continue;
      }
      std::string key = opt.substr(0, eq);
      std::string val = opt.substr(eq + 1);

      if (key == "rootfs") {
        auto resolved = RootFS::ResolveRootFSName(val);
        if (!resolved.ok) {
          std::cerr << "Error: " << resolved.error << "\n";
          return 1;
        }
        rec->rootfs = resolved.path;
        std::cout << "Set rootfs = " << rec->rootfs << "\n";
      } else if (key == "startupnotify") {
        rec->desktop.startup_notify = (val == "on" || val == "1" || val == "true");
        std::cout << "Set startup_notify = " << (rec->desktop.startup_notify ? "true" : "false") << "\n";
      } else if (key == "cache") {
        rec->appconfig["EnableCodeCachingWIP"] = (val == "on" || val == "1" || val == "true") ? "1" : "0";
        std::cout << "Set EnableCodeCachingWIP = " << rec->appconfig["EnableCodeCachingWIP"] << "\n";
      } else if (key == "scope") {
        rec->appconfig["CodeCacheScope"] = val;
        std::cout << "Set CodeCacheScope = " << val << "\n";
      } else if (key == "fusion") {
        rec->appconfig["DisableCmpBranchFusion"] = (val == "on" || val == "1" || val == "true") ? "0" : "1";
        std::cout << "Set DisableCmpBranchFusion = " << rec->appconfig["DisableCmpBranchFusion"] << "\n";
      } else if (key == "stats") {
        rec->appconfig["ProfileStats"] = (val == "on" || val == "1" || val == "true") ? "1" : "0";
        std::cout << "Set ProfileStats = " << rec->appconfig["ProfileStats"] << "\n";
      } else if (key == "mangohud") {
        rec->mangohud.enabled = (val == "on" || val == "1" || val == "true");
        std::cout << "Set mangohud = " << (rec->mangohud.enabled ? "on" : "off") << "\n";
      } else if (key == "emulator") {
        if (val == "stable" || val.empty()) {
          rec->emulator = std::nullopt;
          std::cout << "Set emulator = stable\n";
        } else {
          rec->emulator = val;
          std::cout << "Set emulator = " << val << "\n";
        }
      } else if (key == "arg+") {
        rec->args.push_back(val);
        std::cout << "Added arg: " << val << "\n";
      } else if (key == "arg-") {
        auto it = std::remove(rec->args.begin(), rec->args.end(), val);
        if (it != rec->args.end()) {
          rec->args.erase(it, rec->args.end());
          std::cout << "Removed arg: " << val << "\n";
        }
      } else if (key == "env+") {
        auto subEq = val.find('=');
        if (subEq != std::string::npos) {
          rec->env[val.substr(0, subEq)] = val.substr(subEq + 1);
          std::cout << "Added env: " << val << "\n";
        }
      } else if (key == "env-") {
        rec->env.erase(val);
        std::cout << "Removed env: " << val << "\n";
      } else if (AppConfigWriter::IsValidConfigOption(key)) {
        rec->appconfig[key] = val;
        std::cout << "Set " << key << " = " << val << "\n";
      } else {
        std::cerr << "Warning: unknown option key: " << key << "\n";
      }
    }

    Record::SaveRecord(*rec);
    std::cout << "Updated record for '" << name << "'.\n";

    if (wrapAfter) {
      std::cout << "Applying changes via wrap...\n";
      auto gen = Generate::GenerateFiles(*rec);
      FileWriter::AtomicWrite(gen.launcher_path, gen.launcher_content, 0755);
      rec->generated[gen.launcher_path] = FileWriter::ComputeSha256(gen.launcher_content);
      if (gen.has_desktop) {
        FileWriter::AtomicWrite(gen.desktop_path, gen.desktop_content, 0644);
        rec->generated[gen.desktop_path] = FileWriter::ComputeSha256(gen.desktop_content);
      }
      if (gen.has_appconfig) {
        FileWriter::AtomicWrite(gen.appconfig_path, gen.appconfig_content, 0644);
        rec->generated[gen.appconfig_path] = FileWriter::ComputeSha256(gen.appconfig_content);
      }
      Record::SaveRecord(*rec);
      std::cout << "Wrote updated wrapper files for '" << name << "'.\n";
    }
    return 0;
  }

  // 7. WRAP
  if (cmd == "wrap") {
    if (filteredArgs.size() < 2) {
      std::cerr << "Error: 'sleeve wrap' requires app name\n";
      return 1;
    }
    std::string name = filteredArgs[1];
    auto rec = Record::LoadRecord(name);
    if (!rec) {
      std::cerr << "Error: app '" << name << "' not found\n";
      return 1;
    }

    auto gen = Generate::GenerateFiles(*rec);

    // 1. Launcher
    auto lStatus = FileWriter::CheckStatus(gen.launcher_path, rec->generated.count(gen.launcher_path) ? rec->generated.at(gen.launcher_path) : "");
    std::string lDiff = Diff::UnifiedDiff(lStatus.existing_content, gen.launcher_content,
                                          gen.launcher_path + " (current)", gen.launcher_path + " (new)");

    // 2. Desktop
    std::string dDiff;
    FileWriter::FileStatus dStatus;
    if (gen.has_desktop) {
      dStatus = FileWriter::CheckStatus(gen.desktop_path, rec->generated.count(gen.desktop_path) ? rec->generated.at(gen.desktop_path) : "");
      dDiff = Diff::UnifiedDiff(dStatus.existing_content, gen.desktop_content,
                                gen.desktop_path + " (current)", gen.desktop_path + " (new)");
    }

    // 3. AppConfig
    std::string acDiff;
    FileWriter::FileStatus acStatus;
    if (gen.has_appconfig) {
      acStatus = FileWriter::CheckStatus(gen.appconfig_path, rec->generated.count(gen.appconfig_path) ? rec->generated.at(gen.appconfig_path) : "");
      acDiff = Diff::UnifiedDiff(acStatus.existing_content, gen.appconfig_content,
                                 gen.appconfig_path + " (current)", gen.appconfig_path + " (new)");
    }

    if (dryRun || !assumeYes) {
      std::cout << "=== Launcher: " << gen.launcher_path << " ===\n" << lDiff << "\n";
      if (gen.has_desktop) {
        std::cout << "=== Desktop: " << gen.desktop_path << " ===\n" << dDiff << "\n";
      }
      if (gen.has_appconfig) {
        std::cout << "=== AppConfig: " << gen.appconfig_path << " ===\n" << acDiff << "\n";
      }
      if (dryRun) {
        return 0;
      }
    }

    if (lStatus.verdict == FileWriter::FileVerdict::Foreign) {
      std::cout << "Warning: " << gen.launcher_path << " is a foreign file. Moving to .pre-sleeve first.\n";
      FileWriter::MoveAside(gen.launcher_path);
    }
    if (gen.has_desktop && dStatus.verdict == FileWriter::FileVerdict::Foreign) {
      FileWriter::MoveAside(gen.desktop_path);
    }

    FileWriter::AtomicWrite(gen.launcher_path, gen.launcher_content, 0755);
    rec->generated[gen.launcher_path] = FileWriter::ComputeSha256(gen.launcher_content);

    if (gen.has_desktop) {
      FileWriter::AtomicWrite(gen.desktop_path, gen.desktop_content, 0644);
      rec->generated[gen.desktop_path] = FileWriter::ComputeSha256(gen.desktop_content);
    }

    if (gen.has_appconfig) {
      FileWriter::AtomicWrite(gen.appconfig_path, gen.appconfig_content, 0644);
      rec->generated[gen.appconfig_path] = FileWriter::ComputeSha256(gen.appconfig_content);
    }

    Record::SaveRecord(*rec);
    std::cout << "Wrote launcher:  " << gen.launcher_path << "\n";
    if (gen.has_desktop) {
      std::cout << "Wrote desktop:   " << gen.desktop_path << "\n";
    }
    if (gen.has_appconfig) {
      std::cout << "Wrote AppConfig: " << gen.appconfig_path << "\n";
    }
    return 0;
  }

  // 8. CHECK
  if (cmd == "check") {
    if (filteredArgs.size() < 2) {
      std::cerr << "Error: 'sleeve check' requires app name\n";
      return 1;
    }
    std::string name = filteredArgs[1];
    auto rec = Record::LoadRecord(name);
    if (!rec) {
      std::cerr << "Error: app '" << name << "' not found\n";
      return 1;
    }

    Health::CheckOptions hopt;
    hopt.mode = Health::CheckMode::Version;
    hopt.timeout_seconds = 20.0;
    hopt.wmclass = rec->desktop.wmclass;

    for (size_t i = 2; i < filteredArgs.size(); ++i) {
      if (filteredArgs[i] == "--version") {
        hopt.mode = Health::CheckMode::Version;
      } else if (filteredArgs[i] == "--seconds" && i + 1 < filteredArgs.size()) {
        hopt.mode = Health::CheckMode::Timed;
        hopt.timeout_seconds = std::stod(filteredArgs[++i]);
      }
    }

    std::cout << "Checking " << name << " (" << (hopt.mode == Health::CheckMode::Version ? "--version" : "timed") << ")...\n";
    auto res = Health::RunCheck(*rec, hopt);

    if (jsonOutput) {
      std::cout << "{\n"
                << "  \"name\": \"" << name << "\",\n"
                << "  \"status\": \"" << res.status << "\",\n"
                << "  \"summary\": \"" << res.summary << "\",\n"
                << "  \"exit_code\": " << res.exit_code << ",\n"
                << "  \"elapsed_seconds\": " << res.elapsed_seconds << ",\n"
                << "  \"log_path\": \"" << res.log_path << "\"\n"
                << "}\n";
    } else {
      std::string statusGlyph = (res.status == "ok") ? "✓" : "✗";
      std::cout << statusGlyph << " " << res.summary << " ("
                << std::fixed << std::setprecision(1) << res.elapsed_seconds << " s, exit " << res.exit_code << ")\n";
      for (const auto& note : res.notes) {
        std::cout << "  • " << note << "\n";
      }
      if (!res.log_path.empty()) {
        std::cout << "  Log: " << res.log_path << "\n";
      }
    }

    return (res.status == "ok") ? 0 : 1;
  }

  // 9. ROOTFS
  if (cmd == "rootfs") {
    if (filteredArgs.size() >= 3 && filteredArgs[1] == "use") {
      // A name that does not resolve does not fail loudly at run time: the emulator falls
      // back to host binaries. Refuse it here instead.
      auto resolved = RootFS::ResolveRootFSName(filteredArgs[2]);
      if (!resolved.ok) {
        std::cerr << "Error: " << resolved.error << "\n";
        return 1;
      }
      std::string targetRootfs = resolved.path;
      std::string outDiff;
      bool ok = AppConfigWriter::SetUserConfigRootFS(targetRootfs, dryRun, &outDiff);
      if (dryRun || !assumeYes) {
        std::cout << "=== " << Paths::GetUserConfigPath() << " ===\n" << outDiff << "\n";
      }
      if (!dryRun && ok) {
        std::cout << "Updated Config.json default RootFS to: " << targetRootfs << "\n";
      }
      return ok ? 0 : 1;
    }

    auto rfs = RootFS::DiscoverRootFSes();
    if (jsonOutput) {
      std::cout << "{\n  \"env_rootfs\": \"" << rfs.env_rootfs << "\",\n"
                << "  \"config_default\": \"" << rfs.config_default_rootfs << "\",\n"
                << "  \"rootfses\": [\n";
      for (size_t i = 0; i < rfs.rootfses.size(); ++i) {
        const auto& r = rfs.rootfses[i];
        std::cout << "    {\n"
                  << "      \"name\": \"" << r.name << "\",\n"
                  << "      \"base_path\": \"" << r.base_path << "\",\n"
                  << "      \"has_overlay\": " << (r.has_overlay ? "true" : "false") << ",\n"
                  << "      \"elf_machine\": " << r.elf_machine << ",\n"
                  << "      \"arch_verified\": " << (r.arch_verified ? "true" : "false") << ",\n"
                  << "      \"package_count\": " << r.package_count << "\n"
                  << "    }" << (i + 1 < rfs.rootfses.size() ? "," : "") << "\n";
      }
      std::cout << "  ]\n}\n";
    } else {
      std::cout << "Default RootFS (Config.json): " << (rfs.config_default_rootfs.empty() ? "(none)" : rfs.config_default_rootfs) << "\n";
      std::cout << "Shell RootFS (env):           " << (rfs.env_rootfs.empty() ? "(unset)" : rfs.env_rootfs) << "\n\n";

      if (rfs.rootfses.empty()) {
        std::cout << "No rootfs found for " << Backend::GetActiveBackend().displayName << " under "
                  << Paths::ContractUser(Paths::GetDataDir() + "/RootFS") << ".\n";
      }
      for (const auto& r : rfs.rootfses) {
        std::cout << "  " << (r.is_config_default ? "▶ " : "  ")
                  << std::left << std::setw(20) << r.name
                  << std::setw(12) << RootFS::FormatBytes(r.base_size_bytes)
                  << std::setw(28) << (r.has_overlay ? "overlay: yes (" + std::to_string(r.package_count) + " pkgs)" : "overlay: none")
                  << (r.arch_verified ? "" : "arch unverified")
                  << "\n";
      }
    }
    return 0;
  }

  // 10. THEME
  if (cmd == "theme") {
    auto pal = Theme::ResolvePalette(themeFile);
    std::cout << "Theme: " << pal.theme_name << " (source: " << pal.source_description << ")\n";
    if (pal.accent) {
      std::cout << "  accent:               " << Theme::ColorToHex(*pal.accent) << "\n";
    } else {
      std::cout << "  accent:               Palette16 yellow\n";
    }
    if (pal.selection_bg) {
      std::cout << "  selection_background: " << Theme::ColorToHex(*pal.selection_bg) << "\n";
    }
    if (pal.selection_fg) {
      std::cout << "  selection_foreground: " << Theme::ColorToHex(*pal.selection_fg) << "\n";
    }
    return 0;
  }

  std::cerr << "Unknown command: " << cmd << "\n";
  PrintHelp();
  return 1;
}
