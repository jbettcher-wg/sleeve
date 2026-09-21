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

} // namespace Sleeve::Process
