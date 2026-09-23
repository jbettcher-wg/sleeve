// SPDX-License-Identifier: MIT
#include "App.h"
#include "Actions.h"
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
  auto checkboxes = Container::Vertical({});

  // The list used to be built once, here, from a scan result that was still empty --
  // and never again. Every scan therefore ended on a screen that said "Empty container"
  // under a header claiming it had found eight apps. Rebuilding is now tied to the
  // result changing, and runs on the UI thread out of Job::Collect.
  auto rebuild = [state, checkboxes]() {
    checkboxes->DetachAllChildren();
    if (state->scan_selected.size() < state->scan_result.apps.size()) {
      state->scan_selected.resize(state->scan_result.apps.size(), {true});
    }
    for (size_t i = 0; i < state->scan_result.apps.size(); ++i) {
      const auto& app = state->scan_result.apps[i];
      std::string label = app.name + " (" + app.title + ") - " +
                          Shapes::ShapeToString(app.shape) + " [" +
                          Paths::ContractUser(app.exe_path) + "]";
      checkboxes->Add(Checkbox(label, &state->scan_selected[i].selected));
    }
  };
  state->on_scan_result_changed = rebuild;
  rebuild();

  auto btn_add = Button(" Add Selected ", [state]() { StartAddSelected(*state); });
  auto btn_rescan = Button(" Rescan ", [state]() { StartScan(*state); });
  auto btn_back = Button(" Back ", [state, screen]() {
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  auto buttons = Container::Horizontal({btn_add, btn_rescan, btn_back});

  container->Add(checkboxes);
  container->Add(buttons);

  return Renderer(container, [state, checkboxes, buttons]() {
    std::ostringstream ss;
    ss << state->scan_result.apps.size() << " apps found · "
       << std::fixed << std::setprecision(1) << state->scan_result.stats.elapsed_seconds << " s · "
       << state->scan_result.stats.files_probed << " files probed";

    Element list = state->scan_result.apps.empty()
                       ? Element(text("Nothing found. [r] rescans from this screen.") | dim)
                       : (checkboxes->Render() | vscroll_indicator | frame);

    return vbox({
      text(" Scan Results ") | bold,
      text(ss.str()) | dim,
      separator(),
      list | flex,
      separator(),
      buttons->Render(),
    }) | borderRounded;
  });
}

} // namespace Sleeve::Tui
