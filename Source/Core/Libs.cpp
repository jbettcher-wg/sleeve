// SPDX-License-Identifier: MIT
#include "Libs.h"

#include "Backend.h"
#include "ElfInspect.h"
#include "Paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <elf.h>
#include <deque>
#include <filesystem>
#include <fnmatch.h>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace Sleeve::Libs {

namespace fs = std::filesystem;

namespace {

// The prefixes the emulator's overlay layers (RootFSOverlay.cpp): everything the guest's
// package manager installs into or keeps state in. A path outside them is not layered at
// all, so the overlay never shadows it.
constexpr const char* kOwnedPrefixes[] = {
  "/usr", "/etc", "/opt", "/var/lib/pacman", "/var/cache/pacman", "/var/log/pacman.log",
};

// Package-manager state: overlay, then base, and never the host. A guest pacman that
// fell through to /var/lib/pacman would read the host's own package database.
constexpr const char* kNoFallthroughPrefixes[] = {
  "/var/lib/pacman", "/var/cache/pacman", "/etc/pacman.conf", "/etc/pacman.d", "/var/log/pacman.log",
};

// The directories glibc searches when nothing else named one. aarch64 uses the plain
// lib names; lib64 is carried too because a rootfs may have it as a symlink and a
// candidate that does not exist costs one lstat.
constexpr const char* kDefaultLibDirs[] = {"/lib", "/usr/lib", "/lib64", "/usr/lib64"};

constexpr int kMaxSymlinkHops = 64;
constexpr size_t kMaxNeededBy = 6;

bool HasPathPrefix(const std::string& path, const std::string& prefix) {
  if (path.size() < prefix.size()) return false;
  if (path.compare(0, prefix.size(), prefix) != 0) return false;
  return path.size() == prefix.size() || path[prefix.size()] == '/';
}

bool IsOwnedPath(const std::string& guestPath) {
  for (const char* p : kOwnedPrefixes) {
    if (HasPathPrefix(guestPath, p)) return true;
  }
  return false;
}

bool IsNoFallthroughPath(const std::string& guestPath) {
  for (const char* p : kNoFallthroughPrefixes) {
    if (HasPathPrefix(guestPath, p)) return true;
  }
  return false;
}

bool LinkExists(const fs::path& p) {
  std::error_code ec;
  auto st = fs::symlink_status(p, ec);
  return !ec && fs::exists(st);
}

std::vector<std::string> SplitComponents(const std::string& path) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < path.size()) {
    size_t slash = path.find('/', start);
    std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) parts.push_back(part);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return parts;
}

struct WalkHit {
  bool found {false};
  Layer layer {Layer::None};
  std::string host_path;
};

// Walks a guest absolute path through an overlay stacked on a base, the way the emulator
// does: each component is looked up in the overlay first and in the base second, an
// overlay whiteout hides what the base has, and a symlink whose target is absolute is
// re-rooted at the layer stack rather than at the host's /. `overlay` may be empty, which
// makes this the plain single-tree walk.
WalkHit WalkLayers(const std::string& overlay, const std::string& base, const std::string& guestPath) {
  WalkHit hit;
  if (base.empty()) return hit;

  std::deque<std::string> pending;
  for (auto& c : SplitComponents(guestPath)) pending.push_back(c);

  std::string curGuest; // the guest directory walked so far, without a trailing slash
  int hops = 0;

  while (!pending.empty()) {
    if (++hops > kMaxSymlinkHops) return WalkHit {};

    std::string name = pending.front();
    pending.pop_front();
    if (name.empty() || name == ".") continue;
    if (name == "..") {
      auto slash = curGuest.rfind('/');
      curGuest = (slash == std::string::npos) ? std::string() : curGuest.substr(0, slash);
      continue;
    }

    if (!overlay.empty() && LinkExists(fs::path(overlay + curGuest) / (".wh." + name))) {
      return WalkHit {}; // whited out: the base's copy does not exist for the guest
    }

    Layer layer = Layer::None;
    std::string host;
    if (!overlay.empty() && LinkExists(fs::path(overlay + curGuest) / name)) {
      layer = Layer::Overlay;
      host = overlay + curGuest + "/" + name;
    } else {
      bool baseHidden = !overlay.empty() && LinkExists(fs::path(overlay + curGuest) / ".wh..wh..opq");
      if (!baseHidden && LinkExists(fs::path(base + curGuest) / name)) {
        layer = Layer::Base;
        host = base + curGuest + "/" + name;
      }
    }
    if (layer == Layer::None) return WalkHit {};

    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(host, ec)) && !ec) {
      fs::path target = fs::read_symlink(host, ec);
      if (ec || target.empty()) return WalkHit {};
      auto parts = SplitComponents(target.string());
      if (target.is_absolute()) curGuest.clear();
      for (auto it = parts.rbegin(); it != parts.rend(); ++it) pending.push_front(*it);
      continue;
    }

    curGuest += "/" + name;
    if (pending.empty()) {
      hit.found = true;
      hit.layer = layer;
      hit.host_path = host;
      return hit;
    }
  }
  return hit;
}

// $ORIGIN, $LIB and $PLATFORM, expanded the way the guest loader would for an object
// sitting at `objectGuestDir`. An unexpanded token would otherwise be searched for
// literally and every $ORIGIN-relative dependency would read as missing.
std::string ExpandDynamicTokens(const std::string& dir, const std::string& objectGuestDir) {
  const auto& backend = Backend::GetActiveBackend();
  struct Token {
    const char* braced;
    const char* bare;
    std::string value;
  };
  const Token tokens[] = {
    {"${ORIGIN}", "$ORIGIN", objectGuestDir},
    {"${LIB}", "$LIB", "lib"},
    {"${PLATFORM}", "$PLATFORM", backend.archName == "arm64" ? "aarch64" : "x86_64"},
  };

  std::string out = dir;
  for (const auto& t : tokens) {
    for (const char* pattern : {t.braced, t.bare}) {
      std::string pat(pattern);
      size_t pos = 0;
      while ((pos = out.find(pat, pos)) != std::string::npos) {
        out.replace(pos, pat.size(), t.value);
        pos += t.value.size();
      }
    }
  }
  return out;
}

void AppendUnique(std::vector<std::string>& into, const std::string& value) {
  if (value.empty()) return;
  if (std::find(into.begin(), into.end(), value) == into.end()) into.push_back(value);
}

// The directories /etc/ld.so.conf names, resolved through the layers. The binary
// ld.so.cache is not parsed: it is generated from exactly these directories, so reading
// the configuration finds the same set without depending on a cache that a fresh overlay
// has not built yet.
void CollectConfDirs(const RootFSLayers& layers, const std::string& confPath, int depth,
                     std::vector<std::string>& out, std::set<std::string>& visited) {
  if (depth > 3) return;
  if (!visited.insert(confPath).second) return;

  auto res = ResolveGuestPath(layers, confPath);
  if (res.layer == Layer::None) return;

  std::ifstream f(res.host_path);
  if (!f.is_open()) return;

  std::string line;
  while (std::getline(f, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
    size_t begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos) continue;
    line = line.substr(begin);
    if (line.empty()) continue;

    if (line.rfind("include", 0) == 0 && (line.size() == 7 || line[7] == ' ' || line[7] == '\t')) {
      std::string pattern = line.substr(7);
      size_t p = pattern.find_first_not_of(" \t");
      if (p == std::string::npos) continue;
      pattern = pattern.substr(p);
      if (pattern.find('*') == std::string::npos && pattern.find('?') == std::string::npos) {
        CollectConfDirs(layers, pattern, depth + 1, out, visited);
        continue;
      }
      // A glob: list the directory in both layers and match the basename.
      std::string dir = fs::path(pattern).parent_path().string();
      std::string base = fs::path(pattern).filename().string();
      for (const std::string* tree : {&layers.overlay, &layers.base}) {
        if (tree->empty()) continue;
        std::error_code ec;
        fs::path host = *tree + dir;
        if (!fs::is_directory(host, ec)) continue;
        for (const auto& entry : fs::directory_iterator(host, fs::directory_options::skip_permission_denied, ec)) {
          std::string name = entry.path().filename().string();
          if (::fnmatch(base.c_str(), name.c_str(), 0) == 0) {
            CollectConfDirs(layers, dir + "/" + name, depth + 1, out, visited);
          }
        }
      }
      continue;
    }
    if (line[0] == '/') AppendUnique(out, line);
  }
}

std::vector<std::string> SplitColonList(const std::string& value) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= value.size()) {
    size_t colon = value.find(':', start);
    std::string part = value.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!part.empty()) out.push_back(part);
    if (colon == std::string::npos) break;
    start = colon + 1;
  }
  return out;
}

// One object in the dependency graph, keyed by its guest path.
struct Object {
  std::string guest_path;
  std::string host_path;
  ElfInspect::ElfDetails details;
};

} // namespace

const char* LayerName(Layer layer) {
  switch (layer) {
    case Layer::AppDir: return "app";
    case Layer::Overlay: return "overlay";
    case Layer::Base: return "base";
    case Layer::Host: return "host";
    case Layer::None: break;
  }
  return "none";
}

std::string MachineName(uint16_t machine) {
  switch (machine) {
    case EM_AARCH64: return "arm64";
    case EM_X86_64: return "x86_64";
    case EM_386: return "i386";
    case EM_PPC64: return "ppc64le";
    case EM_PPC: return "ppc";
    case EM_RISCV: return "riscv";
    case 0: return "not an ELF64 object";
    default: break;
  }
  return "ELF machine " + std::to_string(machine);
}

RootFSLayers LayersFor(const std::string& rootfsPath) {
  RootFSLayers layers;
  if (rootfsPath.empty()) return layers;

  std::string base = Paths::ExpandUser(rootfsPath);
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  layers.base = base;

  std::error_code ec;
  std::string candidate = base + "-overlay";
  if (fs::is_directory(candidate, ec)) layers.overlay = candidate;
  return layers;
}

Resolution ResolveGuestPath(const RootFSLayers& layers, const std::string& guestPath) {
  Resolution res;
  if (guestPath.empty() || guestPath[0] != '/') return res;

  WalkHit hit;
  if (!layers.overlay.empty() && IsOwnedPath(guestPath)) {
    hit = WalkLayers(layers.overlay, layers.base, guestPath);
  } else {
    hit = WalkLayers(std::string(), layers.base, guestPath);
  }

  if (hit.found) {
    res.layer = hit.layer;
    res.host_path = hit.host_path;
    res.machine = ElfInspect::ProbeFile(res.host_path).machine;
    return res;
  }

  if (IsNoFallthroughPath(guestPath)) return res;

  std::error_code ec;
  if (fs::exists(guestPath, ec) && !fs::is_directory(guestPath, ec)) {
    res.layer = Layer::Host;
    res.host_path = guestPath;
    res.machine = ElfInspect::ProbeFile(guestPath).machine;
  }
  return res;
}

ScanOptions OptionsForRecord(const Record::AppRecord& record) {
  ScanOptions opt;
  opt.exe_path = record.GetResolvedExePath();
  if (record.source.kind == "dir" && !record.source.dir.empty()) {
    opt.app_dir = Paths::ExpandUser(record.source.dir);
  } else if (!opt.exe_path.empty()) {
    opt.app_dir = fs::path(opt.exe_path).parent_path().string();
  }
  opt.layers = LayersFor(record.rootfs);
  opt.env = record.env;
  return opt;
}

std::vector<std::string> SonamesFromRunOutput(const std::string& text) {
  std::vector<std::string> out;
  std::set<std::string> seen;

  // Both halves of what a failed load looks like: the loader's own line for a DT_NEEDED
  // it could not find, and the message an application prints when a dlopen fails. They
  // end in the same words, which is what this matches.
  static const std::regex kCannotOpen(R"(([A-Za-z0-9_+.\-]+\.so(?:\.[0-9A-Za-z_.\-]+)?)\s*:\s*cannot open shared object file)");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), kCannotOpen); it != std::sregex_iterator(); ++it) {
    std::string name = (*it)[1].str();
    if (seen.insert(name).second) out.push_back(name);
  }

  // "error while loading shared libraries: libfoo.so.1: <anything else>" -- a wrong ELF
  // class, a missing symbol version, a failed mapping. The soname is still the answer.
  static const std::regex kLoadError(R"(error while loading shared libraries:\s*([A-Za-z0-9_+.\-]+\.so(?:\.[0-9A-Za-z_.\-]+)?)\s*:)");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), kLoadError); it != std::sregex_iterator(); ++it) {
    std::string name = (*it)[1].str();
    if (seen.insert(name).second) out.push_back(name);
  }

  return out;
}

std::string NewestHealthLog(const std::string& appName) {
  std::string dir = Paths::GetCacheDir() + "/sleeve/health";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return "";

  std::string best;
  for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string name = entry.path().filename().string();
    if (name.rfind(appName + "-", 0) != 0) continue;
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".log") != 0) continue;
    // The names carry an ISO timestamp, so lexicographic order is chronological order.
    if (best.empty() || name > fs::path(best).filename().string()) best = entry.path().string();
  }
  return best;
}

ScanResult ScanApp(const ScanOptions& options) {
  auto started = std::chrono::steady_clock::now();
  ScanResult result;

  const auto& backend = Backend::GetActiveBackend();

  if (options.exe_path.empty()) {
    result.notes.push_back("the record has no executable path, so there was nothing to read");
    return result;
  }
  if (options.layers.base.empty()) {
    result.notes.push_back("the record names no rootfs, so nothing can be resolved; set one with "
                           "'sleeve set <app> rootfs=<name>'");
    return result;
  }
  std::error_code ec;
  if (!fs::is_directory(options.layers.base, ec)) {
    result.notes.push_back("the rootfs " + options.layers.base + " is not a directory");
    return result;
  }
  if (options.layers.overlay.empty()) {
    result.notes.push_back("this rootfs has no overlay, so there is nowhere for guest pacman to "
                           "install into; build one with POWERarmRootFSFetcher");
  }

  // --- The objects to read: the executable, plus every guest-architecture ELF shipped
  // beside it. Electron bundles its own ffmpeg, ANGLE and swiftshader, and a native Node
  // addon (.node) is a shared object too; each one names dependencies the main binary
  // does not.
  std::vector<std::string> roots;
  roots.push_back(options.exe_path);

  if (!options.app_dir.empty() && fs::is_directory(options.app_dir, ec)) {
    auto iter = fs::recursive_directory_iterator(options.app_dir, fs::directory_options::skip_permission_denied, ec);
    auto end = fs::recursive_directory_iterator();
    while (iter != end && result.files_probed < options.max_probe_files) {
      std::error_code entryEc;
      if (!ec && iter->is_regular_file(entryEc) && !entryEc) {
        ++result.files_probed;
        auto probe = ElfInspect::ProbeFile(iter->path().string());
        if (probe.machine == backend.elfMachine &&
            (probe.kind == ElfInspect::FileKind::AArch64_Dyn ||
             probe.kind == ElfInspect::FileKind::AArch64_Exec || probe.type == ET_DYN ||
             probe.type == ET_EXEC)) {
          AppendUnique(roots, iter->path().string());
        }
      }
      iter.increment(ec);
    }
  }

  // --- The search path, built once. Per-object RPATH/RUNPATH is layered on top of it.
  std::vector<std::string> confDirs;
  std::set<std::string> visitedConf;
  CollectConfDirs(options.layers, "/etc/ld.so.conf", 0, confDirs, visitedConf);

  std::vector<std::string> ldLibraryPath;
  auto envIt = options.env.find("LD_LIBRARY_PATH");
  if (envIt != options.env.end()) ldLibraryPath = SplitColonList(envIt->second);

  std::vector<std::string> tailDirs = confDirs;
  for (const char* d : kDefaultLibDirs) AppendUnique(tailDirs, d);
  for (const auto& d : ldLibraryPath) AppendUnique(result.search_dirs, d);
  for (const auto& d : tailDirs) AppendUnique(result.search_dirs, d);

  // --- Walk the closure.
  std::map<std::string, Object> objects;  // guest path -> object
  std::map<std::string, SonameStatus> statuses;
  std::deque<std::string> queue; // guest paths still to read

  auto adoptObject = [&](const std::string& guestPath, const std::string& hostPath) -> bool {
    if (objects.count(guestPath)) return true;
    auto details = ElfInspect::InspectElf64(hostPath);
    if (!details) return false;
    Object obj;
    obj.guest_path = guestPath;
    obj.host_path = hostPath;
    obj.details = *details;
    objects.emplace(guestPath, std::move(obj));
    queue.push_back(guestPath);
    result.objects.push_back(guestPath);
    return true;
  };

  // A root's guest path is its own path when it lives on the host outside the rootfs,
  // which is the ordinary case (~/Development/<app>), and the path itself when the app
  // was installed into the overlay by guest pacman.
  std::string mainGuest;
  for (const auto& root : roots) {
    bool adopted = false;
    if (fs::exists(root, ec)) {
      adopted = adoptObject(root, root);
    } else {
      auto res = ResolveGuestPath(options.layers, root);
      if (res.layer != Layer::None) adopted = adoptObject(root, res.host_path);
    }
    if (adopted && mainGuest.empty()) mainGuest = root;
  }
  if (objects.empty()) {
    result.notes.push_back("could not read " + options.exe_path + " as a " + backend.archName +
                           " ELF object");
    return result;
  }
  if (mainGuest != options.exe_path) {
    // Almost every bundled application is launched through a shell script; the ELF
    // objects beside it are what actually carry the dependencies.
    result.notes.push_back(options.exe_path +
                           " is not itself an ELF object; the report is built from the " +
                           backend.archName + " objects in the install tree");
  }

  // The executable's own RPATH applies to everything it pulls in, which is how an
  // application that ships its libraries in one directory finds them from a library.
  const Object& mainObject = objects.at(mainGuest);
  std::vector<std::string> mainRpath;
  if (mainObject.details.runpath.empty()) mainRpath = mainObject.details.rpath;
  std::string mainGuestDir = fs::path(mainObject.guest_path).parent_path().string();

  while (!queue.empty() && objects.size() < options.max_objects) {
    std::string guestPath = queue.front();
    queue.pop_front();
    const Object& obj = objects.at(guestPath);
    std::string objGuestDir = fs::path(obj.guest_path).parent_path().string();

    // glibc's order, with the parts that exist here: DT_RPATH (only when the object has
    // no DT_RUNPATH), LD_LIBRARY_PATH, DT_RUNPATH, then the configured and default
    // directories.
    std::vector<std::string> dirs;
    if (obj.details.runpath.empty()) {
      for (const auto& d : obj.details.rpath) AppendUnique(dirs, ExpandDynamicTokens(d, objGuestDir));
      for (const auto& d : mainRpath) AppendUnique(dirs, ExpandDynamicTokens(d, mainGuestDir));
    }
    for (const auto& d : ldLibraryPath) AppendUnique(dirs, ExpandDynamicTokens(d, objGuestDir));
    for (const auto& d : obj.details.runpath) AppendUnique(dirs, ExpandDynamicTokens(d, objGuestDir));
    for (const auto& d : tailDirs) AppendUnique(dirs, d);

    for (const auto& soname : obj.details.needed_libs) {
      auto& status = statuses[soname];
      if (status.soname.empty()) {
        status.soname = soname;
        status.discovered_by = "static";
      }
      if (status.needed_by.size() < kMaxNeededBy) {
        std::string who = fs::path(obj.guest_path).filename().string();
        if (std::find(status.needed_by.begin(), status.needed_by.end(), who) == status.needed_by.end()) {
          status.needed_by.push_back(who);
        }
      }
      if (status.layer != Layer::None) continue; // already resolved through another object

      // A name with a slash in it is used as a path and never searched for.
      std::vector<std::string> candidates;
      if (soname.find('/') != std::string::npos) {
        candidates.push_back(soname[0] == '/' ? soname : objGuestDir + "/" + soname);
      } else {
        for (const auto& d : dirs) candidates.push_back(d + "/" + soname);
      }

      for (const auto& candidate : candidates) {
        auto res = ResolveGuestPath(options.layers, candidate);
        if (res.layer == Layer::None) continue;
        // Anything but the guest architecture is not a hit: the loader rejects it and
        // carries on searching, so it must not stop the search here either.
        bool right = (res.machine == backend.elfMachine);
        if (!right && status.host_path.empty()) {
          // Remember the near miss; it is the difference between "nothing has it" and
          // "only the host has it, and the host's copy is the wrong architecture".
          status.layer = Layer::None;
          status.host_path = res.host_path;
          status.machine = res.machine;
          status.satisfied = false;
          continue;
        }
        if (!right) continue;

        status.layer = res.layer;
        if (status.layer == Layer::Host && !options.app_dir.empty() &&
            HasPathPrefix(res.host_path, options.app_dir)) {
          status.layer = Layer::AppDir;
        }
        status.host_path = res.host_path;
        status.machine = res.machine;
        status.satisfied = true;

        std::string depGuest = candidate;
        adoptObject(depGuest, res.host_path);
        break;
      }
    }
  }

  if (objects.size() >= options.max_objects) {
    result.notes.push_back("stopped after " + std::to_string(options.max_objects) +
                           " objects; the report may be incomplete");
  }

  // --- The dlopen half: sonames an actual run named. Static analysis cannot see a
  // dlopen, and that is how most of Electron's optional pieces load.
  size_t staleRunNames = 0;
  for (const auto& soname : options.run_sonames) {
    auto it = statuses.find(soname);
    if (it != statuses.end()) {
      // A run named something this pass resolves. The likeliest reason is that the run
      // is older than the install that fixed it, so the layer that has it wins: a stale
      // log must not put a library back on the missing list.
      if (it->second.satisfied) ++staleRunNames;
      continue;
    }
    SonameStatus status;
    status.soname = soname;
    status.discovered_by = "run";
    for (const auto& d : tailDirs) {
      auto res = ResolveGuestPath(options.layers, d + "/" + soname);
      if (res.layer == Layer::None) continue;
      status.host_path = res.host_path;
      status.machine = res.machine;
      if (res.machine == backend.elfMachine) {
        status.layer = res.layer;
        status.satisfied = true;
      }
      break;
    }
    statuses.emplace(soname, std::move(status));
  }
  if (staleRunNames > 0) {
    result.notes.push_back(std::to_string(staleRunNames) +
                           " soname(s) a previous run could not open now resolve; that run "
                           "predates whatever fixed them and they are not reported as unmet");
  }

  for (auto& [name, status] : statuses) {
    result.all.push_back(status);
    if (!status.satisfied) result.unmet.push_back(status);
  }

  result.elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return result;
}

// ---------------------------------------------------------------------------
// Guest pacman
// ---------------------------------------------------------------------------

Process::ProcessOptions BuildGuestPacmanCommand(const std::string& rootfsPath,
                                                const std::vector<std::string>& pacmanArgs,
                                                double timeoutSeconds) {
  const auto& backend = Backend::GetActiveBackend();
  Process::ProcessOptions opt;

  // unshare -r is what makes pacman believe it is root; PORTABLE=1 is what keeps the
  // emulator from looking for its server socket under the namespace's uid 0, which is
  // the failure the rootfs README calls out.
  opt.args = {"unshare", "-r", backend.emulatorBin, "/usr/bin/pacman"};
  for (const auto& a : pacmanArgs) opt.args.push_back(a);

  opt.env[backend.envPrefix + "PORTABLE"] = "1";
  opt.env[backend.envPrefix + "ROOTFS"] = Paths::ExpandUser(rootfsPath);
  opt.timeout_seconds = timeoutSeconds;
  return opt;
}

std::vector<PackageMatch> ParseFilesOutput(const std::string& out) {
  std::vector<PackageMatch> matches;
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    // repository\0pkgname\0pkgver\0path
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size()) {
      size_t nul = line.find('\0', start);
      fields.push_back(line.substr(start, nul == std::string::npos ? std::string::npos : nul - start));
      if (nul == std::string::npos) break;
      start = nul + 1;
    }
    if (fields.size() < 4) continue;

    PackageMatch m;
    m.repo = fields[0];
    m.package = fields[1];
    m.version = fields[2];
    m.path = fields[3];
    if (m.package.empty() || m.path.empty()) continue;
    m.soname = fs::path(m.path).filename().string();
    matches.push_back(std::move(m));
  }
  return matches;
}

bool FilesDatabasePresent(const RootFSLayers& layers) {
  std::error_code ec;
  for (const std::string* tree : {&layers.overlay, &layers.base}) {
    if (tree->empty()) continue;
    fs::path dir = *tree + "/var/lib/pacman/sync";
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
      std::string name = entry.path().filename().string();
      if (name.size() > 6 && name.compare(name.size() - 6, 6, ".files") == 0) return true;
    }
  }
  return false;
}

PackagePlan MapSonamesToPackages(const std::string& rootfsPath, const std::vector<std::string>& sonames,
                                 const Process::Runner& run) {
  PackagePlan plan;
  plan.files_db_present = FilesDatabasePresent(LayersFor(rootfsPath));

  if (sonames.empty()) return plan;
  if (!plan.files_db_present) {
    plan.error = "the guest has no file database, so no soname can be traced to a package; "
                 "run 'sleeve libs <app> --sync' first (it downloads into the overlay)";
    return plan;
  }

  std::vector<std::string> args = {"-F", "--machinereadable"};
  for (const auto& s : sonames) args.push_back(s);

  auto opt = BuildGuestPacmanCommand(rootfsPath, args, 180.0);
  plan.command = Process::Describe(opt);

  auto res = Process::Execute(run, opt);
  plan.matches = ParseFilesOutput(res.stdout_str);

  // pacman exits non-zero when at least one term matched nothing, which is a normal
  // outcome here; only an empty result with a non-zero exit is a failure to report.
  if (res.exit_code != 0 && plan.matches.empty()) {
    std::string detail = res.stderr_str.empty() ? res.stdout_str : res.stderr_str;
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
    plan.error = detail.empty() ? ("guest pacman exited " + std::to_string(res.exit_code)) : detail;
    return plan;
  }

  std::set<std::string> claimed;
  std::set<std::string> packages;
  for (const auto& m : plan.matches) {
    claimed.insert(m.soname);
    packages.insert(m.package);
  }
  for (const auto& s : sonames) {
    if (!claimed.count(s)) plan.unmatched.push_back(s);
  }
  plan.packages.assign(packages.begin(), packages.end());
  return plan;
}

Process::RunOutcome SyncFilesDatabase(const std::string& rootfsPath, const Process::Runner& run) {
  Process::RunOutcome outcome;
  auto opt = BuildGuestPacmanCommand(rootfsPath, {"-Fy", "--noconfirm"}, 900.0);
  outcome.command = Process::Describe(opt);

  auto res = Process::Execute(run, opt);
  outcome.output = res.stdout_str + res.stderr_str;
  outcome.ok = (res.exit_code == 0 && !res.timed_out);
  if (!outcome.ok) {
    outcome.error = res.timed_out ? "the sync timed out"
                                  : ("guest pacman exited " + std::to_string(res.exit_code));
  }
  return outcome;
}

Process::RunOutcome InstallPackages(const std::string& rootfsPath, const std::vector<std::string>& packages,
                           const Process::Runner& run) {
  Process::RunOutcome outcome;
  if (packages.empty()) {
    outcome.error = "no packages to install";
    return outcome;
  }

  std::vector<std::string> args = {"-S", "--needed", "--noconfirm"};
  for (const auto& p : packages) args.push_back(p);

  auto opt = BuildGuestPacmanCommand(rootfsPath, args, 1800.0);
  outcome.command = Process::Describe(opt);

  auto res = Process::Execute(run, opt);
  outcome.output = res.stdout_str + res.stderr_str;
  outcome.ok = (res.exit_code == 0 && !res.timed_out);
  if (!outcome.ok) {
    outcome.error = res.timed_out ? "the install timed out"
                                  : ("guest pacman exited " + std::to_string(res.exit_code));
  }
  return outcome;
}

} // namespace Sleeve::Libs
