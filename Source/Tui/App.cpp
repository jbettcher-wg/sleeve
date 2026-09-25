// SPDX-License-Identifier: MIT
#include "App.h"
#include "Actions.h"
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
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace Sleeve::Tui {

using namespace ftxui;

Component CreateMainScreen(AppState* state, ScreenInteractive* screen);
Component CreateScanScreen(AppState* state, ScreenInteractive* screen);
Component CreateOptionsScreen(AppState* state, ScreenInteractive* screen);
Component CreatePreviewModal(AppState* state, ScreenInteractive* screen);
Component CreateHealthScreen(AppState* state, ScreenInteractive* screen);
Component CreateLibsScreen(AppState* state, ScreenInteractive* screen);

void AppState::Refresh() {
  records = Record::ListRecords();
  if (selected_app_index >= static_cast<int>(records.size())) {
    selected_app_index = records.empty() ? 0 : static_cast<int>(records.size()) - 1;
  }
  stable_version = Paths::GetStableVersion();
  binfmt_status = Paths::GetBinfmtStatus();
  rootfs_result = RootFS::DiscoverRootFSes();
}

namespace {

// Repaint interval while a job runs. Fast enough that the elapsed counter and the
// spinner read as motion rather than as a stuck screen.
constexpr int kTickMs = 100;

// How long to let an abandoned worker come back on the way out. Short, because a health
// check can sit in Core for two minutes and quitting must not wait for it; if the budget
// runs out the process leaves without running static destructors instead (see below).
constexpr int kShutdownBudgetMs = 750;

std::string OneDecimal(double v) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << v;
  return ss.str();
}

// The panel that appears the instant a shortcut is pressed. It has to answer two
// questions without being asked: what is happening, and is it still alive.
Element WorkingPanel(const AppState& state) {
  static const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
  double elapsed = state.job.ElapsedSeconds();
  const char* frame = kFrames[static_cast<int>(elapsed * 10.0) % 10];

  std::string progress = state.job.Progress();

  Elements rows;
  rows.push_back(hbox({
      text(std::string(frame) + " ") | bold | color(Color::Palette16(3)),
      text(state.job.Label()) | bold,
  }));
  if (!progress.empty()) {
    rows.push_back(hbox({text("  "), paragraph(progress) | color(Color::Palette16(8))}));
  }
  rows.push_back(hbox({
      text("  "),
      text(OneDecimal(elapsed) + " s elapsed") | color(Color::Palette16(8)),
  }));
  rows.push_back(separator());
  rows.push_back(text(" " + state.job.CancelHint()) |
                 color(state.job.Cancellable() ? Color::Palette16(6) : Color::Palette16(8)));

  return vbox(std::move(rows)) | size(WIDTH, LESS_THAN, 78) | borderRounded |
         bgcolor(Color::Black) | clear_under;
}

// Whatever is about to happen, it says so and names the command before it does it.
Element ConfirmPanel(const AppState& state) {
  const auto& pending = state.pending_confirm;
  return vbox({
             hbox({text(" ") , text(pending.title) | bold | color(Color::Palette16(3))}),
             separator(),
             text("About to run:") | color(Color::Palette16(8)),
             hbox({text("  "), paragraph(pending.command) | bold}),
             text(""),
             paragraph(pending.warning) | color(Color::Palette16(3)),
             separator(),
             hbox({
                 text(" [enter]") | bold | color(Color::Palette16(6)),
                 text(" do it   "),
                 text("[esc]") | bold | color(Color::Palette16(6)),
                 text(" don't — nothing has happened yet "),
             }),
         }) |
         size(WIDTH, LESS_THAN, 78) | borderRounded | bgcolor(Color::Black) | clear_under;
}

} // namespace

int RunTui(const std::string& explicitTheme) {
  auto screen = ScreenInteractive::Fullscreen();
  AppState state;
  state.theme_override = explicitTheme;
  state.palette = Theme::ResolvePalette(explicitTheme);
  state.Refresh();

  // Workers wake the loop through this. Job clears its copy whenever the UI stops
  // listening, so a worker that outlives the screen cannot post to it.
  state.job.SetWake([&screen]() { screen.PostEvent(Event::Custom); });

  auto main_screen = CreateMainScreen(&state, &screen);
  auto scan_screen = CreateScanScreen(&state, &screen);
  auto options_screen = CreateOptionsScreen(&state, &screen);
  auto preview_modal = CreatePreviewModal(&state, &screen);
  auto health_screen = CreateHealthScreen(&state, &screen);
  auto libs_screen = CreateLibsScreen(&state, &screen);

  int tab_index = 0;
  auto tab_container = Container::Tab({
    main_screen,
    scan_screen,
    options_screen,
    preview_modal,
    health_screen,
    libs_screen,
  }, &tab_index);

  auto root = Renderer(tab_container, [&]() {
    // The ticker thread cannot read job.Running(): that is UI-thread state. This runs on
    // the UI thread after every event, including the ones that start a job from a button
    // inside a screen, so it is the one place that sees every transition.
    state.ui_busy.store(state.job.Running(), std::memory_order_relaxed);

    Element body = tab_container->Render();
    if (!state.status_line.empty()) {
      body = vbox({
        body | flex,
        hbox({text(" "), text(state.status_line) | color(Color::Palette16(8))}),
      });
    }
    if (state.pending_confirm.active) {
      return dbox({body, ConfirmPanel(state) | center});
    }
    if (state.job.Running()) {
      return dbox({body, WorkingPanel(state) | center});
    }
    return body;
  });

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

  // Keeps the elapsed counter and the spinner moving between a worker's own wakeups.
  // It waits on a condition variable rather than sleeping so that quitting does not have
  // to sit through a tick first.
  std::mutex tickerMutex;
  std::condition_variable tickerCv;
  bool stopTicker = false;
  std::thread tickerThread([&]() {
    std::unique_lock<std::mutex> lock(tickerMutex);
    while (!stopTicker) {
      tickerCv.wait_for(lock, std::chrono::milliseconds(kTickMs), [&] { return stopTicker; });
      if (stopTicker) {
        break;
      }
      if (state.ui_busy.load(std::memory_order_relaxed)) {
        screen.PostEvent(Event::Custom);
      }
    }
  });

  auto global_handler = CatchEvent(root, [&](Event event) {
    // Picks up a finished worker and copies its result in, on this thread. Everything a
    // worker produced enters AppState here and nowhere else.
    state.job.Collect();

    if (themeNeedsUpdate.exchange(false)) {
      // The theme changed under us. Reloading it reads a file and shells out to
      // omarchy-theme-color, so it goes through a job like everything else -- and the
      // event that happened to arrive at the same moment is not swallowed any more.
      StartThemeReload(state);
    }

    // The confirmation owns the keyboard while it is up.
    if (state.pending_confirm.active) {
      if (event == Event::Return || event == Event::Character('y')) {
        auto accept = state.pending_confirm.on_accept;
        state.pending_confirm = AppState::PendingConfirm{};
        if (accept) accept();
      } else if (event == Event::Escape || event == Event::Character('n') ||
                 event == Event::Character('q')) {
        std::string what = state.pending_confirm.title;
        state.pending_confirm = AppState::PendingConfirm{};
        state.status_line = what + " declined — nothing was run";
      }
      return true;
    }

    if (state.job.Running()) {
      if (event == Event::Escape) {
        if (state.job.Cancellable()) {
          std::string label = state.job.Label();
          state.job.Cancel();
          state.status_line = label + " — cancelled";
        }
        return true;
      }
      if (event == Event::Character('q')) {
        screen.ExitLoopClosure()();
        return true;
      }
      // The keys that would start a second long action. Job::Start refuses them anyway;
      // swallowing them here keeps them from reaching a screen that would act on them,
      // and says why instead of doing nothing visible.
      if (event == Event::Character('s') || event == Event::Character('t') ||
          event == Event::Character('h') || event == Event::Character('w') ||
          event == Event::Character('l')) {
        state.status_line = state.job.Label() + " is still running";
        return true;
      }
      // Everything else -- arrows, tab, typing -- still reaches the screen underneath,
      // because the loop is not blocked any more.
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
        StartScan(state);
        return true;
      }
      if (event == Event::Character('t')) {
        StartThemeReload(state);
        return true;
      }
      if (event == Event::Character('h')) {
        if (!PrepareHealthCheck(state)) {
          state.status_line = "nothing selected to check";
        }
        return true;
      }
      if (event == Event::Character('l')) {
        if (!StartLibsScan(state)) {
          state.status_line = "nothing selected to check for libraries";
        }
        return true;
      }
      if (event == Event::Character('w')) {
        if (state.selected_app_index >= 0 &&
            state.selected_app_index < static_cast<int>(state.records.size())) {
          state.editing_record = state.records[state.selected_app_index];
          StartPreviewBuild(state, /*saveFirst=*/false);
        } else {
          state.status_line = "nothing selected to write";
        }
        return true;
      }
    }

    switch (state.current_tab) {
      case ScreenTab::Main:    tab_index = 0; break;
      case ScreenTab::Scan:    tab_index = 1; break;
      case ScreenTab::Options: tab_index = 2; break;
      case ScreenTab::Preview: tab_index = 3; break;
      case ScreenTab::Health:  tab_index = 4; break;
      case ScreenTab::Libs:    tab_index = 5; break;
      default:                 tab_index = 0; break;
    }

    return false;
  });

  screen.Loop(global_handler);

  {
    std::lock_guard<std::mutex> lock(tickerMutex);
    stopTicker = true;
  }
  tickerCv.notify_all();
  tickerThread.join();

  stopWatcher.store(true);
  if (watcherThread.joinable()) {
    watcherThread.join();
  }

  // Quitting mid-scan is allowed; quitting into a use-after-free is not. A worker that
  // is still inside Scanner::RunScan or Health::RunCheck is reading Backend's namespace
  // -scope strings, and returning from here would start destroying them under it. There
  // is no way to join it -- that is the freeze this whole change removes -- so give it a
  // short chance to come back and otherwise leave without running static destructors at
  // all. ftxui has already put the terminal back by the time Loop() returns, and there
  // is nothing after RunTui but `return` in main.
  if (!state.job.Shutdown(std::chrono::milliseconds(kShutdownBudgetMs))) {
    std::fflush(nullptr);
    std::_Exit(0);
  }

  return 0;
}

} // namespace Sleeve::Tui
