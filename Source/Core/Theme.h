// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <cstdint>
#include <optional>

namespace Sleeve::Theme {

struct RGBColor {
  uint8_t r {0};
  uint8_t g {0};
  uint8_t b {0};
};

struct Palette {
  std::string theme_name {"terminal"};
  std::string source_description;

  // Truecolor overrides (if available from Omarchy)
  std::optional<RGBColor> accent;
  std::optional<RGBColor> selection_bg;
  std::optional<RGBColor> selection_fg;
};

// Resolve theme palette from Omarchy or explicit theme file
Palette ResolvePalette(const std::string& explicitThemePath = "");

std::string ColorToHex(const RGBColor& c);
std::optional<RGBColor> ParseHex(const std::string& hex);

} // namespace Sleeve::Theme
