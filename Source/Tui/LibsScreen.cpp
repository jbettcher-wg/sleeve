// SPDX-License-Identifier: MIT
#include "App.h"
#include "Actions.h"
#include "Libs.h"
#include "Paths.h"
#include "RootFS.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace Sleeve::Tui {

using namespace ftxui;

namespace {

Color Accent(const Theme::Palette& pal) {
  if (pal.accent) return Color::RGB(pal.accent->r, pal.accent->g, pal.accent->b);
  return Color::Palette16(3);
}

std::string Pad(std::string value, size_t width) {
  while (value.size() < width) value += " ";
  return value;
}

// Why a soname is unmet, in the words that say what to do about it.
std::string WhyUnmet(const Libs::SonameStatus& status) {
  if (status.host_path.empty()) return "no layer has it";
  return "only the host has it (" + Libs::MachineName(status.machine) + ")";
}

} // namespace

Component CreateLibsScreen(AppState* state, ScreenInteractive* screen) {
  auto btn_back = Button(" Back [b] ", [state, screen]() {
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  auto renderer = Renderer(btn_back, [state, btn_back]() {
    Color accent = Accent(state->palette);
    const auto& scan = state->libs_result;
    const auto& plan = state->libs_plan;
    const auto& readiness = state->libs_readiness;

    std::string appName = (state->selected_app_index >= 0 &&
                           state->selected_app_index < static_cast<int>(state->records.size()))
                              ? state->records[state->selected_app_index].name
                              : std::string();

    Elements rows;
    rows.push_back(hbox({
        text(" Libraries: ") | bold,
        text(appName) | bold | color(accent),
    }));
    rows.push_back(separator());

    bool ok = (readiness.state == RootFS::Readiness::Ok);
    rows.push_back(hbox({
        text(ok ? "✓ " : "✗ ") | bold | color(ok ? Color::Palette16(2) : Color::Palette16(1)),
        paragraph(readiness.summary) | flex,
    }));
    if (!readiness.fix_hint.empty()) {
      rows.push_back(hbox({text("  fix: ") | color(Color::Palette16(8)),
                           paragraph(readiness.fix_hint) | flex}));
    }
    rows.push_back(separator());

    if (!state->libs_scanned) {
      rows.push_back(text("Nothing read yet. Press [l] on the app list to read this "
                          "application's ELF objects.") |
                     dim);
    } else if (readiness.auto_fixable) {
      rows.push_back(paragraph("There is no guest to resolve against, so nothing was read. "
                               "[B] builds one.") |
                     color(Color::Palette16(3)));
    } else {
      rows.push_back(hbox({
          text(std::to_string(scan.objects.size()) + " ELF object(s) read, " +
               std::to_string(scan.files_probed) + " file(s) probed") |
              color(Color::Palette16(8)),
      }));
      rows.push_back(hbox({
          text(std::to_string(scan.all.size()) + " soname(s) named, ") | color(Color::Palette16(8)),
          text(std::to_string(scan.unmet.size()) + " unmet") | bold |
              color(scan.unmet.empty() ? Color::Palette16(2) : Color::Palette16(1)),
      }));
      for (const auto& note : scan.notes) {
        rows.push_back(hbox({text("  ! ") | color(Color::Palette16(3)), paragraph(note) | flex}));
      }
      rows.push_back(separator());

      if (scan.unmet.empty()) {
        rows.push_back(text("Every soname resolves in the overlay, the base rootfs or the "
                            "application's own directory.") |
                       color(Color::Palette16(2)));
      } else {
        Elements unmet;
        for (const auto& u : scan.unmet) {
          std::string by;
          for (size_t i = 0; i < u.needed_by.size(); ++i) by += (i ? ", " : "") + u.needed_by[i];
          unmet.push_back(hbox({
              text("  " + Pad(u.soname, 26)) | bold,
              text(Pad(u.discovered_by, 9)) | color(Color::Palette16(6)),
              text(Pad(WhyUnmet(u), 34)) | color(Color::Palette16(8)),
              text(by.empty() ? "" : "<- " + by) | color(Color::Palette16(8)),
          }));
        }
        rows.push_back(vbox(std::move(unmet)) | vscroll_indicator | frame | flex);
      }

      if (!plan.error.empty()) {
        rows.push_back(separator());
        rows.push_back(hbox({text("packages: ") | color(Color::Palette16(8)),
                             paragraph(plan.error) | flex}));
      } else if (!plan.packages.empty()) {
        rows.push_back(separator());
        std::string set;
        for (size_t i = 0; i < plan.packages.size(); ++i) set += (i ? " " : "") + plan.packages[i];
        rows.push_back(hbox({
            text("install set  ") | color(Color::Palette16(8)),
            paragraph(set) | bold | flex,
        }));
        if (!plan.unmatched.empty()) {
          std::string left;
          for (size_t i = 0; i < plan.unmatched.size(); ++i) {
            left += (i ? " " : "") + plan.unmatched[i];
          }
          rows.push_back(hbox({text("no owner    ") | color(Color::Palette16(8)),
                               paragraph(left) | flex}));
        }
      }
    }

    rows.push_back(separator());
    rows.push_back(hbox({
        text(" [p]") | bold | color(accent), text(" find packages  "),
        text("[i]") | bold | color(accent), text(" install them  "),
        text("[y]") | bold | color(accent), text(" refresh file db  "),
        text("[B]") | bold | color(accent), text(" build rootfs  "),
        text("[b]") | bold | color(accent), text(" back "),
    }));

    return vbox(std::move(rows)) | borderRounded;
  });

  // The screen's own keys. Each one that writes anything goes through the confirmation
  // panel first; none of them acts on the keypress alone.
  return CatchEvent(renderer, [state, screen](Event event) {
    if (state->current_tab != ScreenTab::Libs) return false;
    if (event == Event::Character('p')) {
      StartLibsPackages(*state);
      return true;
    }
    if (event == Event::Character('i')) {
      PrepareLibsInstall(*state);
      return true;
    }
    if (event == Event::Character('y')) {
      PrepareLibsSync(*state);
      return true;
    }
    if (event == Event::Character('B')) {
      PrepareRootFSBuild(*state);
      return true;
    }
    if (event == Event::Character('b')) {
      state->current_tab = ScreenTab::Main;
      screen->PostEvent(Event::Custom);
      return true;
    }
    return false;
  });
}

} // namespace Sleeve::Tui
