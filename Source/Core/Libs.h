// SPDX-License-Identifier: MIT
#pragma once

// Unmet guest library dependencies, all at once.
//
// A guest rootfs built by the fetcher is a base plus a per-user overlay with guest
// pacman in it. That is enough to run a CLI; an Electron application needs sixty-odd
// library packages on top, and the only thing that knows which ones is the application
// itself. Fixing that one failed launch at a time is miserable, because the loader stops
// at the first soname it cannot find and says nothing about the next fifty.
//
// So this walks the application's ELF objects instead, collects every DT_NEEDED soname,
// resolves each the way the guest loader would, and reports the ones that resolve
// nowhere. What static analysis cannot see -- a dlopen -- is picked up from what a run
// printed, which is the other half the health checker already produces.
//
// Nothing here writes to a rootfs unless InstallPackages is called.

#include "Process.h"
#include "Record.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Sleeve::Libs {

// Where a library was found, named for the layer rather than the directory, because the
// directory is the same /usr/lib in three different trees.
enum class Layer {
  None,    // no layer has it
  AppDir,  // shipped inside the application's own install tree
  Overlay, // the per-user writable layer, which is what an install would add to
  Base,    // the read-only base rootfs
  Host,    // only the host filesystem has it
};

const char* LayerName(Layer layer);

// An ELF e_machine as a person would name it, for the line that explains why a library
// that was found is still not usable.
std::string MachineName(uint16_t machine);

// A rootfs as the emulator sees it: the base tree, and the overlay that shadows it.
struct RootFSLayers {
  std::string base;
  std::string overlay; // "" when the rootfs has none
};

// The overlay for a base rootfs is "<base>-overlay" when that directory exists, which is
// the same default the emulator applies (RootFSOverlay::ConfiguredPath).
RootFSLayers LayersFor(const std::string& rootfsPath);

struct Resolution {
  Layer layer {Layer::None};
  std::string host_path; // the file on this machine, "" when nothing was found
  uint16_t machine {0};  // e_machine of what was found, 0 when it is not an ELF64 LSB file
};

// One guest absolute path, resolved through the overlay, then the base, then the host,
// following symlinks the way the guest kernel would: an absolute link target is
// re-rooted at the layer stack instead of at the host's /. Overlay whiteouts
// (".wh.<name>", ".wh..wh..opq") hide what the base has, as they do for the emulator.
Resolution ResolveGuestPath(const RootFSLayers& layers, const std::string& guestPath);

struct SonameStatus {
  std::string soname;
  Layer layer {Layer::None};
  std::string host_path;
  uint16_t machine {0};
  // Found, and of the guest architecture. A ppc64le library on the host filesystem is
  // found and still unmet: the guest loader rejects it and keeps searching.
  bool satisfied {false};
  // The objects that name it, in the order they were read. Truncated; this is for the
  // report, not for a graph.
  std::vector<std::string> needed_by;
  // Where it came from: "static" (a DT_NEEDED entry) or "run" (a soname an actual run
  // named, which is the only way a dlopen shows up).
  std::string discovered_by {"static"};
};

struct ScanOptions {
  std::string exe_path;   // the application's main executable
  std::string app_dir;    // its install tree, "" for a lone binary
  RootFSLayers layers;
  std::map<std::string, std::string> env; // the record's env; LD_LIBRARY_PATH is read
  // Sonames from a run's output, merged in as dlopen candidates.
  std::vector<std::string> run_sonames;
  size_t max_probe_files {30000}; // ceiling on the install-tree walk
  size_t max_objects {4000};      // ceiling on the dependency closure
};

struct ScanResult {
  std::vector<std::string> objects;  // every ELF object whose DT_NEEDED was read
  std::vector<SonameStatus> all;     // every soname seen, sorted by name
  std::vector<SonameStatus> unmet;   // the subset no layer satisfies
  std::vector<std::string> search_dirs; // the guest directories that were searched
  std::vector<std::string> notes;
  size_t files_probed {0};
  double elapsed_seconds {0.0};
};

// Walks exe_path, every guest-architecture ELF object shipped alongside it, and the
// transitive closure of what those need.
ScanResult ScanApp(const ScanOptions& options);

// Fills a ScanOptions from a record: the executable, its install tree, the record's
// rootfs and env.
ScanOptions OptionsForRecord(const Record::AppRecord& record);

// Every "<soname>: cannot open shared object file" a run printed, deduplicated and in
// first-seen order. This covers both the loader's own "error while loading shared
// libraries" line and the message an application prints when its own dlopen fails, which
// is how most of Electron's optional pieces load.
std::vector<std::string> SonamesFromRunOutput(const std::string& text);

// The newest health log for an app, or "" when it has never been checked.
std::string NewestHealthLog(const std::string& appName);

// ---------------------------------------------------------------------------
// Soname -> package, through guest pacman
// ---------------------------------------------------------------------------

struct PackageMatch {
  std::string soname;
  std::string repo;
  std::string package;
  std::string version;
  std::string path; // the guest path the file lives at
};

struct PackagePlan {
  bool files_db_present {false};
  std::vector<PackageMatch> matches;
  std::vector<std::string> unmatched; // sonames no package claims
  std::vector<std::string> packages;  // the deduplicated set to install, sorted
  std::string command;                // what was run, written out the way a shell shows it
  std::string error;
};

// The argv and environment a guest pacman invocation needs, for the active backend:
//
//   POWERARM_PORTABLE=1 POWERARM_ROOTFS=<rootfs> unshare -r POWERarm /usr/bin/pacman ...
//
// PORTABLE=1 is not optional. Without it the emulator looks for its server socket under
// the user namespace's uid 0 and fails; unshare -r is what makes pacman believe it is
// root. Pure, so the shape of the command is a test rather than something to be found
// out by running it.
Process::ProcessOptions BuildGuestPacmanCommand(const std::string& rootfsPath,
                                                const std::vector<std::string>& pacmanArgs,
                                                double timeoutSeconds);

// `pacman -F --machinereadable` prints repository\0pkgname\0pkgver\0path\n per match.
std::vector<PackageMatch> ParseFilesOutput(const std::string& out);

// Whether a *.files database exists in either layer. `pacman -F` needs one, and building
// it is a download, so it is never done implicitly.
bool FilesDatabasePresent(const RootFSLayers& layers);

// Maps sonames to packages with one `pacman -F` call. Read-only against the rootfs.
PackagePlan MapSonamesToPackages(const std::string& rootfsPath,
                                 const std::vector<std::string>& sonames,
                                 const Process::Runner& run = {});

// `pacman -Fy`: downloads the file databases into the overlay. A write, so it is asked
// for explicitly.
Process::RunOutcome SyncFilesDatabase(const std::string& rootfsPath, const Process::Runner& run = {});

// `pacman -S --needed --noconfirm <pkgs>`: installs into the overlay, which every guest
// on this machine shares. Callers get consent first.
Process::RunOutcome InstallPackages(const std::string& rootfsPath, const std::vector<std::string>& packages,
                           const Process::Runner& run = {});

} // namespace Sleeve::Libs
