// SPDX-License-Identifier: MIT
#include "RootFS.h"
#include "Paths.h"
#include "Record.h"
#include "Backend.h"
#include "ElfInspect.h"

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

// Walks `relative` inside `base` the way the guest kernel would: an absolute symlink target
// is re-rooted at `base` instead of at the host's /, so `/bin/sh -> /usr/bin/bash` resolves to
// the guest's bash and not the host's. Returns "" when it does not resolve inside the tree.
static std::string ResolveInsideRootFS(const std::string& base, const std::string& relative) {
  std::error_code ec;
  fs::path basePath = fs::path(base);
  fs::path result = basePath;

  std::vector<std::string> pending;
  for (const auto& part : fs::path(relative).relative_path()) {
    pending.push_back(part.string());
  }

  int hops = 0;
  while (!pending.empty()) {
    if (++hops > 64) return "";

    std::string part = pending.front();
    pending.erase(pending.begin());
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (result == basePath) return "";
      result = result.parent_path();
      continue;
    }

    result /= part;
    if (!fs::is_symlink(result, ec)) continue;

    fs::path target = fs::read_symlink(result, ec);
    if (ec || target.empty()) return "";

    std::vector<std::string> expanded;
    for (const auto& p : target.relative_path()) {
      expanded.push_back(p.string());
    }
    result = target.is_absolute() ? basePath : result.parent_path();
    pending.insert(pending.begin(), expanded.begin(), expanded.end());
  }

  // Never report a path that climbed back out of the tree.
  std::string resultStr = result.lexically_normal().string();
  std::string baseStr = basePath.lexically_normal().string();
  if (resultStr.rfind(baseStr, 0) != 0) return "";
  return resultStr;
}

uint16_t DetectRootFSMachine(const std::string& rootfsPath) {
  static const char* kProbePaths[] = {
    "/usr/bin/env",
    "/bin/sh",
    "/usr/bin/sh",
    "/bin/bash",
    "/usr/bin/bash",
    "/usr/bin/ls",
  };

  std::error_code ec;
  for (const char* rel : kProbePaths) {
    std::string resolved = ResolveInsideRootFS(rootfsPath, rel);
    if (resolved.empty()) continue;
    if (!fs::is_regular_file(resolved, ec)) continue;
    auto probe = ElfInspect::ProbeFile(resolved);
    if (probe.machine != 0) return probe.machine;
  }
  return 0;
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

  const auto& backend = Backend::GetActiveBackend();
  std::string envVarName = backend.envPrefix + "ROOTFS";
  const char* envRfs = std::getenv(envVarName.c_str());
  if (envRfs && envRfs[0] != '\0') {
    result.env_rootfs = envRfs;
  }
  result.config_default_rootfs = ReadConfigJsonDefaultRootFS();

  std::vector<std::string> searchDirs = {
    Paths::GetDataDir() + "/RootFS",
    "/usr/share/" + backend.dataSubdir + "/RootFS"
  };

  // Only this backend's stores. A checkout-local RootFS dir belonging to the *other*
  // backend holds a guest of the wrong architecture, and a rootfs that does not resolve
  // falls back to host binaries instead of failing loudly.
  const char* home = std::getenv("HOME");
  if (home) {
    searchDirs.push_back(std::string(home) + "/Development/" + backend.devSubdir + "/RootFS");
  }

  if (!result.env_rootfs.empty()) {
    std::error_code ec;
    fs::path p = Paths::ExpandUser(result.env_rootfs);
    if (fs::exists(p, ec)) {
      if (fs::is_directory(p, ec)) {
        searchDirs.push_back(p.parent_path().string());
      } else {
        searchDirs.push_back(p.parent_path().string());
      }
    }
  }

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

      std::string baseName = filename;
      if (baseName.size() > 5 && baseName.rfind(".sqsh") == baseName.size() - 5) {
        baseName = baseName.substr(0, baseName.size() - 5);
      }

      std::string fullPath = entry.path().string();
      if (seenBases.count(baseName)) continue;

      // Verify rather than infer from the directory it sits in: a tree whose own
      // userspace is the wrong machine is not a rootfs this backend can run.
      uint16_t machine = DetectRootFSMachine(fullPath);
      if (machine != 0 && machine != backend.elfMachine) continue;

      seenBases.insert(baseName);

      RootFSInfo info;
      info.name = baseName;
      info.base_path = fullPath;
      info.base_size_bytes = CalculateDirSize(fullPath);
      info.elf_machine = machine;
      info.arch_verified = (machine == backend.elfMachine);

      std::string overlayPath = sDir + "/" + baseName + "-overlay";
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
        std::string envNorm = Paths::ExpandUser(result.env_rootfs);
        if (envNorm == fullPath || result.env_rootfs == baseName || result.env_rootfs == filename ||
            envNorm == (sDir + "/" + baseName)) {
          info.is_env_default = true;
        }
      }
      if (!result.config_default_rootfs.empty()) {
        std::string cfgNorm = Paths::ExpandUser(result.config_default_rootfs);
        if (cfgNorm == fullPath || result.config_default_rootfs == baseName || result.config_default_rootfs == filename ||
            cfgNorm == (sDir + "/" + baseName)) {
          info.is_config_default = true;
        }
      }

      if (rootfsToApps.count(fullPath)) {
        info.used_by_apps = rootfsToApps[fullPath];
      } else if (rootfsToApps.count(baseName)) {
        info.used_by_apps = rootfsToApps[baseName];
      } else if (rootfsToApps.count(filename)) {
        info.used_by_apps = rootfsToApps[filename];
      }

      result.rootfses.push_back(info);
    }
  }

  return result;
}

ResolvedRootFS ResolveRootFSName(const std::string& nameOrPath) {
  ResolvedRootFS res;
  res.name = nameOrPath;

  const auto& backend = Backend::GetActiveBackend();
  if (nameOrPath.empty()) {
    res.error = "no rootfs given";
    return res;
  }

  std::string expanded = Paths::ExpandUser(nameOrPath);

  auto discovered = DiscoverRootFSes();
  for (const auto& rfs : discovered.rootfses) {
    if (rfs.name == nameOrPath || rfs.base_path == expanded) {
      res.ok = true;
      res.name = rfs.name;
      res.path = rfs.base_path;
      return res;
    }
  }

  // A path outside the search dirs is still usable, as long as it is the right guest.
  std::error_code ec;
  if (expanded.find('/') != std::string::npos && fs::exists(expanded, ec)) {
    uint16_t machine = DetectRootFSMachine(expanded);
    if (machine != 0 && machine != backend.elfMachine) {
      res.error = expanded + " is a " + std::to_string(machine) +
                  "-machine tree; " + backend.displayName + " runs machine " +
                  std::to_string(backend.elfMachine) + " guests";
      return res;
    }
    res.ok = true;
    res.name = fs::path(expanded).filename().string();
    res.path = expanded;
    return res;
  }

  std::string known;
  for (const auto& rfs : discovered.rootfses) {
    known += (known.empty() ? "" : ", ") + rfs.name;
  }
  res.error = "no rootfs '" + nameOrPath + "' for backend " + backend.displayName +
              (known.empty() ? " (none discovered)" : " (known: " + known + ")");
  return res;
}


// --------------------------------------------------------------------------
// Readiness and provisioning
// --------------------------------------------------------------------------

namespace {

// The fetcher that builds a guest. It is self-sufficient -- POWERarm's own pinned
// package snapshot, the overlay, guest pacman and the config write -- so this drives it
// and reimplements none of it.
std::string FetcherBinary() {
  const auto& backend = Backend::GetActiveBackend();
  return backend.displayName + "RootFSFetcher";
}

// The overlay directory the emulator would pick up for a base: "<base>-overlay" when it
// exists (RootFSOverlay::ConfiguredPath).
std::string OverlayPathFor(const std::string& basePath) {
  std::error_code ec;
  std::string candidate = basePath + "-overlay";
  return fs::is_directory(candidate, ec) ? candidate : std::string();
}

} // namespace

ReadinessReport CheckReadiness(const std::string& nameOrPath) {
  const auto& backend = Backend::GetActiveBackend();
  ReadinessReport report;
  report.requested = nameOrPath;

  std::string wanted = nameOrPath;
  if (wanted.empty()) {
    auto discovered = DiscoverRootFSes();
    if (!discovered.env_rootfs.empty()) {
      wanted = discovered.env_rootfs;
    } else if (!discovered.config_default_rootfs.empty()) {
      wanted = discovered.config_default_rootfs;
    } else if (!discovered.rootfses.empty()) {
      wanted = discovered.rootfses.front().base_path;
    } else {
      wanted = backend.defaultRootfsDesktop;
    }
    report.requested = wanted;
  }

  auto resolved = ResolveRootFSName(wanted);
  std::error_code ec;

  if (!resolved.ok) {
    // Discovery drops a tree of the wrong architecture, so "not found" and "found but
    // foreign" arrive here the same way. Tell them apart, because only one of them is
    // fixed by building a guest at that name.
    std::string expanded = Paths::ExpandUser(wanted);
    if (expanded.find('/') != std::string::npos && fs::is_directory(expanded, ec)) {
      report.state = Readiness::WrongArch;
      report.rootfs_path = expanded;
      report.elf_machine = DetectRootFSMachine(expanded);
      report.summary = expanded + " is not a " + backend.archName +
                       " tree (its own userspace reports ELF machine " +
                       std::to_string(report.elf_machine) + "), so " + backend.displayName +
                       " would fall off it into host binaries.";
      report.fix_hint = FetcherBinary() + " build --dest " + expanded + " --force";
      report.auto_fixable = true;
      return report;
    }
    report.state = Readiness::Missing;
    report.summary = "there is no " + backend.archName + " rootfs called '" + wanted +
                     "'; a guest has to be built before anything can run under " +
                     backend.displayName + ".";
    report.fix_hint = FetcherBinary() + " build" +
                      (wanted == backend.defaultRootfsDesktop ? "" : " " + wanted);
    report.auto_fixable = true;
    return report;
  }

  report.rootfs_path = resolved.path;
  report.elf_machine = DetectRootFSMachine(resolved.path);
  if (report.elf_machine != 0 && report.elf_machine != backend.elfMachine) {
    report.state = Readiness::WrongArch;
    report.summary = resolved.path + " reports ELF machine " + std::to_string(report.elf_machine) +
                     ", not the " + std::to_string(backend.elfMachine) + " " + backend.displayName +
                     " runs.";
    report.fix_hint = FetcherBinary() + " build --dest " + resolved.path + " --force";
    report.auto_fixable = true;
    return report;
  }

  report.overlay_path = OverlayPathFor(resolved.path);
  if (report.overlay_path.empty()) {
    report.state = Readiness::NoOverlay;
    report.summary = resolved.path +
                     " has no per-user overlay, so guest pacman has nowhere to install into. "
                     "The base alone is a sysroot, not a desktop guest.";
    report.fix_hint = FetcherBinary() + " overlay " + fs::path(resolved.path).filename().string() +
                      " --manifest <the one that built this base>";
    report.auto_fixable = false;
    return report;
  }

  // An overlay directory can exist, be non-empty, and still be unfinished: the package
  // database lands before the packages do, so a run that died partway leaves
  // /var/lib/pacman full and /usr/bin/pacman missing. Installing into that fails in a way
  // that reads like an emulator problem, so it is checked here instead.
  report.guest_pacman = fs::exists(report.overlay_path + "/usr/bin/pacman", ec);
  if (!report.guest_pacman) {
    report.state = Readiness::OverlayIncomplete;
    report.summary = report.overlay_path +
                     " exists but has no /usr/bin/pacman: a build died partway through, and "
                     "nothing can be installed into it until it is rebuilt.";
    report.fix_hint = FetcherBinary() + " overlay " + fs::path(resolved.path).filename().string() +
                      " --manifest <the one that built this base> --force";
    report.auto_fixable = false;
    return report;
  }

  report.state = Readiness::Ok;
  report.summary = resolved.path + " is a " + backend.archName +
                   " guest with a writable overlay and guest pacman in it.";
  return report;
}

ProvisionOptions ProvisionOptionsFromSettings() {
  ProvisionOptions options;
  options.mirrors = Record::LoadSettings().rootfs_mirrors;
  return options;
}

Process::ProcessOptions BuildProvisionCommand(const ProvisionOptions& options) {
  Process::ProcessOptions opt;
  opt.args.push_back(FetcherBinary());
  opt.args.push_back("build");
  if (!options.name.empty()) opt.args.push_back(options.name);
  if (!options.manifest.empty()) opt.args.push_back("--manifest=" + options.manifest);
  if (!options.dest.empty()) opt.args.push_back("--dest=" + options.dest);
  for (const auto& mirror : options.mirrors) opt.args.push_back("--mirror=" + mirror);
  if (options.force) opt.args.push_back("--force");
  if (!options.set_default) opt.args.push_back("--no-set-default");
  if (options.dry_run) opt.args.push_back("--dry-run");
  // sleeve has already shown the plan and been told yes; the fetcher must not then sit
  // on a prompt that a TUI worker thread has no way to answer.
  opt.args.push_back("-y");
  // A base is ~860 MB of downloads and ~1.9 GB written, on a machine that may be
  // building it over a slow link. An hour is not generous.
  opt.timeout_seconds = options.dry_run ? 120.0 : 7200.0;
  return opt;
}

Process::ProcessOptions BuildOverlayCommand(const ProvisionOptions& options) {
  Process::ProcessOptions opt;
  opt.args.push_back(FetcherBinary());
  opt.args.push_back("overlay");
  if (!options.name.empty()) opt.args.push_back(options.name);
  if (!options.manifest.empty()) opt.args.push_back("--manifest=" + options.manifest);
  for (const auto& mirror : options.mirrors) opt.args.push_back("--mirror=" + mirror);
  if (options.force) opt.args.push_back("--force");
  if (options.dry_run) opt.args.push_back("--dry-run");
  opt.args.push_back("-y");
  opt.timeout_seconds = options.dry_run ? 120.0 : 3600.0;
  return opt;
}

namespace {

Process::RunOutcome RunFetcher(const Process::ProcessOptions& opt, const Process::Runner& run) {
  Process::RunOutcome outcome;
  outcome.command = Process::Describe(opt);
  auto res = Process::Execute(run, opt);
  outcome.output = res.stdout_str + res.stderr_str;
  outcome.ok = (res.exit_code == 0 && !res.timed_out);
  if (!outcome.ok) {
    outcome.error = res.timed_out ? "the build timed out"
                                  : (opt.args.empty() ? "nothing to run"
                                                      : opt.args[0] + " exited " +
                                                            std::to_string(res.exit_code));
  }
  return outcome;
}

} // namespace

Process::RunOutcome Provision(const ProvisionOptions& options, const Process::Runner& run) {
  return RunFetcher(BuildProvisionCommand(options), run);
}

Process::RunOutcome ProvisionOverlay(const ProvisionOptions& options, const Process::Runner& run) {
  return RunFetcher(BuildOverlayCommand(options), run);
}

} // namespace Sleeve::RootFS
