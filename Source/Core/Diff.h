// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace Sleeve::Diff {

struct DiffLine {
  enum class Type {
    Context,
    Addition,
    Deletion,
  };
  Type type;
  std::string text;
};

// Generate unified diff string between oldText and newText
std::string UnifiedDiff(const std::string& oldText, const std::string& newText,
                        const std::string& oldLabel = "current", const std::string& newLabel = "new");

std::vector<DiffLine> ComputeDiffLines(const std::string& oldText, const std::string& newText);

} // namespace Sleeve::Diff
