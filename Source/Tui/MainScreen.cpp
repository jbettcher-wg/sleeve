// SPDX-License-Identifier: MIT
#include "App.h"
#include "Paths.h"
#include "Backend.h"
#include "FileWriter.h"
#include "Generate.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <sstream>

namespace Sleeve::Tui {

using namespace ftxui;

static Color GetAccentColor(const Theme::Palette& pal) {
  if (pal.accent) {
    return Color::RGB(pal.accent->r, pal.accent->g, pal.accent->b);
  }
  return Color::Palette16(3); // Yellow
}

static Color GetSelectionBg(const Theme::Palette& pal) {
  if (pal.selection_bg) {
    return Color::RGB(pal.selection_bg->r, pal.selection_bg->g, pal.selection_bg->b);
  }
  return Color::Palette16(4); // Blue fallback
}

static Color GetSelectionFg(const Theme::Palette& pal) {
  if (pal.selection_fg) {
    return Color::RGB(pal.selection_fg->r, pal.selection_fg->g, pal.selection_fg->b);
  }
  return Color::Palette16(15); // White fallback
}

Component CreateMainScreen(AppState* state, ScreenInteractive* screen) {
  auto menu_entries = std::make_shared<std::vector<std::string>>();

  auto update_entries = [state, menu_entries]() {
    menu_entries->clear();
    for (const auto& rec : state->records) {
      std::string line = rec.name;
      while (line.size() < 12) line += " ";
      line += rec.title;
      while (line.size() < 32) line += " ";
      line += rec.shape;
      while (line.size() < 42) line += " ";
      line += "✓";
      menu_entries->push_back(line);
    }
  };
  update_entries();

  MenuOption menu_opt;
  menu_opt.on_enter = [state, screen]() {
    if (state->selected_app_index >= 0 && state->selected_app_index < static_cast<int>(state->records.size())) {
      state->editing_record = state->records[state->selected_app_index];
      state->edit_args_str = Record::JoinArgsForEditing(state->editing_record.args);
      state->edit_env_str = Record::JoinEnvForEditing(state->editing_record.env);
      state->current_tab = ScreenTab::Options;
      screen->PostEvent(Event::Custom);
    }
  };

  auto app_menu = Menu(menu_entries.get(), &state->selected_app_index, menu_opt);

  return Renderer(app_menu, [state, app_menu, update_entries]() {
    update_entries();

    Color accentCol = GetAccentColor(state->palette);
    Color selBg = GetSelectionBg(state->palette);
    Color selFg = GetSelectionFg(state->palette);
    (void)selBg; (void)selFg;

    // Top status line
    auto header = hbox({
      text(" sleeve · ") | bold | color(accentCol),
      text(Backend::GetActiveBackend().displayName + " apps  ") | bold,
      filler(),
      text("stable: ") | color(Color::Palette16(8)),
      text(state->stable_version.empty() ? "dev" : state->stable_version) | bold,
      text("   binfmt: ") | color(Color::Palette16(8)),
      text(state->binfmt_status) | bold,
      text("   theme: ") | color(Color::Palette16(8)),
      text(state->palette.theme_name) | bold | color(accentCol),
      text(" "),
    });

    // Left pane: Apps
    Element left_content;
    if (state->records.empty()) {
      left_content = vbox({
        text("No apps configured yet.") | dim,
        text("Press [s] to scan directories for " + Backend::GetActiveBackend().archName + " programs.") | bold,
      });
    } else {
      left_content = app_menu->Render() | vscroll_indicator | frame;
    }

    auto left_pane = window(text(" Apps ") | bold, left_content) | flex;

    // Right pane: Selected app details
    Element right_content;
    if (state->selected_app_index >= 0 && state->selected_app_index < static_cast<int>(state->records.size())) {
      const auto& cur = state->records[state->selected_app_index];

      auto outputs = Generate::GenerateFiles(cur);
      auto launcherStatus = FileWriter::CheckStatus(outputs.launcher_path);
      auto desktopStatus = FileWriter::CheckStatus(outputs.desktop_path);

      std::string launcherVerdictStr = (launcherStatus.verdict == FileWriter::FileVerdict::Managed) ? "managed" :
                                       (launcherStatus.verdict == FileWriter::FileVerdict::New) ? "new" :
                                       (launcherStatus.verdict == FileWriter::FileVerdict::HandEdited) ? "modified" : "foreign";

      std::string desktopVerdictStr = (desktopStatus.verdict == FileWriter::FileVerdict::Managed) ? "managed" :
                                      (desktopStatus.verdict == FileWriter::FileVerdict::New) ? "new" :
                                      (desktopStatus.verdict == FileWriter::FileVerdict::HandEdited) ? "modified" : "foreign";

      right_content = vbox({
        text(cur.title + " (" + Backend::GetActiveBackend().archName + ")") | bold | color(accentCol),
        separator(),
        hbox({ text("exe     ") | color(Color::Palette16(8)), text(Paths::ContractUser(cur.GetResolvedExePath())) | bold }),
        hbox({ text("rootfs  ") | color(Color::Palette16(8)), text(Paths::ContractUser(cur.rootfs)) }),
        hbox({ text("shape   ") | color(Color::Palette16(8)), text(cur.shape) }),
        hbox({ text("args    ") | color(Color::Palette16(8)), text(cur.args.empty() ? "(none)" : [&]() {
          std::string a;
          for (const auto& s : cur.args) a += s + " ";
          return a;
        }()) }),
        separator(),
        text("Files:") | bold,
        hbox({ text("  launcher: ") | color(Color::Palette16(8)), text(Paths::ContractUser(outputs.launcher_path)), text(" [" + launcherVerdictStr + "]") | color(launcherStatus.verdict == FileWriter::FileVerdict::Managed ? Color::Green : Color::Yellow) }),
        hbox({ text("  desktop:  ") | color(Color::Palette16(8)), text(Paths::ContractUser(outputs.desktop_path)), text(" [" + desktopVerdictStr + "]") | color(desktopStatus.verdict == FileWriter::FileVerdict::Managed ? Color::Green : Color::Yellow) }),
        separator(),
        text("Config:") | bold,
        hbox({ text("  cache: ") | color(Color::Palette16(8)), text(cur.appconfig.count("EnableCodeCachingWIP") && cur.appconfig.at("EnableCodeCachingWIP") == "1" ? "on" : "off") }),
        hbox({ text("  fusion: ") | color(Color::Palette16(8)), text(cur.appconfig.count("DisableCmpBranchFusion") && cur.appconfig.at("DisableCmpBranchFusion") == "1" ? "off" : "on") }),
      });
    } else {
      right_content = text("Select an app to view details") | dim;
    }

    auto right_pane = window(text(" Details ") | bold, right_content) | flex;

    // Bottom keys line
    auto keys_line = hbox({
      text(" [s]") | bold | color(accentCol), text(" scan  "),
      text("[a]") | bold | color(accentCol), text(" add  "),
      text("[enter]") | bold | color(accentCol), text(" options  "),
      text("[w]") | bold | color(accentCol), text(" write files  "),
      text("[r]") | bold | color(accentCol), text(" rootfs  "),
      text("[t]") | bold | color(accentCol), text(" theme  "),
      text("[q]") | bold | color(accentCol), text(" quit "),
    });

    return vbox({
      header,
      separator(),
      hbox({ left_pane, right_pane }) | flex,
      separator(),
      keys_line,
    });
  });
}

} // namespace Sleeve::Tui
