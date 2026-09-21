// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "ElfInspect.h"
#include <elf.h>
#include <fstream>
#include <filesystem>

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
