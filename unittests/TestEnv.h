// SPDX-License-Identifier: MIT
#pragma once

// Test scaffolding. Nothing here may touch the invoking user's real HOME: the record,
// launcher, desktop and AppConfig paths are all derived from it, and a test run must not
// be able to write into the files a person actually launches apps with.

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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

} // namespace SleeveTest
