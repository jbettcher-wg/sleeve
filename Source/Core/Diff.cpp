// SPDX-License-Identifier: MIT
#include "Diff.h"

#include <sstream>
#include <vector>
#include <algorithm>

namespace Sleeve::Diff {

static std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

std::vector<DiffLine> ComputeDiffLines(const std::string& oldText, const std::string& newText) {
  auto a = SplitLines(oldText);
  auto b = SplitLines(newText);

  size_t n = a.size();
  size_t m = b.size();

  // Standard LCS table
  std::vector<std::vector<size_t>> dp(n + 1, std::vector<size_t>(m + 1, 0));
  for (size_t i = 1; i <= n; ++i) {
    for (size_t j = 1; j <= m; ++j) {
      if (a[i - 1] == b[j - 1]) {
        dp[i][j] = dp[i - 1][j - 1] + 1;
      } else {
        dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
      }
    }
  }

  // Backtrack
  std::vector<DiffLine> res;
  size_t i = n, j = m;
  while (i > 0 || j > 0) {
    if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
      res.push_back({DiffLine::Type::Context, a[i - 1]});
      --i;
      --j;
    } else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) {
      res.push_back({DiffLine::Type::Addition, b[j - 1]});
      --j;
    } else if (i > 0 && (j == 0 || dp[i][j - 1] < dp[i - 1][j])) {
      res.push_back({DiffLine::Type::Deletion, a[i - 1]});
      --i;
    }
  }

  std::reverse(res.begin(), res.end());
  return res;
}

std::string UnifiedDiff(const std::string& oldText, const std::string& newText,
                        const std::string& oldLabel, const std::string& newLabel) {
  std::ostringstream ss;
  ss << "--- " << oldLabel << "\n";
  ss << "+++ " << newLabel << "\n";

  auto lines = ComputeDiffLines(oldText, newText);
  for (const auto& l : lines) {
    switch (l.type) {
      case DiffLine::Type::Context:
        ss << " " << l.text << "\n";
        break;
      case DiffLine::Type::Addition:
        ss << "+" << l.text << "\n";
        break;
      case DiffLine::Type::Deletion:
        ss << "-" << l.text << "\n";
        break;
    }
  }
  return ss.str();
}

} // namespace Sleeve::Diff
