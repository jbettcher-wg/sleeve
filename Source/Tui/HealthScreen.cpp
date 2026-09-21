// SPDX-License-Identifier: MIT
#include "App.h"
#include "Health.h"
#include "Theme.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace Sleeve::Tui {

using namespace ftxui;

Component CreateHealthScreen(AppState* state, ScreenInteractive* screen) {
  auto btn_back = Button(" Back [b] ", [state, screen]() {
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  return Renderer(btn_back, [state, btn_back]() {
    const auto& res = state->current_health_result;
    bool isOk = (res.status == "ok");

    Color statusColor = isOk ? Color::Palette16(2) : Color::Palette16(1);
    std::string statusGlyph = isOk ? "✓ " : "✗ ";

    Elements notesElements;
    for (const auto& note : res.notes) {
      notesElements.push_back(hbox({
        text("  • ") | color(Color::Palette16(8)),
        paragraph(note) | flex,
      }));
    }

    return vbox({
      hbox({
        text(" Health Check: ") | bold,
        text(state->records.empty() ? "" : state->records[state->selected_app_index].name) | bold,
        text("  (" + std::to_string(res.elapsed_seconds) + " s)") | color(Color::Palette16(8)),
      }),
      separator(),
      hbox({
        text(statusGlyph) | color(statusColor) | bold,
        text(res.summary) | bold,
      }),
      separator(),
      vbox(std::move(notesElements)),
      separator(),
      hbox({
        text("Log: ") | color(Color::Palette16(8)),
        text(res.log_path.empty() ? "(none)" : res.log_path),
      }),
      separator(),
      hbox({
        filler(),
        btn_back->Render(),
        filler(),
      }),
    }) | borderRounded;
  });
}

} // namespace Sleeve::Tui
