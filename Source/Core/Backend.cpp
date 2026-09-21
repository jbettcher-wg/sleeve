// SPDX-License-Identifier: MIT
#include "Backend.h"

namespace Sleeve::Backend {

static BackendSpec g_powerarm = {
  .id = "powerarm",
  .displayName = "POWERarm",
  .emulatorBin = "POWERarm",
  .envPrefix = "POWERARM_",
  .configSubdir = "powerarm",
  .dataSubdir = "powerarm",
  .targetArch = TargetArch::AArch64,
  .elfMachine = 183, // EM_AARCH64
  .archName = "arm64",
  .defaultRootfsBase = "ArchLinuxARM-m2",
  .defaultRootfsDesktop = "ArchLinuxARM-vk",
};

static BackendSpec g_fastppcx86 = {
  .id = "fastppcx86",
  .displayName = "fastppcx86",
  .emulatorBin = "FEX",
  .envPrefix = "FEX_",
  .configSubdir = "fex-emu",
  .dataSubdir = "fex-emu",
  .targetArch = TargetArch::X86_64,
  .elfMachine = 62, // EM_X86_64
  .archName = "x86_64",
  .defaultRootfsBase = "Ubuntu-24.04",
  .defaultRootfsDesktop = "Ubuntu-24.04",
};

static BackendSpec* g_activeBackend = &g_powerarm;

const BackendSpec& GetActiveBackend() {
  return *g_activeBackend;
}

bool SetActiveBackend(const std::string& id) {
  if (id == "powerarm") {
    g_activeBackend = &g_powerarm;
    return true;
  }
  if (id == "fastppcx86" || id == "fex") {
    g_activeBackend = &g_fastppcx86;
    return true;
  }
  return false;
}

} // namespace Sleeve::Backend
