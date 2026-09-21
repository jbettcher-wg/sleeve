// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>
#include "Record.h"

namespace Sleeve::Health {

enum class CheckMode {
  Version,
  Timed,
};

struct CheckOptions {
  CheckMode mode {CheckMode::Version};
  double timeout_seconds {20.0};
  std::string wmclass;
};

struct CheckResult {
  std::string status {"ok"}; // "ok", "error", "sigill", "sigsegv", "sigabrt", "timeout", "missing_library", "host_fault", "unsupported_elf", "server_socket_error"
  std::string summary;
  std::vector<std::string> notes;
  std::vector<std::string> unimplemented_words;
  int exit_code {0};
  double elapsed_seconds {0.0};
  bool window_seen {false};
  std::string log_path;
  std::string raw_stdout;
  std::string raw_stderr;
};

// Pure classifier over run outputs (unit testable without executing external commands)
CheckResult ClassifyRun(int exit_code, bool timed_out, double elapsed_seconds,
                        const std::string& stdout_str, const std::string& stderr_str,
                        bool window_seen = false, const std::string& wmclass = "");

// Execute health check on an app record, log output, update record.health, and save
CheckResult RunCheck(Record::AppRecord& record, const CheckOptions& options);

} // namespace Sleeve::Health
