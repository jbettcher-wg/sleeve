// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "Backend.h"
#include "Libs.h"
#include "Process.h"
#include "Record.h"
#include "TestEnv.h"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;
using namespace Sleeve;
using SleeveTest::DynamicElfSpec;

namespace {

// A base rootfs and its overlay, the way the fetcher lays them out. Only the pieces the
// resolver reads: a lib directory in each, and the configuration the guest loader would.
struct Fixture {
  explicit Fixture(const fs::path& root) : base(root / "ArchLinuxARM-fix"), overlay(root / "ArchLinuxARM-fix-overlay") {
    std::error_code ec;
    fs::create_directories(base / "usr/lib", ec);
    fs::create_directories(base / "etc", ec);
    fs::create_directories(overlay / "usr/lib", ec);
    // Arch ships /lib as a symlink to usr/lib; the resolver has to follow it the way the
    // guest kernel would rather than off to the host's own /lib.
    fs::create_symlink("usr/lib", base / "lib", ec);
    std::ofstream(base / "etc/ld.so.conf") << "# nothing but the defaults\n";
  }

  Libs::RootFSLayers Layers() const {
    Libs::RootFSLayers l;
    l.base = base.string();
    l.overlay = overlay.string();
    return l;
  }

  fs::path base;
  fs::path overlay;
};

Process::Runner FakeRunner(std::vector<Process::ProcessOptions>* seen, int exitCode,
                           const std::string& out) {
  return [seen, exitCode, out](const Process::ProcessOptions& options) {
    if (seen) seen->push_back(options);
    Process::ProcessResult res;
    res.exit_code = exitCode;
    res.stdout_str = out;
    return res;
  };
}

// `pacman -F --machinereadable` writes NUL-separated fields, which no shell heredoc can
// carry; building the lines here keeps the parser's contract explicit.
std::string FilesLine(const std::string& repo, const std::string& pkg, const std::string& ver,
                      const std::string& path) {
  std::string line = repo;
  line.push_back('\0');
  line += pkg;
  line.push_back('\0');
  line += ver;
  line.push_back('\0');
  line += path;
  line.push_back('\n');
  return line;
}

} // namespace

TEST_CASE("Libs: the overlay shadows the base, and a whiteout hides it", "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-layers");
  Fixture fx(home.Root());
  auto layers = fx.Layers();

  // The same soname in both layers: the overlay is what an install writes into, so the
  // overlay is what the guest sees.
  SleeveTest::WriteFakeElf(fx.base / "usr/lib/libboth.so.1", SleeveTest::kMachineAArch64);
  SleeveTest::WriteFakeElf(fx.overlay / "usr/lib/libboth.so.1", SleeveTest::kMachineAArch64);
  SleeveTest::WriteFakeElf(fx.base / "usr/lib/libbaseonly.so.1", SleeveTest::kMachineAArch64);

  auto both = Libs::ResolveGuestPath(layers, "/usr/lib/libboth.so.1");
  CHECK(both.layer == Libs::Layer::Overlay);
  CHECK(both.host_path == (fx.overlay / "usr/lib/libboth.so.1").string());

  auto baseOnly = Libs::ResolveGuestPath(layers, "/usr/lib/libbaseonly.so.1");
  CHECK(baseOnly.layer == Libs::Layer::Base);

  SECTION("a whiteout makes the base's copy not exist") {
    std::ofstream(fx.overlay / "usr/lib/.wh.libbaseonly.so.1") << "";
    auto hidden = Libs::ResolveGuestPath(layers, "/usr/lib/libbaseonly.so.1");
    CHECK(hidden.layer == Libs::Layer::None);
  }

  SECTION("an opaque marker hides everything the base has below that directory") {
    std::ofstream(fx.overlay / "usr/lib/.wh..wh..opq") << "";
    auto hidden = Libs::ResolveGuestPath(layers, "/usr/lib/libbaseonly.so.1");
    CHECK(hidden.layer == Libs::Layer::None);
    // What the overlay itself has is still there.
    CHECK(Libs::ResolveGuestPath(layers, "/usr/lib/libboth.so.1").layer == Libs::Layer::Overlay);
  }
}

TEST_CASE("Libs: an absolute symlink is re-rooted at the rootfs, not at the host's /",
          "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-symlink");
  Fixture fx(home.Root());
  auto layers = fx.Layers();

  SleeveTest::WriteFakeElf(fx.base / "usr/lib/libtarget.so.1", SleeveTest::kMachineAArch64);
  std::error_code ec;
  // The shape Arch actually ships: the versioned file, and a development symlink whose
  // target is absolute. Following it against the host would find nothing (or, worse, the
  // host's own copy).
  fs::create_symlink("/usr/lib/libtarget.so.1", fx.overlay / "usr/lib/libtarget.so", ec);
  REQUIRE_FALSE(ec);

  auto res = Libs::ResolveGuestPath(layers, "/usr/lib/libtarget.so");
  CHECK(res.layer == Libs::Layer::Base);
  CHECK(res.host_path == (fx.base / "usr/lib/libtarget.so.1").string());

  SECTION("and /lib reaches usr/lib through the rootfs's own relative symlink") {
    auto viaLib = Libs::ResolveGuestPath(layers, "/lib/libtarget.so.1");
    CHECK(viaLib.layer == Libs::Layer::Base);
  }
}

TEST_CASE("Libs: package-manager state never falls through to the host", "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-nofallthrough");
  Fixture fx(home.Root());
  auto layers = fx.Layers();

  // A path no layer has, outside the guest-owned prefixes, reaches the host filesystem:
  // that is how an application installed under ~ finds the libraries it ships.
  SleeveTest::WriteFakeElf(home.Root() / "outside/libhost.so.1", SleeveTest::kMachineAArch64);
  auto outside = Libs::ResolveGuestPath(layers, (home.Root() / "outside/libhost.so.1").string());
  CHECK(outside.layer == Libs::Layer::Host);

  // The package database is the exception: a guest pacman that fell through to it would
  // read, and try to write, the host's own. The layers still answer for it.
  std::error_code ec;
  fs::create_directories(fx.overlay / "var/lib/pacman/local/nss-3.108-1", ec);
  std::ofstream(fx.overlay / "var/lib/pacman/local/nss-3.108-1/desc") << "%NAME%\nnss\n";

  CHECK(Libs::ResolveGuestPath(layers, "/var/lib/pacman/local/nss-3.108-1/desc").layer ==
        Libs::Layer::Overlay);

  // And a file the host really has under that prefix is still invisible. Picked from the
  // host at run time so this asserts something on an Arch machine and nothing anywhere
  // else, rather than asserting the wrong thing everywhere.
  std::string hostOwned;
  for (const char* dir : {"/var/lib/pacman/sync", "/etc/pacman.d"}) {
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
      if (entry.is_regular_file(ec)) {
        hostOwned = entry.path().string();
        break;
      }
    }
    if (!hostOwned.empty()) break;
  }
  if (!hostOwned.empty()) {
    INFO("host file that must not show through: " << hostOwned);
    CHECK(Libs::ResolveGuestPath(layers, hostOwned).layer == Libs::Layer::None);
  }
}

TEST_CASE("Libs: unmet sonames are found in one pass, across everything the app ships",
          "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-scan");
  REQUIRE(Backend::SetActiveBackend("powerarm"));
  Fixture fx(home.Root());

  fs::path app = home.Root() / "app";
  std::error_code ec;
  fs::create_directories(app, ec);

  // The main binary, an Electron-shaped bundle: it ships a library of its own next to
  // itself and finds it through DT_RUNPATH=$ORIGIN.
  DynamicElfSpec main;
  main.type = 2; // ET_EXEC
  main.needed = {"libpresent.so.1", "libmissing.so.1", "libshipped.so.1", "libwrongarch.so.1"};
  main.runpath = {"$ORIGIN"};
  SleeveTest::WriteDynamicElf(app / "app", main);

  DynamicElfSpec shipped;
  shipped.soname = "libshipped.so.1";
  SleeveTest::WriteDynamicElf(app / "libshipped.so.1", shipped);

  // A native addon, the case the main binary never names: it is only found because every
  // guest-architecture object in the tree is read, not just the executable.
  DynamicElfSpec addon;
  addon.needed = {"libaddononly.so.2"};
  SleeveTest::WriteDynamicElf(app / "resources/keymap.node", addon);

  // A foreign object in the same tree must not be read for dependencies at all.
  DynamicElfSpec foreign;
  foreign.machine = SleeveTest::kMachineX86_64;
  foreign.needed = {"libneverasked.so.1"};
  SleeveTest::WriteDynamicElf(app / "resources/foreign.so", foreign);

  SleeveTest::WriteFakeElf(fx.overlay / "usr/lib/libpresent.so.1", SleeveTest::kMachineAArch64);
  // Only the host has this one, and the host is not the guest's architecture: found, and
  // still unmet, because the guest loader rejects it and goes on searching.
  SleeveTest::WriteFakeElf(fx.base / "usr/lib/libwrongarch.so.1", SleeveTest::kMachinePPC64);

  Libs::ScanOptions opt;
  opt.exe_path = (app / "app").string();
  opt.app_dir = app.string();
  opt.layers = fx.Layers();

  auto scan = Libs::ScanApp(opt);

  auto unmet = [&scan](const std::string& soname) {
    for (const auto& u : scan.unmet) {
      if (u.soname == soname) return true;
    }
    return false;
  };
  auto status = [&scan](const std::string& soname) -> Libs::SonameStatus {
    for (const auto& s : scan.all) {
      if (s.soname == soname) return s;
    }
    return {};
  };

  CHECK(unmet("libmissing.so.1"));
  CHECK(unmet("libaddononly.so.2"));
  CHECK(unmet("libwrongarch.so.1"));
  CHECK_FALSE(unmet("libpresent.so.1"));
  CHECK_FALSE(unmet("libshipped.so.1"));
  CHECK_FALSE(unmet("libneverasked.so.1"));

  CHECK(status("libpresent.so.1").layer == Libs::Layer::Overlay);
  // $ORIGIN resolved to the application's own directory, which is a host path outside
  // the rootfs; that is how a bundled application finds what it ships.
  CHECK(status("libshipped.so.1").layer == Libs::Layer::AppDir);
  CHECK(status("libwrongarch.so.1").machine == SleeveTest::kMachinePPC64);
  CHECK(status("libaddononly.so.2").needed_by == std::vector<std::string> {"keymap.node"});
}

TEST_CASE("Libs: a run's output supplies what no ELF header can", "[sleeve][libs]") {
  // The loader's own line, an application's dlopen failure, and the same soname twice.
  std::string text =
      "/home/j/app: error while loading shared libraries: libnspr4.so: cannot open shared object "
      "file: No such file or directory\n"
      "[1234:0925/101500.1] libpulse.so.0: cannot open shared object file: No such file or "
      "directory\n"
      "libpulse.so.0: cannot open shared object file\n"
      "error while loading shared libraries: libcurl.so.4: wrong ELF class: ELFCLASS32\n";

  auto names = Libs::SonamesFromRunOutput(text);
  CHECK(names.size() == 3);
  CHECK(std::find(names.begin(), names.end(), "libnspr4.so") != names.end());
  CHECK(std::find(names.begin(), names.end(), "libpulse.so.0") != names.end());
  // A load error that is not "cannot open" still names the library that failed.
  CHECK(std::find(names.begin(), names.end(), "libcurl.so.4") != names.end());
}

TEST_CASE("Libs: sonames from a run are merged into the report as dlopen candidates",
          "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-dlopen");
  REQUIRE(Backend::SetActiveBackend("powerarm"));
  Fixture fx(home.Root());

  fs::path app = home.Root() / "app";
  DynamicElfSpec main;
  main.type = 2;
  main.needed = {"libpresent.so.1"};
  SleeveTest::WriteDynamicElf(app / "app", main);
  SleeveTest::WriteFakeElf(fx.overlay / "usr/lib/libpresent.so.1", SleeveTest::kMachineAArch64);

  Libs::ScanOptions opt;
  opt.exe_path = (app / "app").string();
  opt.app_dir = app.string();
  opt.layers = fx.Layers();
  // No ELF header anywhere names this; only a run does.
  opt.run_sonames = {"libpulse.so.0"};

  auto scan = Libs::ScanApp(opt);
  REQUIRE(scan.unmet.size() == 1);
  CHECK(scan.unmet[0].soname == "libpulse.so.0");
  CHECK(scan.unmet[0].discovered_by == "run");

  SECTION("a log older than the install that fixed it does not resurrect the library") {
    Libs::ScanOptions stale = opt;
    stale.run_sonames = {"libpresent.so.1"};
    auto again = Libs::ScanApp(stale);
    CHECK(again.unmet.empty());
    REQUIRE_FALSE(again.notes.empty());
    CHECK(again.notes.back().find("predates") != std::string::npos);
  }
}

TEST_CASE("Libs: guest pacman is invoked the only way that works", "[sleeve][libs]") {
  REQUIRE(Backend::SetActiveBackend("powerarm"));
  auto opt = Libs::BuildGuestPacmanCommand("/rootfs/vk", {"-F", "libnspr4.so"}, 60.0);

  REQUIRE(opt.args.size() == 6);
  // unshare -r is what makes pacman believe it is root.
  CHECK(opt.args[0] == "unshare");
  CHECK(opt.args[1] == "-r");
  CHECK(opt.args[2] == "POWERarm");
  CHECK(opt.args[3] == "/usr/bin/pacman");
  CHECK(opt.args[4] == "-F");
  CHECK(opt.args[5] == "libnspr4.so");

  // Without PORTABLE=1 the emulator looks for its server socket under the namespace's
  // uid 0 and the whole thing fails before pacman starts.
  CHECK(opt.env.at("POWERARM_PORTABLE") == "1");
  CHECK(opt.env.at("POWERARM_ROOTFS") == "/rootfs/vk");

  CHECK(Process::Describe(opt).find("POWERARM_PORTABLE=1") != std::string::npos);

  SECTION("and the x86 backend gets its own emulator and its own variables") {
    REQUIRE(Backend::SetActiveBackend("fastppcx86"));
    auto other = Libs::BuildGuestPacmanCommand("/rootfs/x86", {"-Q"}, 60.0);
    CHECK(other.args[2] != "POWERarm");
    CHECK(other.env.count("POWERARM_PORTABLE") == 0);
    REQUIRE(Backend::SetActiveBackend("powerarm"));
  }
}

TEST_CASE("Libs: pacman's machine-readable files output parses into packages", "[sleeve][libs]") {
  std::string out = FilesLine("extra", "nspr", "4.36-1", "usr/lib/libnspr4.so") +
                    FilesLine("extra", "gtk3", "1:3.24.50-1", "usr/lib/libgtk-3.so.0") +
                    "a line that is not machine readable\n";

  auto matches = Libs::ParseFilesOutput(out);
  REQUIRE(matches.size() == 2);
  CHECK(matches[0].repo == "extra");
  CHECK(matches[0].package == "nspr");
  CHECK(matches[0].version == "4.36-1");
  CHECK(matches[0].soname == "libnspr4.so");
  CHECK(matches[1].package == "gtk3");
  CHECK(matches[1].soname == "libgtk-3.so.0");
}

TEST_CASE("Libs: without a file database nothing is run and nothing is guessed",
          "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-nofilesdb");
  Fixture fx(home.Root());

  std::vector<Process::ProcessOptions> seen;
  auto plan = Libs::MapSonamesToPackages(fx.base.string(), {"libnspr4.so"},
                                         FakeRunner(&seen, 0, ""));
  CHECK_FALSE(plan.files_db_present);
  CHECK(plan.packages.empty());
  CHECK_FALSE(plan.error.empty());
  // The point: a missing database is reported, not worked around by running something.
  CHECK(seen.empty());
}

TEST_CASE("Libs: sonames map to a deduplicated package set", "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-map");
  Fixture fx(home.Root());
  std::error_code ec;
  fs::create_directories(fx.overlay / "var/lib/pacman/sync", ec);
  std::ofstream(fx.overlay / "var/lib/pacman/sync/extra.files") << "";

  REQUIRE(Libs::FilesDatabasePresent(fx.Layers()));

  std::string out = FilesLine("extra", "nss", "3.108-1", "usr/lib/libnss3.so") +
                    FilesLine("extra", "nss", "3.108-1", "usr/lib/libsmime3.so") +
                    FilesLine("extra", "gtk3", "1:3.24.50-1", "usr/lib/libgtk-3.so.0");

  std::vector<Process::ProcessOptions> seen;
  auto plan = Libs::MapSonamesToPackages(
      fx.base.string(), {"libnss3.so", "libsmime3.so", "libgtk-3.so.0", "libnowhere.so.9"},
      FakeRunner(&seen, 0, out));

  REQUIRE(seen.size() == 1);
  CHECK(seen[0].args[4] == "-F");
  CHECK(seen[0].args[5] == "--machinereadable");

  // Two sonames from one package collapse to one package.
  CHECK(plan.packages == std::vector<std::string> {"gtk3", "nss"});
  CHECK(plan.unmatched == std::vector<std::string> {"libnowhere.so.9"});
  CHECK(plan.error.empty());
}

TEST_CASE("Libs: installing says exactly what it will run, and runs exactly that",
          "[sleeve][libs]") {
  REQUIRE(Backend::SetActiveBackend("powerarm"));
  std::vector<Process::ProcessOptions> seen;
  auto outcome = Libs::InstallPackages("/rootfs/vk", {"gtk3", "nss"}, FakeRunner(&seen, 0, "ok"));

  REQUIRE(seen.size() == 1);
  std::vector<std::string> expected = {"unshare", "-r",   "POWERarm", "/usr/bin/pacman", "-S",
                                       "--needed", "--noconfirm", "gtk3", "nss"};
  CHECK(seen[0].args == expected);
  CHECK(outcome.ok);
  // The line shown before the install is the line that ran.
  CHECK(outcome.command == Process::Describe(seen[0]));

  SECTION("a non-zero exit is a failure with the exit code in it") {
    std::vector<Process::ProcessOptions> failed;
    auto bad = Libs::InstallPackages("/rootfs/vk", {"gtk3"}, FakeRunner(&failed, 1, ""));
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("1") != std::string::npos);
  }

  SECTION("an empty package set runs nothing at all") {
    std::vector<Process::ProcessOptions> none;
    auto empty = Libs::InstallPackages("/rootfs/vk", {}, FakeRunner(&none, 0, ""));
    CHECK_FALSE(empty.ok);
    CHECK(none.empty());
  }
}

TEST_CASE("Libs: refreshing the file database is a -Fy and nothing else", "[sleeve][libs]") {
  REQUIRE(Backend::SetActiveBackend("powerarm"));
  std::vector<Process::ProcessOptions> seen;
  auto outcome = Libs::SyncFilesDatabase("/rootfs/vk", FakeRunner(&seen, 0, ""));

  REQUIRE(seen.size() == 1);
  CHECK(seen[0].args[4] == "-Fy");
  CHECK(seen[0].args[5] == "--noconfirm");
  CHECK(outcome.ok);
}

TEST_CASE("Libs: the overlay a rootfs uses is the one the emulator would pick",
          "[sleeve][libs]") {
  SleeveTest::ScopedTestHome home("libs-layersfor");
  Fixture fx(home.Root());

  auto withOverlay = Libs::LayersFor(fx.base.string());
  CHECK(withOverlay.base == fx.base.string());
  CHECK(withOverlay.overlay == fx.overlay.string());

  // A trailing slash is the same rootfs, not a different one.
  CHECK(Libs::LayersFor(fx.base.string() + "/").overlay == fx.overlay.string());

  std::error_code ec;
  fs::path bare = home.Root() / "bare";
  fs::create_directories(bare, ec);
  CHECK(Libs::LayersFor(bare.string()).overlay.empty());
}

TEST_CASE("Libs: a record's rootfs, executable and env become the scan's inputs",
          "[sleeve][libs]") {
  Record::AppRecord rec;
  rec.name = "code";
  rec.source.kind = "dir";
  rec.source.dir = "/opt/app";
  rec.exe = "bin/code";
  rec.rootfs = "/rootfs/vk";
  rec.env["LD_LIBRARY_PATH"] = "/opt/app/lib";

  auto opt = Libs::OptionsForRecord(rec);
  CHECK(opt.exe_path == "/opt/app/bin/code");
  CHECK(opt.app_dir == "/opt/app");
  CHECK(opt.layers.base == "/rootfs/vk");
  CHECK(opt.env.at("LD_LIBRARY_PATH") == "/opt/app/lib");
}
