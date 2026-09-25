// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "ElfInspect.h"
#include "Backend.h"
#include "TestEnv.h"
#include <elf.h>
#include <fstream>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using namespace Sleeve::ElfInspect;

TEST_CASE("ElfInspect: 64-byte Probe classification", "[sleeve][elf]") {
  std::string tmpDir = "/tmp/sleeve_test_elf";
  fs::create_directories(tmpDir);

  // 1. Synthetic AArch64 Executable
  {
    std::string path = tmpDir + "/test_aarch64";
    Elf64_Ehdr ehdr {};
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_machine = EM_AARCH64;
    ehdr.e_type = ET_EXEC;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
    f.close();

    auto probe = ProbeFile(path);
    REQUIRE(probe.kind == FileKind::AArch64_Exec);
    REQUIRE(probe.machine == EM_AARCH64);
  }

  // 2. Synthetic PPC64LE Executable
  {
    std::string path = tmpDir + "/test_ppc64le";
    Elf64_Ehdr ehdr {};
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_machine = EM_PPC64;
    ehdr.e_type = ET_EXEC;

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
    f.close();

    auto probe = ProbeFile(path);
    REQUIRE(probe.kind == FileKind::Foreign_PPC64LE);
  }

  // 3. Synthetic X86_64 Executable
  {
    std::string path = tmpDir + "/test_x86_64";
    Elf64_Ehdr ehdr {};
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_type = ET_EXEC;

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
    f.close();

    auto probe = ProbeFile(path);
    REQUIRE(probe.kind == FileKind::Foreign_X86_64);
  }

  // 4. Synthetic AppImage
  {
    std::string path = tmpDir + "/test_appimage";
    Elf64_Ehdr ehdr {};
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[8] = 'A';
    ehdr.e_ident[9] = 'I';
    ehdr.e_ident[10] = 0x02;
    ehdr.e_machine = EM_AARCH64;
    ehdr.e_type = ET_EXEC;
    ehdr.e_shoff = 1024;
    ehdr.e_shnum = 10;
    ehdr.e_shentsize = 64;

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
    f.close();

    auto probe = ProbeFile(path);
    REQUIRE(probe.kind == FileKind::AppImage);
    REQUIRE(probe.is_appimage == true);
    REQUIRE(probe.appimage_offset == 1024 + 10 * 64);
  }

  fs::remove_all(tmpDir);
}

TEST_CASE("ElfInspect: Position-independent executables count as target binaries", "[sleeve][elf]") {
  // `sleeve add` used to require probe.has_interp, which the 64-byte probe never sets, so
  // every PIE -- which is to say almost every modern binary -- was invisible to it while
  // `sleeve scan` found it.
  std::string tmpDir = "/tmp/sleeve_test_elf_pie";
  fs::remove_all(tmpDir);
  fs::create_directories(tmpDir);

  auto write = [&](const std::string& name, uint16_t machine, uint16_t type) {
    std::string path = tmpDir + "/" + name;
    Elf64_Ehdr ehdr {};
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_machine = machine;
    ehdr.e_type = type;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
    return path;
  };

  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));

  auto pie = ProbeFile(write("pie", EM_AARCH64, ET_DYN));
  CHECK(pie.kind == FileKind::AArch64_Dyn);
  CHECK(pie.has_interp == false); // the probe does not read program headers
  CHECK(IsTargetBinary(pie));

  auto exec = ProbeFile(write("exec", EM_AARCH64, ET_EXEC));
  CHECK(IsTargetBinary(exec));

  auto foreign = ProbeFile(write("foreign", EM_X86_64, ET_DYN));
  CHECK_FALSE(IsTargetBinary(foreign));

  SECTION("and the x86 backend sees the mirror image") {
    REQUIRE(Sleeve::Backend::SetActiveBackend("fastppcx86"));
    CHECK(IsTargetBinary(ProbeFile(tmpDir + "/foreign")));
    CHECK_FALSE(IsTargetBinary(ProbeFile(tmpDir + "/pie")));
    REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));
  }

  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));
  fs::remove_all(tmpDir);
}

TEST_CASE("ElfInspect: the dynamic section gives up its search paths, not just its needs",
          "[sleeve][elf]") {
  // The dependency walk stands on these: a bundled application finds the libraries it
  // ships through DT_RUNPATH=$ORIGIN, and without reading them every one of them reads
  // as missing.
  SleeveTest::ScopedTestHome home("elf-dynamic");
  REQUIRE(Sleeve::Backend::SetActiveBackend("powerarm"));

  SleeveTest::DynamicElfSpec spec;
  spec.soname = "libthing.so.1";
  spec.needed = {"libc.so.6", "libm.so.6"};
  spec.runpath = {"$ORIGIN", "$ORIGIN/../lib"};
  auto path = home.Root() / "libthing.so.1";
  SleeveTest::WriteDynamicElf(path, spec);

  auto details = InspectTarget(path.string());
  REQUIRE(details.has_value());
  CHECK(details->soname == "libthing.so.1");
  CHECK(details->needed_libs == std::vector<std::string> {"libc.so.6", "libm.so.6"});
  // One DT_RUNPATH string, split on ':' the way the loader splits it.
  CHECK(details->runpath == std::vector<std::string> {"$ORIGIN", "$ORIGIN/../lib"});
  CHECK(details->rpath.empty());

  SECTION("DT_RPATH is kept apart from DT_RUNPATH, because the loader treats them apart") {
    SleeveTest::DynamicElfSpec old;
    old.needed = {"libc.so.6"};
    old.rpath = {"/opt/app/lib"};
    auto oldPath = home.Root() / "libold.so.1";
    SleeveTest::WriteDynamicElf(oldPath, old);

    auto oldDetails = InspectTarget(oldPath.string());
    REQUIRE(oldDetails.has_value());
    CHECK(oldDetails->rpath == std::vector<std::string> {"/opt/app/lib"});
    CHECK(oldDetails->runpath.empty());
  }

  SECTION("a foreign object is still readable when the architecture gate is not applied") {
    SleeveTest::DynamicElfSpec foreign;
    foreign.machine = SleeveTest::kMachineX86_64;
    foreign.needed = {"libfoo.so.1"};
    auto foreignPath = home.Root() / "libforeign.so";
    SleeveTest::WriteDynamicElf(foreignPath, foreign);

    CHECK_FALSE(InspectTarget(foreignPath.string()).has_value());
    auto any = InspectElf64(foreignPath.string());
    REQUIRE(any.has_value());
    CHECK(any->probe.machine == SleeveTest::kMachineX86_64);
  }
}
