// SPDX-License-Identifier: MIT
#include "Health.h"
#include "Paths.h"
#include "Process.h"
#include "FileWriter.h"
#include "Backend.h"

#include <sstream>
#include <fstream>
#include <regex>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <set>

namespace Sleeve::Health {

namespace fs = std::filesystem;

static std::string GetCurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%H:%M:%S");
  return ss.str();
}

static std::string ExtractFirstStdoutLine(const std::string& stdout_str) {
  std::istringstream iss(stdout_str);
  std::string line;
  while (std::getline(iss, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) {
      return line;
    }
  }
  return "";
}

CheckResult ClassifyRun(int exit_code, bool timed_out, double elapsed_seconds,
                        const std::string& stdout_str, const std::string& stderr_str,
                        bool window_seen, const std::string& wmclass) {
  CheckResult res;
  res.exit_code = exit_code;
  res.elapsed_seconds = elapsed_seconds;
  res.window_seen = window_seen;
  res.raw_stdout = stdout_str;
  res.raw_stderr = stderr_str;

  // Search for unimplemented instructions in both stderr and stdout
  std::set<std::string> uniqueWords;
  std::regex unimplemRegex(R"(Unimplemented A64 instruction (0x[0-9a-fA-F]+))");
  auto words_begin = std::sregex_iterator(stderr_str.begin(), stderr_str.end(), unimplemRegex);
  auto words_end = std::sregex_iterator();
  for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
    std::smatch match = *i;
    uniqueWords.insert(match[1].str());
  }
  for (const auto& w : uniqueWords) {
    res.unimplemented_words.push_back(w);
  }

  std::string combined = stderr_str + "\n" + stdout_str;

  // 1. Not a supported ELF
  if (combined.find("is not a supported ELF") != std::string::npos) {
    res.status = "unsupported_elf";
    res.summary = "This is not an AArch64 program, or the rootfs did not resolve and a host binary was found instead. If the launcher uses a build path, the rootfs must be an absolute path.";
  }
  // 2. Fatal host fault
  else if (combined.find("FATAL host fault:") != std::string::npos) {
    res.status = "host_fault";
    std::regex faultRegex(R"(FATAL host fault: ([^\n\r]+))");
    std::smatch match;
    if (std::regex_search(combined, match, faultRegex)) {
      res.summary = "The emulator itself crashed (" + match[1].str() + "). This is an emulator bug; keep this line for the report.";
    } else {
      res.summary = "The emulator itself crashed with a fatal host fault. This is an emulator bug; keep this line for the report.";
    }
  }
  // 3. Missing library
  else if (combined.find("error while loading shared libraries:") != std::string::npos) {
    res.status = "missing_library";
    std::regex libRegex(R"(error while loading shared libraries:\s*([^:]+):)");
    std::smatch match;
    if (std::regex_search(combined, match, libRegex)) {
      res.summary = "The program needs the library " + match[1].str() + " and the rootfs does not have it.";
    } else {
      res.summary = "The program needs a shared library and the rootfs does not have it.";
    }
  }
  // 4. Server socket error
  else if (combined.find("Couldn't connect to POWERarmServer socket") != std::string::npos ||
           combined.find("Couldn't connect to FEXServer socket") != std::string::npos) {
    res.status = "server_socket_error";
    res.summary = "The emulator server could not start; with a user namespace or a build path, set " +
                  Backend::GetActiveBackend().envPrefix + "PORTABLE=1 (README line 274-275).";
  }
  // 5. Signals
  else if (exit_code == 132) {
    res.status = "sigill";
    if (!res.unimplemented_words.empty()) {
      std::string wordsStr;
      for (size_t i = 0; i < res.unimplemented_words.size(); ++i) {
        wordsStr += (i > 0 ? ", " : "") + res.unimplemented_words[i];
      }
      res.summary = "Died with SIGILL executing unimplemented instruction(s): " + wordsStr;
    } else {
      res.summary = "Died with SIGILL";
    }
  }
  else if (exit_code == 139) {
    res.status = "sigsegv";
    res.summary = "Died with SIGSEGV";
  }
  else if (exit_code == 134) {
    res.status = "sigabrt";
    res.summary = "Died with SIGABRT";
  }
  // 6. Timed out
  else if (timed_out) {
    if (window_seen) {
      res.status = "ok";
      std::ostringstream ss;
      ss << "Started and showed a window (class " << (wmclass.empty() ? "unknown" : wmclass)
         << "); stopped after " << std::fixed << std::setprecision(1) << elapsed_seconds << " s.";
      res.summary = ss.str();
    } else {
      res.status = "timeout";
      std::ostringstream ss;
      ss << "Ran for " << std::fixed << std::setprecision(1) << elapsed_seconds
         << " s without a window; not necessarily wrong for a CLI program.";
      res.summary = ss.str();
    }
  }
  // 7. Exit 0
  else if (exit_code == 0) {
    res.status = "ok";
    std::string firstLine = ExtractFirstStdoutLine(stdout_str);
    if (!firstLine.empty()) {
      res.summary = "Ran and exited normally; printed \"" + firstLine + "\"";
    } else {
      res.summary = "Ran and exited normally";
    }
  }
  // 8. Other exit code
  else {
    res.status = "error";
    res.summary = "Exited with code " + std::to_string(exit_code);
  }

  // Notes
  if (!res.unimplemented_words.empty()) {
    std::string wordsStr;
    for (size_t i = 0; i < res.unimplemented_words.size(); ++i) {
      wordsStr += (i > 0 ? ", " : "") + res.unimplemented_words[i];
    }
    std::string note = "The translator met " + std::to_string(res.unimplemented_words.size()) +
                       " instruction(s) it does not implement (words " + wordsStr +
                       "). Only a SIGILL exit means one was executed; feature probes are expected (MRS TPIDR2/GCS, MTE, SVE, SME, HANDOVER lines 51-53).";
    res.notes.push_back(note);
  }

  return res;
}

CheckResult RunCheck(Record::AppRecord& record, const CheckOptions& options) {
  const char* home = std::getenv("HOME");
  std::string homeStr = home ? home : "";

  std::string launcherPath = homeStr + "/.local/bin/" + record.name;
  std::vector<std::string> cmd;

  if (fs::exists(launcherPath)) {
    cmd.push_back(launcherPath);
  } else {
    cmd.push_back(record.GetResolvedExePath());
  }

  if (options.mode == CheckMode::Version) {
    cmd.push_back("--version");
  }

  Process::ProcessOptions procOpt;
  procOpt.args = cmd;
  procOpt.timeout_seconds = (options.mode == CheckMode::Timed) ? options.timeout_seconds : 120.0;
  procOpt.env = record.env;
  // The log knobs belong to whichever emulator is active; under fastppcx86 the POWERarm
  // names are ignored and the run produces nothing to classify.
  const auto& backend = Backend::GetActiveBackend();
  procOpt.env[backend.envPrefix + "SILENTLOG"] = "0";
  procOpt.env[backend.envPrefix + "OUTPUTLOG"] = "stderr";

  // Check window presence if timed and Hyprland
  bool windowSeen = false;
  std::string targetWmClass = options.wmclass.empty() ? record.desktop.wmclass : options.wmclass;

  auto runResult = Process::RunCommand(procOpt);

  // If timed run and Hyprland is running, check if window was opened
  if (options.mode == CheckMode::Timed && !targetWmClass.empty() && std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
    auto hyprCheck = Process::RunCommand({"hyprctl", "clients", "-j"}, 2.0);
    if (hyprCheck.exit_code == 0 && hyprCheck.stdout_str.find(targetWmClass) != std::string::npos) {
      windowSeen = true;
    }
  }

  CheckResult res = ClassifyRun(runResult.exit_code, runResult.timed_out, runResult.elapsed_seconds,
                               runResult.stdout_str, runResult.stderr_str, windowSeen, targetWmClass);

  // Save log file
  std::string cacheDir = Paths::GetCacheDir() + "/sleeve/health";
  std::error_code ec;
  fs::create_directories(cacheDir, ec);
  std::string timestamp = GetCurrentTimestamp();
  std::string logName = record.name + "-" + timestamp + ".log";
  std::string logPath = cacheDir + "/" + logName;

  std::ofstream logFile(logPath);
  if (logFile.is_open()) {
    logFile << "Command: ";
    for (const auto& a : cmd) logFile << a << " ";
    logFile << "\nExit Code: " << runResult.exit_code << "\n";
    logFile << "Elapsed: " << runResult.elapsed_seconds << "s\n\n";
    logFile << "--- STDOUT ---\n" << runResult.stdout_str << "\n";
    logFile << "--- STDERR ---\n" << runResult.stderr_str << "\n";
    logFile.close();
    res.log_path = logPath;
  }

  // Update record health
  record.health.when = timestamp;
  record.health.mode = (options.mode == CheckMode::Version ? "--version" : "timed");
  record.health.status = res.status;
  record.health.unimplemented = res.unimplemented_words;
  record.health.notes.clear();
  record.health.notes.push_back(res.summary);
  for (const auto& n : res.notes) {
    record.health.notes.push_back(n);
  }

  Record::SaveRecord(record);

  return res;
}

} // namespace Sleeve::Health
