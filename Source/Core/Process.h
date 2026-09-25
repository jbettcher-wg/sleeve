// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>

namespace Sleeve::Process {

struct ProcessResult {
  int exit_code {-1};
  std::string stdout_str;
  std::string stderr_str;
  bool timed_out {false};
  double elapsed_seconds {0.0};
};

struct ProcessOptions {
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::string cwd;
  double timeout_seconds {0.0}; // 0.0 means no timeout
  std::function<void(const std::string&)> on_stdout;
  std::function<void(const std::string&)> on_stderr;
};

ProcessResult RunCommand(const ProcessOptions& options);
ProcessResult RunCommand(const std::vector<std::string>& args, double timeout_seconds = 0.0);

// Runs a command. Injected wherever the thing being run is a guest emulator or a
// provisioning tool, so the command that would be run is a test rather than something
// to be found out by running it.
using Runner = std::function<ProcessResult(const ProcessOptions&)>;

// `runner` when one was given, Process::RunCommand otherwise.
ProcessResult Execute(const Runner& runner, const ProcessOptions& options);

// One long action's result, written for a person: what ran, what it said, what went wrong.
struct RunOutcome {
  bool ok {false};
  std::string command;
  std::string output;
  std::string error;
};

// The command written out the way a shell would show it, environment assignments first.
// This is what gets shown before anything is run, so it has to be the thing that runs.
std::string Describe(const ProcessOptions& options);

} // namespace Sleeve::Process
