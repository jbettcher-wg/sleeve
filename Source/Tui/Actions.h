// SPDX-License-Identifier: MIT
#pragma once

#include "Record.h"

#include <string>

namespace Sleeve::Tui {

struct AppState;

// Every one of these starts a background job and returns immediately, leaving the UI
// thread free to repaint. They return false when a job is already running, which is what
// a second press of the same key runs into.
//
// None of the work functions below touch AppState. Each allocates a shared result block,
// fills it on the worker thread, and lets Job::Collect copy it into AppState on the UI
// thread. See Job.h for the contract.

bool StartScan(AppState& state);
bool StartThemeReload(AppState& state);
bool StartAddSelected(AppState& state);
bool StartPreviewBuild(AppState& state, bool saveFirst);
bool StartWriteFiles(AppState& state);

// Fills state.pending_confirm with the exact command line that a health check would run.
// Nothing is started; `h` shows this first because the check launches the application.
bool PrepareHealthCheck(AppState& state);

// Runs the check that PrepareHealthCheck described.
bool StartHealthCheck(AppState& state);

// The static library pass: reads the app's ELF objects and everything they need, and
// resolves each soname against the overlay and the base. Read-only, and it launches
// nothing.
bool StartLibsScan(AppState& state);

// Asks guest pacman which packages own the unmet sonames. Read-only against the rootfs,
// but it starts the emulator, so it is its own job rather than part of the scan.
bool StartLibsPackages(AppState& state);

// Fills state.pending_confirm with the guest pacman install. Installing into the overlay
// changes what every guest using that rootfs sees, so nothing runs until it is accepted.
bool PrepareLibsInstall(AppState& state);

// Fills state.pending_confirm with the `pacman -Fy` that downloads the guest's file
// database into the overlay.
bool PrepareLibsSync(AppState& state);

// Fills state.pending_confirm with the POWERarmRootFSFetcher build that provisions a
// guest, for the two readiness states worth acting on automatically: no rootfs at all,
// and a rootfs of the wrong architecture.
bool PrepareRootFSBuild(AppState& state);

// The command `Health::RunCheck` will actually execute for this record, written out the
// way a shell would show it.
std::string DescribeHealthCommand(const Record::AppRecord& record);

} // namespace Sleeve::Tui
