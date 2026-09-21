// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "Diff.h"

using namespace Sleeve::Diff;

TEST_CASE("Diff: UnifiedDiff computation", "[sleeve][diff]") {
  std::string oldText = "line 1\nline 2\nline 3\n";
  std::string newText = "line 1\nline 2 modified\nline 3\nline 4\n";

  std::string diff = UnifiedDiff(oldText, newText);

  CHECK(diff.find("--- current") != std::string::npos);
  CHECK(diff.find("+++ new") != std::string::npos);
  CHECK(diff.find("-line 2") != std::string::npos);
  CHECK(diff.find("+line 2 modified") != std::string::npos);
  CHECK(diff.find("+line 4") != std::string::npos);
}
