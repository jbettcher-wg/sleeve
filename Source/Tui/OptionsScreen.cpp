// SPDX-License-Identifier: MIT
#include "App.h"
#include "Paths.h"
#include "Generate.h"
#include "FileWriter.h"
#include "Diff.h"
#include "AppConfigWriter.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <sstream>
#include <vector>

namespace Sleeve::Tui {

using namespace ftxui;

Component CreateOptionsScreen(AppState* state, ScreenInteractive* screen) {
  auto container = Container::Vertical({});

  auto input_exe = Input(&state->editing_record.exe, "executable path");
  auto input_args = Input(&state->edit_args_str, "command line arguments");
  auto input_env = Input(&state->edit_env_str, "KEY=VAL KEY2=VAL2");
  auto input_rootfs = Input(&state->editing_record.rootfs, "rootfs path");
  auto input_wmclass = Input(&state->editing_record.desktop.wmclass, "WMClass");

  auto cb_desktop = Checkbox("Enable Desktop Entry", &state->editing_record.desktop.enabled);
  auto cb_startup_notify = Checkbox("Startup notification (spins forever under emulation)",
                                    &state->editing_record.desktop.startup_notify);

  // Emulator options
  auto cb_code_cache = Checkbox("Enable Code Caching (WIP)", &state->edit_code_cache);
  std::vector<std::string> scope_entries = {"off", "rootfs", "home", "all"};
  auto radio_scope = Radiobox(&scope_entries, &state->edit_cache_scope_index);
  auto cb_cmp_fusion = Checkbox("Compare-Branch Fusion", &state->edit_cmp_fusion);
  auto cb_profile_stats = Checkbox("ProfileStats (/dev/shm/powerarm-<pid>-stats)", &state->edit_profile_stats);
  auto cb_mangohud = Checkbox("MangoHud HUD overlay", &state->editing_record.mangohud.enabled);
  auto input_emulator = Input(&state->edit_emulator_path, "stable via binfmt or /path/to/POWERarm");

  auto btn_preview = Button(" Preview & Write ", [state, screen, scope_entries]() {
    // Quote-aware: an argument or an env value may contain a space.
    state->editing_record.args = Record::SplitArgsFromEditing(state->edit_args_str);
    state->editing_record.env = Record::SplitEnvFromEditing(state->edit_env_str);

    // Update emulator options
    state->editing_record.appconfig["EnableCodeCachingWIP"] = state->edit_code_cache ? "1" : "0";
    if (state->edit_cache_scope_index >= 0 && state->edit_cache_scope_index < static_cast<int>(scope_entries.size())) {
      state->editing_record.appconfig["CodeCacheScope"] = scope_entries[state->edit_cache_scope_index];
    }
    state->editing_record.appconfig["DisableCmpBranchFusion"] = state->edit_cmp_fusion ? "0" : "1";
    state->editing_record.appconfig["ProfileStats"] = state->edit_profile_stats ? "1" : "0";

    if (state->edit_emulator_path.empty() || state->edit_emulator_path == "stable") {
      state->editing_record.emulator = std::nullopt;
    } else {
      state->editing_record.emulator = state->edit_emulator_path;
    }

    // Save record updates
    Record::SaveRecord(state->editing_record);
    state->Refresh();

    // Prepare diffs for preview modal
    auto gen = Generate::GenerateFiles(state->editing_record);
    state->files_to_write.clear();
    state->preview_diffs.clear();

    // 1. Launcher
    auto launcherStatus = FileWriter::CheckStatus(gen.launcher_path);
    std::string launcherDiff = Diff::UnifiedDiff(launcherStatus.existing_content, gen.launcher_content,
                                                 gen.launcher_path + " (current)", gen.launcher_path + " (new)");
    state->files_to_write.push_back({gen.launcher_path, gen.launcher_content});
    state->preview_diffs.push_back(launcherDiff);

    // 2. Desktop entry
    if (gen.has_desktop) {
      auto desktopStatus = FileWriter::CheckStatus(gen.desktop_path);
      std::string desktopDiff = Diff::UnifiedDiff(desktopStatus.existing_content, gen.desktop_content,
                                                  gen.desktop_path + " (current)", gen.desktop_path + " (new)");
      state->files_to_write.push_back({gen.desktop_path, gen.desktop_content});
      state->preview_diffs.push_back(desktopDiff);
    }

    // 3. AppConfig
    if (gen.has_appconfig) {
      auto acStatus = FileWriter::CheckStatus(gen.appconfig_path);
      std::string acDiff = Diff::UnifiedDiff(acStatus.existing_content, gen.appconfig_content,
                                             gen.appconfig_path + " (current)", gen.appconfig_path + " (new)");
      state->files_to_write.push_back({gen.appconfig_path, gen.appconfig_content});
      state->preview_diffs.push_back(acDiff);
    }

    state->current_tab = ScreenTab::Preview;
    screen->PostEvent(Event::Custom);
  });

  auto btn_back = Button(" Back ", [state, screen]() {
    state->current_tab = ScreenTab::Main;
    screen->PostEvent(Event::Custom);
  });

  auto buttons = Container::Horizontal({btn_preview, btn_back});

  container->Add(input_exe);
  container->Add(input_args);
  container->Add(input_env);
  container->Add(input_rootfs);
  container->Add(cb_desktop);
  container->Add(input_wmclass);
  container->Add(cb_startup_notify);
  container->Add(cb_code_cache);
  container->Add(radio_scope);
  container->Add(cb_cmp_fusion);
  container->Add(cb_profile_stats);
  container->Add(cb_mangohud);
  container->Add(input_emulator);
  container->Add(buttons);

  return Renderer(container, [state, input_exe, input_args, input_env, input_rootfs,
                             cb_desktop, cb_startup_notify, input_wmclass, cb_code_cache, radio_scope,
                             cb_cmp_fusion, cb_profile_stats, cb_mangohud, input_emulator, buttons]() {
    return vbox({
      text(" Options: " + state->editing_record.name) | bold,
      separator(),
      text("Launch Settings") | bold | color(Color::Palette16(3)),
      hbox({ text("Executable:  ") | bold, input_exe->Render() | flex }),
      hbox({ text("Arguments:   ") | bold, input_args->Render() | flex }),
      hbox({ text("Environment: ") | bold, input_env->Render() | flex }),
      hbox({ text("RootFS:      ") | bold, input_rootfs->Render() | flex }),
      separator(),
      text("Desktop Entry") | bold | color(Color::Palette16(3)),
      cb_desktop->Render(),
      hbox({ text("WMClass:     ") | bold, input_wmclass->Render() | flex }),
      cb_startup_notify->Render(),
      separator(),
      text("Emulator Settings (written to AppConfig/" + state->editing_record.name + ".json)") | bold | color(Color::Palette16(3)),
      cb_code_cache->Render(),
      hbox({ text("  Scope:     ") | color(Color::Palette16(8)), radio_scope->Render() }),
      cb_cmp_fusion->Render(),
      cb_profile_stats->Render(),
      cb_mangohud->Render(),
      hbox({ text("Emulator:    ") | bold, input_emulator->Render() | flex }),
      separator(),
      buttons->Render(),
    }) | borderRounded;
  });
}

} // namespace Sleeve::Tui
