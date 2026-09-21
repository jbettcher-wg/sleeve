// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "RootFS.h"

using namespace Sleeve::RootFS;

TEST_CASE("RootFS: FormatBytes formatting", "[sleeve][rootfs]") {
  CHECK(FormatBytes(500) == "500 B");
  CHECK(FormatBytes(2048) == "2 KB");
  CHECK(FormatBytes(50 * 1024 * 1024) == "50 MB");
  CHECK(FormatBytes(1536ULL * 1024ULL * 1024ULL) == "1.5 GB");
}
