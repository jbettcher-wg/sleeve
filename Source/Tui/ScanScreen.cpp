// SPDX-License-Identifier: MIT
#include "App.h"
#include "Paths.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <iomanip>
#include <sstream>

namespace Sleeve::Tui {

using namespace ftxui;

Component CreateScanScreen(AppState* state, ScreenInteractive* screen) {
  auto container = Container::Vertical({});

  auto btn_add = Button(" Add Selected ", [state, screen]() {
    for (size_t i = 0; i < state->scan_result.apps.size(); ++i) {
      if (i < state->scan_selected.size() && state->scan_selected[i].selected) {
        const auto& cand = state->scan_result.apps[i];
        std::string defRootfs = !state->rootfs_result.rootfses.empty() ? state->rootfs_result.rootfses[0].base_path : "";
        auto rec = Record::CreateFromCandidate(cand, defRootfs);
        Record::SaveRecord(rec);
      }
    }
    state->Refresh();
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  auto btn_rescan = Button(" Rescan ", [state, screen]() {
    Scanner::ScanOptions opt;
    state->scan_result = Scanner::RunScan(opt);
    state->scan_selected.assign(state->scan_result.apps.size(), {true});
    screen->PostEvent(Event::Custom);
  });

  auto btn_back = Button(" Back ", [state, screen]() {
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  auto buttons = Container::Horizontal({btn_add, btn_rescan, btn_back});

  auto checkboxes = Container::Vertical({});
  for (size_t i = 0; i < state->scan_result.apps.size(); ++i) {
    if (i >= state->scan_selected.size()) state->scan_selected.push_back({true});
    const auto& app = state->scan_result.apps[i];
    std::string label = app.name + " (" + app.title + ") - " + Shapes::ShapeToString(app.shape) + " [" + Paths::ContractUser(app.exe_path) + "]";
    checkboxes->Add(Checkbox(label, &state->scan_selected[i].selected));
  }

  container->Add(checkboxes);
  container->Add(buttons);

  return Renderer(container, [state, checkboxes, buttons]() {
    std::ostringstream ss;
    ss << state->scan_result.apps.size() << " apps found · "
       << std::fixed << std::setprecision(1) << state->scan_result.stats.elapsed_seconds << " s · "
       << state->scan_result.stats.files_probed << " files probed";

    return vbox({
      text(" Scan Results ") | bold,
      text(ss.str()) | dim,
      separator(),
      checkboxes->Render() | vscroll_indicator | frame | flex,
      separator(),
      buttons->Render(),
    }) | borderRounded;
  });
}

} // namespace Sleeve::Tui
