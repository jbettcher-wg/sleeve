// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "Theme.h"

using namespace Sleeve::Theme;

TEST_CASE("Theme: Hex color conversion", "[sleeve][theme]") {
  RGBColor c {0xFF, 0xC6, 0x00}; // cobalt2 accent
  CHECK(ColorToHex(c) == "#ffc600");

  auto parsed = ParseHex("#ffc600");
  REQUIRE(parsed.has_value());
  CHECK(parsed->r == 0xFF);
  CHECK(parsed->g == 0xC6);
  CHECK(parsed->b == 0x00);

  auto parsedNoHash = ParseHex("3ad900");
  REQUIRE(parsedNoHash.has_value());
  CHECK(parsedNoHash->r == 0x3A);
  CHECK(parsedNoHash->g == 0xD9);
  CHECK(parsedNoHash->b == 0x00);
}
