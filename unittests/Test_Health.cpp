// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "Health.h"

using namespace Sleeve;

TEST_CASE("Health: Classifier identifies unsupported ELF", "[Health]") {
  std::string stderrStr = "POWERarm: '/opt/apps/test' is not a supported ELF: bad machine 62";
  auto res = Health::ClassifyRun(1, false, 0.2, "", stderrStr);
  REQUIRE(res.status == "unsupported_elf");
  REQUIRE(res.summary.find("not an AArch64 program") != std::string::npos);
}

TEST_CASE("Health: Classifier identifies fatal host fault", "[Health]") {
  std::string stderrStr = "POWERarm: FATAL host fault: signal 11 (si_code 1, addr 0x1000) at host nip 0x2000";
  auto res = Health::ClassifyRun(139, false, 0.5, "", stderrStr);
  REQUIRE(res.status == "host_fault");
  REQUIRE(res.summary.find("emulator itself crashed") != std::string::npos);
}

TEST_CASE("Health: Classifier identifies missing shared library", "[Health]") {
  std::string stderrStr = "/bin/app: error while loading shared libraries: libgtk-3.so.0: cannot open shared object file";
  auto res = Health::ClassifyRun(127, false, 0.1, "", stderrStr);
  REQUIRE(res.status == "missing_library");
  REQUIRE(res.summary.find("libgtk-3.so.0") != std::string::npos);
}

TEST_CASE("Health: Classifier handles unimplemented instructions deduplication", "[Health]") {
  std::string stderrStr = "I Unimplemented A64 instruction 0xd53bd0a0 at pc 0x100\n"
                          "I Unimplemented A64 instruction 0xd53bd0a0 at pc 0x104\n"
                          "I Unimplemented A64 instruction 0xd5384100 at pc 0x200\n";
  std::string stdoutStr = "1.0.0\n";
  auto res = Health::ClassifyRun(0, false, 1.2, stdoutStr, stderrStr);
  REQUIRE(res.status == "ok");
  REQUIRE(res.unimplemented_words.size() == 2);
  REQUIRE(res.notes.size() == 1);
  REQUIRE(res.notes[0].find("The translator met 2 instruction(s)") != std::string::npos);
}

TEST_CASE("Health: Classifier handles SIGILL exit with unimplemented instructions", "[Health]") {
  std::string stderrStr = "I Unimplemented A64 instruction 0xd53bd0a0 at pc 0x100\n";
  auto res = Health::ClassifyRun(132, false, 0.3, "", stderrStr);
  REQUIRE(res.status == "sigill");
  REQUIRE(res.summary.find("Died with SIGILL") != std::string::npos);
  REQUIRE(res.summary.find("0xd53bd0a0") != std::string::npos);
}

TEST_CASE("Health: Classifier handles timed out run with window seen", "[Health]") {
  auto res = Health::ClassifyRun(0, true, 20.0, "", "", true, "Code");
  REQUIRE(res.status == "ok");
  REQUIRE(res.summary.find("showed a window (class Code)") != std::string::npos);
}

TEST_CASE("Health: Classifier handles normal exit 0", "[Health]") {
  auto res = Health::ClassifyRun(0, false, 2.5, "v2.1.19\nextra line\n", "");
  REQUIRE(res.status == "ok");
  REQUIRE(res.summary.find("Ran and exited normally; printed \"v2.1.19\"") != std::string::npos);
}
