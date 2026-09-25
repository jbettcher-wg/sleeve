// SPDX-License-Identifier: MIT
#pragma once

// Test scaffolding. Nothing here may touch the invoking user's real HOME: the record,
// launcher, desktop and AppConfig paths are all derived from it, and a test run must not
// be able to write into the files a person actually launches apps with.

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace SleeveTest {

namespace fs = std::filesystem;

// Points HOME (and clears the XDG overrides that would otherwise win) at a fresh
// temporary directory for the lifetime of the object, then restores the environment.
class ScopedTestHome {
public:
  explicit ScopedTestHome(const std::string& label) {
    root_ = fs::temp_directory_path() /
            ("sleeve-test-" + label + "-" + std::to_string(static_cast<long>(::getpid())));
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_, ec);

    for (const char* name : kVars) {
      const char* value = std::getenv(name);
      saved_.push_back(value ? std::optional<std::string>(value) : std::nullopt);
    }

    ::setenv("HOME", root_.string().c_str(), 1);
    ::unsetenv("XDG_CONFIG_HOME");
    ::unsetenv("XDG_DATA_HOME");
    ::unsetenv("XDG_CACHE_HOME");
  }

  ~ScopedTestHome() {
    for (size_t i = 0; i < saved_.size(); ++i) {
      if (saved_[i]) {
        ::setenv(kVars[i], saved_[i]->c_str(), 1);
      } else {
        ::unsetenv(kVars[i]);
      }
    }
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  ScopedTestHome(const ScopedTestHome&) = delete;
  ScopedTestHome& operator=(const ScopedTestHome&) = delete;

  const fs::path& Root() const { return root_; }
  std::string Path(const std::string& relative) const { return (root_ / relative).string(); }

private:
  static constexpr const char* kVars[] = {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"};
  fs::path root_;
  std::vector<std::optional<std::string>> saved_;
};

// A 64-byte ELF header, which is all ElfInspect::ProbeFile reads.
inline void WriteFakeElf(const fs::path& path, uint16_t machine, uint16_t type = 2 /* ET_EXEC */) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);

  unsigned char hdr[64] = {};
  hdr[0] = 0x7f;
  hdr[1] = 'E';
  hdr[2] = 'L';
  hdr[3] = 'F';
  hdr[4] = 2; // ELFCLASS64
  hdr[5] = 1; // ELFDATA2LSB
  hdr[6] = 1; // EV_CURRENT
  hdr[16] = static_cast<unsigned char>(type & 0xff);
  hdr[17] = static_cast<unsigned char>((type >> 8) & 0xff);
  hdr[18] = static_cast<unsigned char>(machine & 0xff);
  hdr[19] = static_cast<unsigned char>((machine >> 8) & 0xff);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
  f.close();

  fs::permissions(path,
                  fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                      fs::perms::others_read | fs::perms::others_exec,
                  ec);
}

constexpr uint16_t kMachineAArch64 = 183;
constexpr uint16_t kMachineX86_64 = 62;
constexpr uint16_t kMachinePPC64 = 21;

// A real, if minimal, ELF64 shared object: one PT_LOAD covering the whole file so that
// virtual addresses map straight to file offsets, one PT_DYNAMIC, and a string table.
// This is what it takes to exercise the DT_NEEDED / DT_RPATH / DT_RUNPATH parse, which a
// bare 64-byte header cannot.
struct DynamicElfSpec {
  uint16_t machine {kMachineAArch64};
  uint16_t type {3}; // ET_DYN
  std::vector<std::string> needed;
  std::vector<std::string> rpath;   // written as one DT_RPATH, joined with ':'
  std::vector<std::string> runpath; // written as one DT_RUNPATH
  std::string soname;
};

inline void WriteDynamicElf(const fs::path& path, const DynamicElfSpec& spec) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);

  auto join = [](const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? ":" : "") + parts[i];
    return out;
  };

  // The string table: offset 0 is the empty string, by convention.
  std::string strtab(1, '\0');
  auto intern = [&strtab](const std::string& value) {
    uint64_t offset = strtab.size();
    strtab += value;
    strtab.push_back('\0');
    return offset;
  };

  struct Entry {
    int64_t tag;
    uint64_t value;
  };
  std::vector<Entry> dynamic;
  for (const auto& n : spec.needed) dynamic.push_back({1 /* DT_NEEDED */, intern(n)});
  if (!spec.soname.empty()) dynamic.push_back({14 /* DT_SONAME */, intern(spec.soname)});
  if (!spec.rpath.empty()) dynamic.push_back({15 /* DT_RPATH */, intern(join(spec.rpath))});
  if (!spec.runpath.empty()) dynamic.push_back({29 /* DT_RUNPATH */, intern(join(spec.runpath))});

  constexpr uint64_t kEhdrSize = 64;
  constexpr uint64_t kPhdrSize = 56;
  constexpr uint64_t kPhdrCount = 2;
  const uint64_t dynOffset = kEhdrSize + kPhdrSize * kPhdrCount;
  // DT_STRTAB and DT_STRSZ, then DT_NULL, come after everything interned above.
  const uint64_t dynCount = dynamic.size() + 3;
  const uint64_t dynSize = dynCount * 16;
  const uint64_t strOffset = dynOffset + dynSize;
  const uint64_t total = strOffset + strtab.size();

  dynamic.push_back({5 /* DT_STRTAB */, strOffset});
  dynamic.push_back({10 /* DT_STRSZ */, strtab.size()});
  dynamic.push_back({0 /* DT_NULL */, 0});

  std::vector<unsigned char> image(total, 0);
  auto put16 = [&image](size_t at, uint16_t v) {
    image[at] = static_cast<unsigned char>(v & 0xff);
    image[at + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
  };
  auto put32 = [&image](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) image[at + i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
  };
  auto put64 = [&image](size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) image[at + i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
  };

  image[0] = 0x7f;
  image[1] = 'E';
  image[2] = 'L';
  image[3] = 'F';
  image[4] = 2; // ELFCLASS64
  image[5] = 1; // ELFDATA2LSB
  image[6] = 1; // EV_CURRENT
  put16(16, spec.type);
  put16(18, spec.machine);
  put32(20, 1);                                      // e_version
  put64(32, kEhdrSize);                              // e_phoff
  put16(52, static_cast<uint16_t>(kEhdrSize));       // e_ehsize
  put16(54, static_cast<uint16_t>(kPhdrSize));       // e_phentsize
  put16(56, static_cast<uint16_t>(kPhdrCount));      // e_phnum

  // PT_LOAD over the whole file, with p_vaddr == p_offset so the address-to-offset map
  // in ElfInspect is the identity.
  size_t ph = kEhdrSize;
  put32(ph + 0, 1); // PT_LOAD
  put32(ph + 4, 5); // PF_R | PF_X
  put64(ph + 8, 0);
  put64(ph + 16, 0);
  put64(ph + 24, 0);
  put64(ph + 32, total);
  put64(ph + 40, total);

  ph = kEhdrSize + kPhdrSize;
  put32(ph + 0, 2); // PT_DYNAMIC
  put32(ph + 4, 6); // PF_R | PF_W
  put64(ph + 8, dynOffset);
  put64(ph + 16, dynOffset);
  put64(ph + 24, dynOffset);
  put64(ph + 32, dynSize);
  put64(ph + 40, dynSize);

  size_t at = dynOffset;
  for (const auto& entry : dynamic) {
    put64(at, static_cast<uint64_t>(entry.tag));
    put64(at + 8, entry.value);
    at += 16;
  }
  std::copy(strtab.begin(), strtab.end(), image.begin() + strOffset);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  f.close();

  fs::permissions(path,
                  fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                      fs::perms::others_read | fs::perms::others_exec,
                  ec);
}

} // namespace SleeveTest
