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

// Fills state.pending_launch with the exact command line that a health check would run.
// Nothing is started; `h` shows this first because the check launches the application.
bool PrepareHealthCheck(AppState& state);

// Runs the check that PrepareHealthCheck described, and clears the pending launch.
bool StartHealthCheck(AppState& state);

// The command `Health::RunCheck` will actually execute for this record, written out the
// way a shell would show it.
std::string DescribeHealthCommand(const Record::AppRecord& record);

} // namespace Sleeve::Tui
