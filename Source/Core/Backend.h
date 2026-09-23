// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <cstdint>

namespace Sleeve::Backend {

enum class TargetArch {
  AArch64,
  X86_64,
};

struct BackendSpec {
  std::string id {"powerarm"};
  std::string displayName {"POWERarm"};
  std::string emulatorBin {"POWERarm"};
  std::string envPrefix {"POWERARM_"};
  std::string configSubdir {"powerarm"};
  std::string dataSubdir {"powerarm"};
  // Checkout-local RootFS store for this backend: ~/Development/<devSubdir>/RootFS
  std::string devSubdir {"powerarm"};
  TargetArch targetArch {TargetArch::AArch64};
  uint16_t elfMachine {183}; // EM_AARCH64 = 183, EM_X86_64 = 62
  std::string archName {"arm64"};
  std::string defaultRootfsBase {"ArchLinuxARM-m2"};
  std::string defaultRootfsDesktop {"ArchLinuxARM-vk"};
  // binfmt_misc registration name, and the pinned-build directory under ~/.local/opt.
  std::string binfmtName {"POWERarm-aarch64"};
  std::string stableOptDir {"powerarm-stable"};
};

const BackendSpec& GetActiveBackend();
bool SetActiveBackend(const std::string& id);

} // namespace Sleeve::Backend
