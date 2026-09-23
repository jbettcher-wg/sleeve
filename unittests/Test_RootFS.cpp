// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "RootFS.h"
#include "Backend.h"
#include "TestEnv.h"

#include <filesystem>
#include <set>

using namespace Sleeve::RootFS;

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
