// SPDX-License-Identifier: MIT
#include "App.h"
#include "Paths.h"
#include "Generate.h"
#include "FileWriter.h"
#include "Diff.h"
#include "Health.h"
#include "AppConfigWriter.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <thread>
#include <atomic>

namespace Sleeve::Tui {

using namespace ftxui;

Component CreateMainScreen(AppState* state, ScreenInteractive* screen);
Component CreateScanScreen(AppState* state, ScreenInteractive* screen);
Component CreateOptionsScreen(AppState* state, ScreenInteractive* screen);
Component CreatePreviewModal(AppState* state, ScreenInteractive* screen);
Component CreateHealthScreen(AppState* state, ScreenInteractive* screen);

void AppState::Refresh() {
  records = Record::ListRecords();
  if (selected_app_index >= static_cast<int>(records.size())) {
    selected_app_index = records.empty() ? 0 : static_cast<int>(records.size()) - 1;
  }
  stable_version = Paths::GetStableVersion();
  binfmt_status = Paths::GetBinfmtStatus();
  rootfs_result = RootFS::DiscoverRootFSes();
}

int RunTui(const std::string& explicitTheme) {
  auto screen = ScreenInteractive::Fullscreen();
  AppState state;
  state.palette = Theme::ResolvePalette(explicitTheme);
  state.Refresh();

  auto main_screen = CreateMainScreen(&state, &screen);
  auto scan_screen = CreateScanScreen(&state, &screen);
  auto options_screen = CreateOptionsScreen(&state, &screen);
  auto preview_modal = CreatePreviewModal(&state, &screen);
  auto health_screen = CreateHealthScreen(&state, &screen);

  int tab_index = 0;
  auto tab_container = Container::Tab({
    main_screen,
    scan_screen,
    options_screen,
    preview_modal,
    health_screen,
  }, &tab_index);

  std::atomic<bool> stopWatcher{false};
  std::atomic<bool> themeNeedsUpdate{false};
  int inotifyFd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  std::thread watcherThread;
  if (inotifyFd >= 0) {
    const char* home = std::getenv("HOME");
    std::string watchDir = (home ? std::string(home) : "") + "/.local/state/omarchy/current";
    int wd = inotify_add_watch(inotifyFd, watchDir.c_str(), IN_MOVED_TO | IN_CREATE | IN_CLOSE_WRITE);
    if (wd >= 0) {
      watcherThread = std::thread([inotifyFd, &stopWatcher, &themeNeedsUpdate, &screen]() {
        pollfd pfd;
        pfd.fd = inotifyFd;
        pfd.events = POLLIN;
        while (!stopWatcher.load()) {
          int ret = poll(&pfd, 1, 500);
          if (ret > 0 && (pfd.revents & POLLIN)) {
            char buf[1024];
            while (read(inotifyFd, buf, sizeof(buf)) > 0) {}
            themeNeedsUpdate.store(true);
            screen.PostEvent(Event::Custom);
          }
        }
        close(inotifyFd);
      });
    } else {
      close(inotifyFd);
    }
  }

  auto global_handler = CatchEvent(tab_container, [&](Event event) {
    if (themeNeedsUpdate.exchange(false)) {
      state.palette = Theme::ResolvePalette(explicitTheme);
      state.Refresh();
      return true;
    }

    if (event == Event::Character('q') || event == Event::Escape) {
      if (state.current_tab != ScreenTab::Main) {
        state.current_tab = ScreenTab::Main;
        tab_index = 0;
        return true;
      }
      screen.ExitLoopClosure()();
      return true;
    }

    if (state.current_tab == ScreenTab::Main) {
      if (event == Event::Character('s')) {
        Scanner::ScanOptions opt;
        state.scan_result = Scanner::RunScan(opt);
        state.scan_selected.assign(state.scan_result.apps.size(), {true});
        state.current_tab = ScreenTab::Scan;
        tab_index = 1;
        return true;
      }
      if (event == Event::Character('t')) {
        state.palette = Theme::ResolvePalette(explicitTheme);
        return true;
      }
      if (event == Event::Character('h')) {
        if (state.selected_app_index >= 0 && state.selected_app_index < static_cast<int>(state.records.size())) {
          auto& rec = state.records[state.selected_app_index];
          Health::CheckOptions hopt;
          hopt.mode = Health::CheckMode::Version;
          hopt.timeout_seconds = 20.0;
          hopt.wmclass = rec.desktop.wmclass;
          state.current_health_result = Health::RunCheck(rec, hopt);
          state.current_tab = ScreenTab::Health;
          tab_index = 4;
          state.Refresh();
          return true;
        }
      }
      if (event == Event::Character('w')) {
        if (state.selected_app_index >= 0 && state.selected_app_index < static_cast<int>(state.records.size())) {
          const auto& rec = state.records[state.selected_app_index];
          auto gen = Generate::GenerateFiles(rec);
          state.files_to_write.clear();
          state.preview_diffs.clear();
          state.editing_record = rec;

          // 1. Launcher
          auto lStatus = FileWriter::CheckStatus(gen.launcher_path, rec.generated.count(gen.launcher_path) ? rec.generated.at(gen.launcher_path) : "");
          state.files_to_write.push_back({gen.launcher_path, gen.launcher_content});
          state.preview_diffs.push_back(Diff::UnifiedDiff(lStatus.existing_content, gen.launcher_content,
                                                          gen.launcher_path + " (current)", gen.launcher_path + " (new)"));

          // 2. Desktop entry
          if (gen.has_desktop) {
            auto dStatus = FileWriter::CheckStatus(gen.desktop_path, rec.generated.count(gen.desktop_path) ? rec.generated.at(gen.desktop_path) : "");
            state.files_to_write.push_back({gen.desktop_path, gen.desktop_content});
            state.preview_diffs.push_back(Diff::UnifiedDiff(dStatus.existing_content, gen.desktop_content,
                                                            gen.desktop_path + " (current)", gen.desktop_path + " (new)"));
          }

          // 3. AppConfig
          if (gen.has_appconfig) {
            auto aStatus = FileWriter::CheckStatus(gen.appconfig_path, rec.generated.count(gen.appconfig_path) ? rec.generated.at(gen.appconfig_path) : "");
            state.files_to_write.push_back({gen.appconfig_path, gen.appconfig_content});
            state.preview_diffs.push_back(Diff::UnifiedDiff(aStatus.existing_content, gen.appconfig_content,
                                                            gen.appconfig_path + " (current)", gen.appconfig_path + " (new)"));
          }

          state.current_tab = ScreenTab::Preview;
          tab_index = 3;
          return true;
        }
      }
    }

    switch (state.current_tab) {
      case ScreenTab::Main:    tab_index = 0; break;
      case ScreenTab::Scan:    tab_index = 1; break;
      case ScreenTab::Options: tab_index = 2; break;
      case ScreenTab::Preview: tab_index = 3; break;
      case ScreenTab::Health:  tab_index = 4; break;
      default:                 tab_index = 0; break;
    }

    return false;
  });

  screen.Loop(global_handler);

  stopWatcher.store(true);
  if (watcherThread.joinable()) {
    watcherThread.join();
  }

  return 0;
}

} // namespace Sleeve::Tui
