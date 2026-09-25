// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "RootFS.h"
#include "Backend.h"
#include "Process.h"
#include "Record.h"
#include "TestEnv.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <vector>

namespace fs = std::filesystem;
using namespace Sleeve::RootFS;
namespace RootFS = Sleeve::RootFS;

TEST_CASE("RootFS: FormatBytes formatting", "[sleeve][rootfs]") {
  CHECK(FormatBytes(500) == "500 B");
  CHECK(FormatBytes(2048) == "2 KB");
  CHECK(FormatBytes(50 * 1024 * 1024) == "50 MB");
  CHECK(FormatBytes(1536ULL * 1024ULL * 1024ULL) == "1.5 GB");
}

TEST_CASE("RootFS: Architecture is read from the tree, not inferred from its directory", "[sleeve][rootfs]") {
  SleeveTest::ScopedTestHome home("rootfs-machine");
  namespace fs = std::filesystem;

  std::string arm = home.Path("arm-rootfs");
  SleeveTest::WriteFakeElf(fs::path(arm) / "usr/bin/env", SleeveTest::kMachineAArch64);

  std::string x86 = home.Path("x86-rootfs");
  SleeveTest::WriteFakeElf(fs::path(x86) / "usr/bin/env", SleeveTest::kMachineX86_64);

  CHECK(DetectRootFSMachine(arm) == SleeveTest::kMachineAArch64);
  CHECK(DetectRootFSMachine(x86) == SleeveTest::kMachineX86_64);

  SECTION("an empty tree cannot be established and is not guessed at") {
    std::string empty = home.Path("empty-rootfs");
    fs::create_directories(empty);
    CHECK(DetectRootFSMachine(empty) == 0);
  }

  SECTION("an absolute symlink is re-rooted at the rootfs, not at the host") {
    std::string tricky = home.Path("tricky-rootfs");
    SleeveTest::WriteFakeElf(fs::path(tricky) / "usr/bin/bash", SleeveTest::kMachineAArch64);
    fs::create_directories(fs::path(tricky) / "bin");
    // The guest sees /usr/bin/bash inside its own tree; resolving this against the host's
    // root would probe a ppc64le binary and reject a perfectly good rootfs.
    fs::create_symlink("/usr/bin/bash", fs::path(tricky) / "bin/sh");
    fs::remove(fs::path(tricky) / "usr/bin/env");
    CHECK(DetectRootFSMachine(tricky) == SleeveTest::kMachineAArch64);
  }

  SECTION("a link that climbs out of the tree resolves to nothing") {
    std::string escape = home.Path("escape-rootfs");
    fs::create_directories(fs::path(escape) / "usr/bin");
    fs::create_symlink("../../../../../../usr/bin/env", fs::path(escape) / "usr/bin/env");
    CHECK(DetectRootFSMachine(escape) == 0);
  }
}

TEST_CASE("RootFS: Discovery is scoped to the active backend", "[sleeve][rootfs]") {
  SleeveTest::ScopedTestHome home("rootfs-discovery");
  namespace fs = std::filesystem;

  // The backend's own store.
  SleeveTest::WriteFakeElf(fs::path(home.Path(".local/share/powerarm/RootFS/ArchLinuxARM-m2")) / "usr/bin/env",
                           SleeveTest::kMachineAArch64);
  // The other backend's checkout-local store. Offering this for an AArch64 guest is the
  // bug: a named rootfs that does not resolve falls back to host binaries.
  SleeveTest::WriteFakeElf(fs::path(home.Path("Development/fexrootfs/RootFS/Ubuntu_24_04")) / "usr/bin/env",
                           SleeveTest::kMachineX86_64);
  // And a wrong-architecture tree sitting in this backend's own store.
  SleeveTest::WriteFakeElf(fs::path(home.Path(".local/share/powerarm/RootFS/Impostor")) / "usr/bin/env",
                           SleeveTest::kMachineX86_64);

  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));
  auto result = DiscoverRootFSes();

  std::set<std::string> names;
  for (const auto& r : result.rootfses) names.insert(r.name);

  CHECK(names.count("ArchLinuxARM-m2") == 1);
  CHECK(names.count("Ubuntu_24_04") == 0);
  CHECK(names.count("Impostor") == 0);

  for (const auto& r : result.rootfses) {
    if (r.name == "ArchLinuxARM-m2") {
      CHECK(r.arch_verified);
      CHECK(r.elf_machine == SleeveTest::kMachineAArch64);
    }
  }

  SECTION("and the x86 backend sees the mirror image") {
    REQUIRE(Sleeve::Backend::SetActiveBackend("fastppcx86"));
    auto x86Result = DiscoverRootFSes();
    std::set<std::string> x86Names;
    for (const auto& r : x86Result.rootfses) x86Names.insert(r.name);
    CHECK(x86Names.count("Ubuntu_24_04") == 1);
    CHECK(x86Names.count("ArchLinuxARM-m2") == 0);
    REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));
  }

  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));
}

TEST_CASE("RootFS: Naming a rootfs the backend cannot run is refused", "[sleeve][rootfs]") {
  SleeveTest::ScopedTestHome home("rootfs-resolve");
  namespace fs = std::filesystem;

  SleeveTest::WriteFakeElf(fs::path(home.Path(".local/share/powerarm/RootFS/ArchLinuxARM-vk")) / "usr/bin/env",
                           SleeveTest::kMachineAArch64);
  std::string foreign = home.Path("Development/fexrootfs/RootFS/Ubuntu_24_04");
  SleeveTest::WriteFakeElf(fs::path(foreign) / "usr/bin/env", SleeveTest::kMachineX86_64);

  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));

  auto good = ResolveRootFSName("ArchLinuxARM-vk");
  CHECK(good.ok);
  CHECK(good.path == home.Path(".local/share/powerarm/RootFS/ArchLinuxARM-vk"));

  auto unknown = ResolveRootFSName("Ubuntu_24_04");
  CHECK_FALSE(unknown.ok);
  CHECK_FALSE(unknown.error.empty());

  auto wrongArch = ResolveRootFSName(foreign);
  CHECK_FALSE(wrongArch.ok);
  CHECK_FALSE(wrongArch.error.empty());
}

// ---------------------------------------------------------------------------
// Readiness and provisioning
//
// A missing rootfs is an unmet precondition like a missing library: sleeve names it
// exactly and drives POWERarmRootFSFetcher to fix it. Nothing here builds a rootfs, so
// what is tested is what would be run.
// ---------------------------------------------------------------------------

namespace {

// A rootfs tree that the architecture probe will accept, with the pieces readiness looks
// at: the tree's own userspace, and optionally an overlay with guest pacman in it.
void MakeRootFSTree(const fs::path& path, uint16_t machine) {
  SleeveTest::WriteFakeElf(path / "usr/bin/env", machine);
  SleeveTest::WriteFakeElf(path / "usr/bin/ls", machine);
}

} // namespace

TEST_CASE("RootFS: readiness tells the four kinds of unusable apart", "[sleeve][rootfs]") {
  SleeveTest::ScopedTestHome home("rootfs-readiness");
  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));

  SECTION("nothing there at all is the case worth building for") {
    auto report = RootFS::CheckReadiness("ArchLinuxARM-nope");
    CHECK(report.state == RootFS::Readiness::Missing);
    CHECK(report.auto_fixable);
    CHECK(report.fix_hint.find("POWERarmRootFSFetcher build") != std::string::npos);
  }

  SECTION("a tree of the wrong architecture is not repaired in place by accident") {
    fs::path tree = home.Root() / "x86tree";
    MakeRootFSTree(tree, SleeveTest::kMachineX86_64);
    auto report = RootFS::CheckReadiness(tree.string());
    CHECK(report.state == RootFS::Readiness::WrongArch);
    CHECK(report.elf_machine == SleeveTest::kMachineX86_64);
    CHECK(report.auto_fixable);
    // Rebuilding over it has to be explicit about where and about replacing it.
    CHECK(report.fix_hint.find("--force") != std::string::npos);
    CHECK(report.fix_hint.find(tree.string()) != std::string::npos);
  }

  SECTION("a base with no overlay has nowhere for guest pacman to install into") {
    fs::path tree = home.Root() / "baseonly";
    MakeRootFSTree(tree, SleeveTest::kMachineAArch64);
    auto report = RootFS::CheckReadiness(tree.string());
    CHECK(report.state == RootFS::Readiness::NoOverlay);
    CHECK(report.overlay_path.empty());
    // A repair needs the manifest that built the base, which only the person knows.
    CHECK_FALSE(report.auto_fixable);
  }

  SECTION("a non-empty overlay is not a finished one") {
    // The shape a half-finished build leaves behind: the package database landed, the
    // packages did not, so /usr/bin/pacman is missing and installing anything into it
    // fails in a way that reads like an emulator problem.
    fs::path tree = home.Root() / "halfdone";
    MakeRootFSTree(tree, SleeveTest::kMachineAArch64);
    std::error_code ec;
    fs::create_directories(home.Root() / "halfdone-overlay/var/lib/pacman/local", ec);

    auto report = RootFS::CheckReadiness(tree.string());
    CHECK(report.state == RootFS::Readiness::OverlayIncomplete);
    CHECK_FALSE(report.guest_pacman);
    CHECK_FALSE(report.auto_fixable);
    CHECK(report.fix_hint.find("--force") != std::string::npos);
  }

  SECTION("a base, an overlay and guest pacman in it is the usable case") {
    fs::path tree = home.Root() / "good";
    MakeRootFSTree(tree, SleeveTest::kMachineAArch64);
    SleeveTest::WriteFakeElf(home.Root() / "good-overlay/usr/bin/pacman",
                             SleeveTest::kMachineAArch64);

    auto report = RootFS::CheckReadiness(tree.string());
    CHECK(report.state == RootFS::Readiness::Ok);
    CHECK(report.guest_pacman);
    CHECK(report.overlay_path == (home.Root() / "good-overlay").string());
    CHECK(report.fix_hint.empty());
  }
}

TEST_CASE("RootFS: the fetcher is driven, not reimplemented", "[sleeve][rootfs]") {
  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));

  RootFS::ProvisionOptions opts;
  opts.name = "ArchLinuxARM-vk";
  opts.dest = "/scratch/vk";
  opts.force = true;
  opts.mirrors = {"https://mirror.one/archrootfs", "https://mirror.two/aarch64"};

  auto cmd = RootFS::BuildProvisionCommand(opts);
  std::vector<std::string> expected = {"POWERarmRootFSFetcher",
                                       "build",
                                       "ArchLinuxARM-vk",
                                       "--manifest=vk",
                                       "--dest=/scratch/vk",
                                       "--mirror=https://mirror.one/archrootfs",
                                       "--mirror=https://mirror.two/aarch64",
                                       "--force",
                                       "-y"};
  CHECK(cmd.args == expected);

  SECTION("no mirrors means the fetcher's own defaults, not one of sleeve's") {
    RootFS::ProvisionOptions plain;
    auto bare = RootFS::BuildProvisionCommand(plain);
    for (const auto& arg : bare.args) {
      CHECK(arg.rfind("--mirror", 0) != 0);
    }
    // A name is the fetcher's to default too.
    CHECK(bare.args == std::vector<std::string> {"POWERarmRootFSFetcher", "build", "--manifest=vk", "-y"});
  }

  SECTION("--dry-run stops the fetcher before it downloads anything") {
    RootFS::ProvisionOptions preview;
    preview.dry_run = true;
    auto cmdPreview = RootFS::BuildProvisionCommand(preview);
    CHECK(std::find(cmdPreview.args.begin(), cmdPreview.args.end(), "--dry-run") !=
          cmdPreview.args.end());
  }

  SECTION("repairing an overlay is the fetcher's overlay command, not a build") {
    RootFS::ProvisionOptions repair;
    repair.name = "ArchLinuxARM-vk";
    repair.force = true;
    auto overlay = RootFS::BuildOverlayCommand(repair);
    CHECK(overlay.args[1] == "overlay");
    CHECK(std::find(overlay.args.begin(), overlay.args.end(), "--force") != overlay.args.end());
  }
}

TEST_CASE("RootFS: mirrors are configuration, not code", "[sleeve][rootfs]") {
  SleeveTest::ScopedTestHome home("rootfs-mirrors");

  // Nothing configured: sleeve contributes no mirror of its own, so the fetcher keeps
  // using POWERarm's pinned snapshot and upstream.
  CHECK(RootFS::ProvisionOptionsFromSettings().mirrors.empty());

  Sleeve::Record::Settings settings;
  settings.rootfs_mirrors = {"https://mirror.example/archrootfs"};
  REQUIRE(Sleeve::Record::SaveSettings(settings));

  auto loaded = RootFS::ProvisionOptionsFromSettings();
  REQUIRE(loaded.mirrors.size() == 1);
  CHECK(loaded.mirrors[0] == "https://mirror.example/archrootfs");
  // And it reaches the command as a repeatable pass-through.
  auto cmd = RootFS::BuildProvisionCommand(loaded);
  CHECK(std::find(cmd.args.begin(), cmd.args.end(), "--mirror=https://mirror.example/archrootfs") !=
        cmd.args.end());
}

TEST_CASE("RootFS: provisioning reports what it ran and what went wrong", "[sleeve][rootfs]") {
  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));

  std::vector<Sleeve::Process::ProcessOptions> seen;
  auto runner = [&seen](const Sleeve::Process::ProcessOptions& options) {
    seen.push_back(options);
    Sleeve::Process::ProcessResult res;
    res.exit_code = 3;
    res.stderr_str = "no mirror answered\n";
    return res;
  };

  RootFS::ProvisionOptions opts;
  auto outcome = RootFS::Provision(opts, runner);
  REQUIRE(seen.size() == 1);
  CHECK_FALSE(outcome.ok);
  CHECK(outcome.output.find("no mirror answered") != std::string::npos);
  CHECK(outcome.error.find("3") != std::string::npos);
  CHECK(outcome.command == Sleeve::Process::Describe(seen[0]));
}
