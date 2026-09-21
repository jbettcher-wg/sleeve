// SPDX-License-Identifier: MIT
#include "App.h"
#include "FileWriter.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <sstream>

namespace Sleeve::Tui {

using namespace ftxui;

Component CreatePreviewModal(AppState* state, ScreenInteractive* screen) {
  auto container = Container::Vertical({});

  auto btn_write_all = Button(" Write All ", [state, screen]() {
    for (const auto& [path, content] : state->files_to_write) {
      mode_t mode = (path.find("/bin/") != std::string::npos) ? 0755 : 0644;
      FileWriter::AtomicWrite(path, content, mode);

      // Record the hash in the app record if applicable
      std::string sha = FileWriter::ComputeSha256(content);
      state->editing_record.generated[path] = sha;
    }
    Record::SaveRecord(state->editing_record);
    state->Refresh();
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  auto btn_cancel = Button(" Cancel ", [state, screen]() {
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  auto buttons = Container::Horizontal({btn_write_all, btn_cancel});
  container->Add(buttons);

  return Renderer(container, [state, buttons]() {
    Elements diff_elements;
    for (size_t i = 0; i < state->preview_diffs.size(); ++i) {
      const auto& d = state->preview_diffs[i];
      std::istringstream stream(d);
      std::string line;
      Elements lines;
      while (std::getline(stream, line)) {
        if (!line.empty() && line[0] == '+') {
          lines.push_back(text(line) | color(Color::Green));
        } else if (!line.empty() && line[0] == '-') {
          lines.push_back(text(line) | color(Color::Red));
        } else {
          lines.push_back(text(line));
        }
      }
      diff_elements.push_back(vbox(lines) | borderRounded);
    }

    return vbox({
      text(" Preview Changes ") | bold,
      text("The following files will be written atomically:") | dim,
      separator(),
      vbox(diff_elements) | vscroll_indicator | frame | flex,
      separator(),
      buttons->Render(),
    }) | borderRounded;
  });
}

} // namespace Sleeve::Tui
