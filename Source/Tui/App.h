// SPDX-License-Identifier: MIT
#pragma once

#include "Record.h"
#include "Scanner.h"
#include "RootFS.h"
#include "Theme.h"
#include "Health.h"
#include "Libs.h"
#include "Job.h"

#include <atomic>
#include <functional>
#include <vector>
#include <string>
#include <memory>

namespace Sleeve::Tui {

enum class ScreenTab {
  Main,
  Scan,
  Options,
  RootFS,
  Preview,
  Health,
  Libs,
};

struct AppState {
  ScreenTab current_tab {ScreenTab::Main};
  std::vector<Record::AppRecord> records;
  int selected_app_index {0};

  // Scanner state
  Scanner::ScanResult scan_result;
  struct BoolItem { bool selected {true}; };
  std::vector<BoolItem> scan_selected;
  int selected_scan_index {0};

  // Rootfs state
  RootFS::DiscoveryResult rootfs_result;
  int selected_rootfs_index {0};

  // Preview state
  std::vector<std::pair<std::string, std::string>> files_to_write; // path, content
  std::vector<std::string> preview_diffs;
  int preview_file_index {0};

  // Theme & Status
  Theme::Palette palette;
  std::string stable_version;
  std::string binfmt_status;

  // Options edit buffer
  Record::AppRecord editing_record;
  std::string edit_args_str;
  std::string edit_env_str;
  bool edit_code_cache {true};
  int edit_cache_scope_index {2}; // 0: off, 1: rootfs, 2: home, 3: all
  bool edit_cmp_fusion {true};
  bool edit_profile_stats {false};
  std::string edit_emulator_path;

  // Health state
  Health::CheckResult current_health_result;

  // Library state: what the static pass found, what guest pacman says owns it, and
  // whether there is a usable guest to resolve against in the first place.
  Libs::ScanResult libs_result;
  Libs::PackagePlan libs_plan;
  RootFS::ReadinessReport libs_readiness;
  bool libs_scanned {false};

  // --- Background work ---
  //
  // One job at a time. The actions on offer here are mutually exclusive as far as the
  // person is concerned -- a scan, a health check, a write -- and a single slot is what
  // makes a second press of a key while its action runs a no-op rather than a race.
  Job job;

  // Read by the repaint ticker thread, which is why it is an atomic and not job.Running().
  std::atomic<bool> ui_busy {false};

  // One line about whatever finished last: the result, the cancellation or the failure.
  std::string status_line;

  // Whatever RunTui was told to use, kept so a theme reload can ask for it again.
  std::string theme_override;

  // Set by the scan screen. The checkbox list used to be built once, when the result was
  // still empty, and never rebuilt -- so the screen read "Empty container" after every
  // scan. Delivering a scan result calls this, on the UI thread, to rebuild it.
  std::function<void()> on_scan_result_changed;

  // Anything that starts a program, downloads a gigabyte or writes into a guest every
  // other session shares says so first, names the exact command, and does nothing until
  // it is accepted. One panel, because it is one question.
  struct PendingConfirm {
    bool active {false};
    std::string title;
    std::string command;
    std::string warning;
    // Runs on the UI thread when the person says yes; it starts the job.
    std::function<void()> on_accept;
  };
  PendingConfirm pending_confirm;

  void Refresh();
};

int RunTui(const std::string& explicitTheme = "");

} // namespace Sleeve::Tui
