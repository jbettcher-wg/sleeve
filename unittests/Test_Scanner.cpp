// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "Scanner.h"
#include "Shapes.h"
#include "Paths.h"
#include "Backend.h"
#include "TestEnv.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace Sleeve;

TEST_CASE("Paths: A stable alias is preferred over the version directory", "[sleeve][paths]") {
  // The version directory is renamed on every update; a launcher pinned to it breaks
  // silently, which is how a working record and a fresh scan came to disagree.
  SleeveTest::ScopedTestHome home("paths-stable-symlink");

  fs::path app = fs::path(home.Path("Development/vscode-arm64"));
  fs::create_directories(app / "1.138.0/bin");
  fs::create_symlink("1.138.0", app / "current");

  CHECK(Paths::PreferStableSymlinkPath((app / "1.138.0").string()) == (app / "current").string());

  SECTION("and the alias is used even when several point at the same target") {
    fs::create_symlink("1.138.0", app / "stable");
    CHECK(Paths::PreferStableSymlinkPath((app / "1.138.0").string()) == (app / "current").string());
  }

  SECTION("an alias that is itself a version is not more stable") {
    fs::path other = fs::path(home.Path("Development/other-arm64"));
    fs::create_directories(other / "2.0.0");
    fs::create_symlink("2.0.0", other / "2.0");
    CHECK(Paths::PreferStableSymlinkPath((other / "2.0.0").string()) == (other / "2.0.0").string());
  }

  SECTION("a directory with no alias is left alone") {
    fs::path plain = fs::path(home.Path("Development/plain"));
    fs::create_directories(plain);
    CHECK(Paths::PreferStableSymlinkPath(plain.string()) == plain.string());
  }

  SECTION("a symlink pointing somewhere else is not mistaken for an alias") {
    fs::path decoy = fs::path(home.Path("Development/decoy"));
    fs::create_directories(decoy / "1.0.0");
    fs::create_directories(decoy / "2.0.0");
    fs::create_symlink("2.0.0", decoy / "current");
    CHECK(Paths::PreferStableSymlinkPath((decoy / "1.0.0").string()) == (decoy / "1.0.0").string());
  }
}

TEST_CASE("Scanner: One binary is one app however many records point at it", "[sleeve][scanner]") {
  // `gnupg` and `pinentry` both claimed .../usr/bin/pinentry-qt with the same title.
  SleeveTest::ScopedTestHome home("scanner-dedupe");

  std::string exe = home.Path("overlay/usr/bin/pinentry-qt");
  SleeveTest::WriteFakeElf(exe, SleeveTest::kMachineAArch64);

  std::vector<Shapes::AppCandidate> apps;
  {
    Shapes::AppCandidate a;
    a.name = "gnupg";
    a.title = "Pinentry";
    a.exe_path = exe;
    apps.push_back(a);
  }
  {
    Shapes::AppCandidate b;
    b.name = "pinentry";
    b.title = "Pinentry";
    b.exe_path = exe;
    apps.push_back(b);
  }

  size_t dropped = Scanner::DedupeByResolvedTarget(apps);
  CHECK(dropped == 1);
  REQUIRE(apps.size() == 1);
  // The surviving row is the one whose name describes the binary.
  CHECK(apps[0].name == "pinentry");
}

TEST_CASE("Scanner: Dedupe follows symlinks to the same target", "[sleeve][scanner]") {
  SleeveTest::ScopedTestHome home("scanner-dedupe-symlink");

  std::string real = home.Path("app/1.0.0/bin/tool");
  SleeveTest::WriteFakeElf(real, SleeveTest::kMachineAArch64);
  fs::create_symlink("1.0.0", fs::path(home.Path("app")) / "current");

  std::vector<Shapes::AppCandidate> apps;
  Shapes::AppCandidate viaVersion;
  viaVersion.name = "tool";
  viaVersion.exe_path = real;
  apps.push_back(viaVersion);

  Shapes::AppCandidate viaAlias;
  viaAlias.name = "tool";
  viaAlias.exe_path = home.Path("app/current/bin/tool");
  apps.push_back(viaAlias);

  CHECK(Scanner::DedupeByResolvedTarget(apps) == 1);
  CHECK(apps.size() == 1);

  SECTION("but two different binaries are two apps") {
    std::vector<Shapes::AppCandidate> distinct;
    SleeveTest::WriteFakeElf(home.Path("other/bin/tool"), SleeveTest::kMachineAArch64);
    Shapes::AppCandidate a;
    a.name = "tool";
    a.exe_path = real;
    Shapes::AppCandidate b;
    b.name = "tool";
    b.exe_path = home.Path("other/bin/tool");
    distinct.push_back(a);
    distinct.push_back(b);
    CHECK(Scanner::DedupeByResolvedTarget(distinct) == 0);
    CHECK(distinct.size() == 2);
  }
}

TEST_CASE("Scanner: A named directory is the whole answer", "[sleeve][scanner]") {
  // `sleeve scan ~/Games` used to return four pacman rows from the rootfs overlay and
  // nothing from ~/Games.
  SleeveTest::ScopedTestHome home("scanner-explicit-dir");

  // Something in the standard locations, and an overlay, both of which must stay out.
  SleeveTest::WriteFakeElf(home.Path("Development/elsewhere/bin/elsewhere"), SleeveTest::kMachineAArch64);

  std::string games = home.Path("Games");
  SleeveTest::WriteFakeElf(games + "/mygame/bin/mygame", SleeveTest::kMachineAArch64);

  REQUIRE(Backend::SetActiveBackend("powerarm"));

  Scanner::ScanOptions opts;
  opts.search_dirs = {games};
  auto result = Scanner::RunScan(opts);

  CHECK(result.explicit_dirs);
  REQUIRE(result.searched_dirs.size() == 1);
  CHECK(result.searched_dirs[0] == games);
  CHECK(result.foreign_launchers.empty());

  REQUIRE(result.apps.size() == 1);
  CHECK(result.apps[0].exe_path.rfind(games, 0) == 0);
  // Every row says where it came from.
  CHECK(result.apps[0].origin == games);

  SECTION("an empty directory reports nothing rather than something from elsewhere") {
    std::string empty = home.Path("Empty");
    fs::create_directories(empty);
    Scanner::ScanOptions emptyOpts;
    emptyOpts.search_dirs = {empty};
    auto emptyResult = Scanner::RunScan(emptyOpts);
    CHECK(emptyResult.apps.empty());
    CHECK(emptyResult.explicit_dirs);
  }
}

TEST_CASE("Scanner: Scan reports the stable path, not the version path", "[sleeve][scanner]") {
  SleeveTest::ScopedTestHome home("scanner-stable-path");

  std::string app = home.Path("Development/vscode-arm64");
  fs::create_directories(app + "/1.138.0/resources/app");
  {
    std::ofstream f(app + "/1.138.0/resources/app/product.json");
    f << "{\n  \"nameShort\": \"Code\",\n  \"applicationName\": \"code\",\n  \"ideVersion\": \"1.138.0\"\n}\n";
  }
  SleeveTest::WriteFakeElf(app + "/1.138.0/bin/code", SleeveTest::kMachineAArch64);
  fs::create_symlink("1.138.0", fs::path(app) / "current");

  REQUIRE(Backend::SetActiveBackend("powerarm"));

  Scanner::ScanOptions opts;
  opts.search_dirs = {home.Path("Development")};
  auto result = Scanner::RunScan(opts);

  REQUIRE(result.apps.size() == 1);
  const auto& cand = result.apps[0];
  CHECK(cand.name == "code");
  CHECK(cand.exe_path == app + "/current/bin/code");
  CHECK(cand.dir == app + "/current");
  CHECK(cand.exe_path.find("1.138.0") == std::string::npos);
  // The version is still reported; it just is not baked into the path.
  CHECK(cand.version == "1.138.0");
}

TEST_CASE("Scanner: A version directory with no alias is reported as it is", "[sleeve][scanner]") {
  SleeveTest::ScopedTestHome home("scanner-no-alias");

  std::string app = home.Path("Development/thing-arm64");
  SleeveTest::WriteFakeElf(app + "/3.2.1/bin/thing", SleeveTest::kMachineAArch64);

  REQUIRE(Backend::SetActiveBackend("powerarm"));

  Scanner::ScanOptions opts;
  opts.search_dirs = {home.Path("Development")};
  auto result = Scanner::RunScan(opts);

  REQUIRE(result.apps.size() == 1);
  CHECK(result.apps[0].exe_path == app + "/3.2.1/bin/thing");
}
