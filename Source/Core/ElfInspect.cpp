// SPDX-License-Identifier: MIT
#include "ElfInspect.h"
#include "Backend.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace Sleeve::ElfInspect {

ProbeResult ProbeFile(const std::string& path) {
  ProbeResult res;

  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return res;
  }

  uint8_t hdr[64];
  ssize_t n = ::read(fd, hdr, sizeof(hdr));
  if (n < 16) {
    ::close(fd);
    return res;
  }

  // Check ELF magic: \x7f E L F
  if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
    uint8_t elf_class = hdr[4];
    uint8_t elf_data = hdr[5];

    // Check AppImage magic at offset 8: 'A' 'I' 0x02
    if (n >= 11 && hdr[8] == 'A' && hdr[9] == 'I' && hdr[10] == 0x02) {
      res.is_appimage = true;
    }

    if (n >= 64 && elf_class == ELFCLASS64 && elf_data == ELFDATA2LSB) {
      Elf64_Ehdr ehdr;
      std::memcpy(&ehdr, hdr, sizeof(ehdr));

      res.machine = ehdr.e_machine;
      res.type = ehdr.e_type;

      if (res.is_appimage) {
        // AppImage squashfs offset is e_shoff + e_shnum * e_shentsize
        res.appimage_offset = ehdr.e_shoff + (static_cast<uint64_t>(ehdr.e_shnum) * ehdr.e_shentsize);
        res.kind = FileKind::AppImage;
      } else if (ehdr.e_machine == EM_AARCH64) {
        if (ehdr.e_type == ET_EXEC) {
          res.kind = FileKind::AArch64_Exec;
        } else if (ehdr.e_type == ET_DYN) {
          res.kind = FileKind::AArch64_Dyn;
        } else {
          res.kind = FileKind::Unknown;
        }
      } else if (ehdr.e_machine == EM_PPC64) {
        res.kind = FileKind::Foreign_PPC64LE;
      } else if (ehdr.e_machine == EM_X86_64) {
        res.kind = FileKind::Foreign_X86_64;
      } else {
        res.kind = FileKind::Foreign_Other;
      }
    } else if (n >= 52 && elf_class == ELFCLASS32 && elf_data == ELFDATA2LSB) {
      Elf32_Ehdr ehdr;
      std::memcpy(&ehdr, hdr, sizeof(ehdr));

      res.machine = ehdr.e_machine;
      res.type = ehdr.e_type;
      res.kind = FileKind::Foreign_Other;
    } else {
      res.kind = FileKind::Foreign_Other;
    }
    ::close(fd);
    return res;
  }

  // Check Archive signatures
  if (n >= 8 && std::memcmp(hdr, "!<arch>\n", 8) == 0) {
    res.kind = FileKind::DebArchive;
  } else if (n >= 2 && hdr[0] == 0x1f && hdr[1] == 0x8b) {
    res.kind = FileKind::TarArchive; // tar.gz
  } else if (n >= 6 && hdr[0] == 0xfd && hdr[1] == '7' && hdr[2] == 'z' && hdr[3] == 'X' && hdr[4] == 'Z' && hdr[5] == 0x00) {
    res.kind = FileKind::TarArchive; // tar.xz
  } else if (n >= 4 && hdr[0] == 0x28 && hdr[1] == 0xb5 && hdr[2] == 0x2f && hdr[3] == 0xfd) {
    res.kind = FileKind::TarArchive; // tar.zst
  } else {
    res.kind = FileKind::NotElf;
  }

  ::close(fd);
  return res;
}

std::optional<ElfDetails> InspectTarget(const std::string& path) {
  ProbeResult probe = ProbeFile(path);
  if (!IsTargetBinary(probe)) {
    return std::nullopt;
  }
  return InspectElf64(path);
}

// DT_RPATH and DT_RUNPATH are one string of colon-separated directories.
static void AppendSplitPaths(const char* value, std::vector<std::string>& out) {
  std::string all(value);
  size_t start = 0;
  while (start <= all.size()) {
    size_t colon = all.find(':', start);
    std::string part = all.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!part.empty()) {
      out.push_back(part);
    }
    if (colon == std::string::npos) break;
    start = colon + 1;
  }
}

std::optional<ElfDetails> InspectElf64(const std::string& path) {
  ProbeResult probe = ProbeFile(path);

  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::nullopt;
  }

  struct stat st;
  if (::fstat(fd, &st) < 0) {
    ::close(fd);
    return std::nullopt;
  }

  Elf64_Ehdr ehdr;
  if (::pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr)) {
    ::close(fd);
    return std::nullopt;
  }

  // Everything below reads Elf64 little-endian structures. A 32-bit or big-endian file
  // gets its header fields from the wrong offsets, so refuse it here rather than
  // reporting the garbage that comes out.
  if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
      ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
    ::close(fd);
    return std::nullopt;
  }

  if (ehdr.e_phnum == 0 || ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
    ::close(fd);
    return std::nullopt;
  }

  std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
  if (::pread(fd, phdrs.data(), sizeof(Elf64_Phdr) * ehdr.e_phnum, ehdr.e_phoff) != static_cast<ssize_t>(sizeof(Elf64_Phdr) * ehdr.e_phnum)) {
    ::close(fd);
    return std::nullopt;
  }

  ElfDetails details;
  details.probe = probe;
  details.file_size = st.st_size;

  auto vaToFile = [&](uint64_t va) -> off_t {
    for (const auto& ph : phdrs) {
      if (ph.p_vaddr <= va && (ph.p_vaddr + ph.p_memsz) > va) {
        auto diff = va - ph.p_vaddr;
        if (diff < ph.p_filesz) {
          return ph.p_offset + diff;
        }
      }
    }
    return -1;
  };

  off_t dynamic_offset = -1;
  size_t dynamic_size = 0;

  for (const auto& ph : phdrs) {
    if (ph.p_type == PT_INTERP) {
      details.probe.has_interp = true;
      if (ph.p_filesz > 0 && ph.p_filesz < 4096) {
        std::string interp(ph.p_filesz, '\0');
        if (::pread(fd, interp.data(), ph.p_filesz, ph.p_offset) == static_cast<ssize_t>(ph.p_filesz)) {
          while (!interp.empty() && (interp.back() == '\0' || interp.back() == '\n' || interp.back() == '\r')) {
            interp.pop_back();
          }
          details.interpreter = interp;
        }
      }
    } else if (ph.p_type == PT_DYNAMIC) {
      dynamic_offset = ph.p_offset;
      dynamic_size = ph.p_filesz;
    }
  }

  if (dynamic_offset >= 0 && dynamic_size >= sizeof(Elf64_Dyn)) {
    size_t dyn_count = dynamic_size / sizeof(Elf64_Dyn);
    std::vector<Elf64_Dyn> dyns(dyn_count);
    if (::pread(fd, dyns.data(), dynamic_size, dynamic_offset) == static_cast<ssize_t>(dynamic_size)) {
      uint64_t strtab_va = 0;
      uint64_t strtab_sz = 0;
      std::vector<uint64_t> needed_offsets;
      std::vector<uint64_t> rpath_offsets;
      std::vector<uint64_t> runpath_offsets;
      uint64_t soname_offset = 0;
      bool has_soname = false;

      for (const auto& dyn : dyns) {
        if (dyn.d_tag == DT_STRTAB) {
          strtab_va = dyn.d_un.d_ptr;
        } else if (dyn.d_tag == DT_STRSZ) {
          strtab_sz = dyn.d_un.d_val;
        } else if (dyn.d_tag == DT_NEEDED) {
          needed_offsets.push_back(dyn.d_un.d_val);
        } else if (dyn.d_tag == DT_RPATH) {
          rpath_offsets.push_back(dyn.d_un.d_val);
        } else if (dyn.d_tag == DT_RUNPATH) {
          runpath_offsets.push_back(dyn.d_un.d_val);
        } else if (dyn.d_tag == DT_SONAME) {
          soname_offset = dyn.d_un.d_val;
          has_soname = true;
        } else if (dyn.d_tag == DT_NULL) {
          break;
        }
      }

      off_t strtab_offset = vaToFile(strtab_va);
      if (strtab_offset >= 0 && strtab_sz > 0 && strtab_sz < 10 * 1024 * 1024) {
        std::vector<char> strtab(strtab_sz);
        if (::pread(fd, strtab.data(), strtab_sz, strtab_offset) == static_cast<ssize_t>(strtab_sz)) {
          // A string table read off disk is not guaranteed to end in a NUL, and every
          // read below is a C string walk from an attacker-supplied offset.
          strtab.back() = '\0';
          for (auto off : needed_offsets) {
            if (off < strtab_sz) {
              details.needed_libs.push_back(std::string(strtab.data() + off));
            }
          }
          for (auto off : rpath_offsets) {
            if (off < strtab_sz) {
              AppendSplitPaths(strtab.data() + off, details.rpath);
            }
          }
          for (auto off : runpath_offsets) {
            if (off < strtab_sz) {
              AppendSplitPaths(strtab.data() + off, details.runpath);
            }
          }
          if (has_soname && soname_offset < strtab_sz) {
            details.soname = std::string(strtab.data() + soname_offset);
          }
        }
      }
    }
  }

  ::close(fd);
  return details;
}

bool IsTargetBinary(const ProbeResult& probe) {
  const auto& backend = Backend::GetActiveBackend();
  if (backend.targetArch == Backend::TargetArch::AArch64) {
    return probe.kind == FileKind::AArch64_Exec || probe.kind == FileKind::AArch64_Dyn || probe.machine == EM_AARCH64;
  } else if (backend.targetArch == Backend::TargetArch::X86_64) {
    return probe.machine == EM_X86_64 || probe.machine == EM_386 || probe.kind == FileKind::Foreign_X86_64;
  }
  return false;
}

} // namespace Sleeve::ElfInspect
