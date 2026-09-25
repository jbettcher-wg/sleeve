// SPDX-License-Identifier: MIT
#include "Actions.h"
#include "App.h"

#include "AppConfigWriter.h"
#include "Backend.h"
#include "Diff.h"
#include "FileWriter.h"
#include "Generate.h"
#include "Health.h"
#include "Libs.h"
#include "Paths.h"
#include "Process.h"
#include "RootFS.h"
#include "Scanner.h"
#include "Theme.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace Sleeve::Tui {

namespace fs = std::filesystem;

namespace {

std::string OneDecimal(double v) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << v;
  return ss.str();
}

// Same shape as `sleeve wrap`/health writes them: only quote when it would matter.
std::string ShellShow(const std::string& word) {
  bool needsQuotes = word.empty();
  for (char c : word) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' || c == '-' ||
          c == '_' || c == '=' || c == '+' || c == ':' || c == ',' || c == '@' || c == '~')) {
      needsQuotes = true;
      break;
    }
  }
  if (!needsQuotes) {
    return word;
  }
  std::string out = "'";
  for (char c : word) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
}

// Builds the launcher/diff pair for one file, the way both the write path and the
// options screen need it.
void AppendFile(std::vector<std::pair<std::string, std::string>>& files,
                std::vector<std::string>& diffs, const Record::AppRecord& record,
                const std::string& path, const std::string& content) {
  auto status = FileWriter::CheckStatus(
      path, record.generated.count(path) ? record.generated.at(path) : "");
  files.push_back({path, content});
  diffs.push_back(Diff::UnifiedDiff(status.existing_content, content, path + " (current)",
                                    path + " (new)"));
}

struct PreviewWork {
  Record::AppRecord record;
  std::vector<std::pair<std::string, std::string>> files;
  std::vector<std::string> diffs;
};

} // namespace

std::string DescribeHealthCommand(const Record::AppRecord& record) {
  // Health::RunCheck picks the launcher when one exists and the executable otherwise.
  // This has to agree with it, because the whole point of showing the line is that it is
  // the line that will run.
  const char* home = std::getenv("HOME");
  std::string launcher = (home ? std::string(home) : "") + "/.local/bin/" + record.name;
  std::error_code ec;
  std::string target = fs::exists(launcher, ec) ? launcher : record.GetResolvedExePath();
  // Contracted to ~ so the line stays readable; a shell expands it back to the same path.
  return ShellShow(Paths::ContractUser(target)) + " --version";
}

bool StartScan(AppState& state) {
  auto result = std::make_shared<Scanner::ScanResult>();

  Job::Request req;
  req.label = "Scanning for " + Backend::GetActiveBackend().archName + " programs";
  req.cancel_hint = "[esc] cancel — nothing is written by a scan";
  req.cancellable = true;
  req.work = [result](JobToken& token) {
    token.Progress("reading directories (cold cache makes this slower)");
    if (token.Cancelled()) {
      return;
    }
    Scanner::ScanOptions opt;
    *result = Scanner::RunScan(opt);
  };
  req.deliver = [&state, result]() {
    state.scan_result = std::move(*result);
    state.scan_selected.assign(state.scan_result.apps.size(), {true});
    state.selected_scan_index = 0;
    if (state.on_scan_result_changed) {
      state.on_scan_result_changed();
    }
    state.current_tab = ScreenTab::Scan;
    state.status_line = std::to_string(state.scan_result.apps.size()) + " apps found in " +
                        OneDecimal(state.scan_result.stats.elapsed_seconds) + " s · " +
                        std::to_string(state.scan_result.stats.files_probed) + " files probed";
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

bool StartThemeReload(AppState& state) {
  auto palette = std::make_shared<Theme::Palette>();
  auto themeArg = std::make_shared<std::string>(state.theme_override);

  Job::Request req;
  req.label = "Reading the Omarchy theme";
  req.cancel_hint = "[esc] stop waiting — the theme stays as it is";
  req.cancellable = true;
  req.work = [palette, themeArg](JobToken& token) {
    token.Progress("asking omarchy-theme-color for the palette");
    if (token.Cancelled()) {
      return;
    }
    *palette = Theme::ResolvePalette(*themeArg);
  };
  req.deliver = [&state, palette]() {
    state.palette = *palette;
    state.Refresh();
    state.status_line = "theme: " + state.palette.theme_name + " (" +
                        state.palette.source_description + ")";
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

bool StartAddSelected(AppState& state) {
  auto chosen = std::make_shared<std::vector<Shapes::AppCandidate>>();
  for (size_t i = 0; i < state.scan_result.apps.size(); ++i) {
    if (i < state.scan_selected.size() && state.scan_selected[i].selected) {
      chosen->push_back(state.scan_result.apps[i]);
    }
  }
  if (chosen->empty()) {
    state.status_line = "nothing selected to add";
    return false;
  }

  auto defaultRootfs = std::make_shared<std::string>(
      state.rootfs_result.rootfses.empty() ? "" : state.rootfs_result.rootfses[0].base_path);
  auto added = std::make_shared<size_t>(0);

  Job::Request req;
  req.label = "Adding " + std::to_string(chosen->size()) + " app(s)";
  req.cancel_hint = "[esc] stop after the app being written";
  req.cancellable = true;
  req.work = [chosen, defaultRootfs, added](JobToken& token) {
    for (const auto& cand : *chosen) {
      if (token.Cancelled()) {
        return;
      }
      token.Progress("writing record for " + cand.name);
      auto rec = Record::CreateFromCandidate(cand, *defaultRootfs);
      if (Record::SaveRecord(rec)) {
        ++*added;
      }
    }
  };
  req.deliver = [&state, added]() {
    state.Refresh();
    state.current_tab = ScreenTab::Main;
    state.status_line = "added " + std::to_string(*added) + " app(s)";
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

bool StartPreviewBuild(AppState& state, bool saveFirst) {
  auto work = std::make_shared<PreviewWork>();
  work->record = state.editing_record;

  Job::Request req;
  req.label = "Preparing " + work->record.name;
  req.cancel_hint = "[esc] cancel — nothing has been written yet";
  req.cancellable = true;
  req.work = [work, saveFirst](JobToken& token) {
    if (saveFirst) {
      token.Progress("saving the record");
      Record::SaveRecord(work->record);
    }
    if (token.Cancelled()) {
      return;
    }
    token.Progress("generating launcher, desktop entry and AppConfig");
    auto gen = Generate::GenerateFiles(work->record);
    AppendFile(work->files, work->diffs, work->record, gen.launcher_path, gen.launcher_content);
    if (gen.has_desktop) {
      AppendFile(work->files, work->diffs, work->record, gen.desktop_path, gen.desktop_content);
    }
    if (gen.has_appconfig) {
      AppendFile(work->files, work->diffs, work->record, gen.appconfig_path,
                 gen.appconfig_content);
    }
  };
  req.deliver = [&state, work]() {
    state.editing_record = work->record;
    state.files_to_write = std::move(work->files);
    state.preview_diffs = std::move(work->diffs);
    state.preview_file_index = 0;
    state.Refresh();
    state.current_tab = ScreenTab::Preview;
    state.status_line = std::to_string(state.files_to_write.size()) + " file(s) ready to write";
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

bool StartWriteFiles(AppState& state) {
  auto work = std::make_shared<PreviewWork>();
  work->record = state.editing_record;
  work->files = state.files_to_write;

  Job::Request req;
  req.label = "Writing " + std::to_string(work->files.size()) + " file(s)";
  // Stopping halfway leaves a launcher that points at an AppConfig that was never
  // written, which is worse than waiting. Each write is atomic and there are only ever
  // three of them.
  req.cancel_hint = "writing cannot be interrupted";
  req.cancellable = false;
  req.work = [work](JobToken& token) {
    for (const auto& [path, content] : work->files) {
      token.Progress("writing " + Paths::ContractUser(path));
      mode_t mode = (path.find("/bin/") != std::string::npos) ? 0755 : 0644;
      if (!FileWriter::AtomicWrite(path, content, mode)) {
        token.Fail("could not write " + path);
        return;
      }
      work->record.generated[path] = FileWriter::ComputeSha256(content);
    }
    token.Progress("saving the record");
    if (!Record::SaveRecord(work->record)) {
      token.Fail("could not save the record for " + work->record.name);
    }
  };
  req.deliver = [&state, work]() {
    state.editing_record = work->record;
    state.Refresh();
    state.current_tab = ScreenTab::Main;
    state.status_line = "wrote " + std::to_string(work->files.size()) + " file(s)";
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

bool PrepareHealthCheck(AppState& state) {
  if (state.job.Running()) {
    return false;
  }
  if (state.selected_app_index < 0 ||
      state.selected_app_index >= static_cast<int>(state.records.size())) {
    return false;
  }
  const auto& rec = state.records[state.selected_app_index];
  state.pending_confirm.active = true;
  state.pending_confirm.title = "Health check: " + rec.name;
  state.pending_confirm.command = DescribeHealthCommand(rec);
  state.pending_confirm.on_accept = [&state]() { StartHealthCheck(state); };
  state.pending_confirm.warning =
      "This starts " + rec.title +
      " for real, under the emulator. A windowed application will put a window on "
      "screen over this terminal, and it keeps running until it exits or Core's "
      "120 s cap stops it.";
  return true;
}

namespace {
struct HealthWork {
  Record::AppRecord record;
  Health::CheckResult result;
};
} // namespace

bool StartHealthCheck(AppState& state) {
  if (state.selected_app_index < 0 ||
      state.selected_app_index >= static_cast<int>(state.records.size())) {
    return false;
  }

  auto work = std::make_shared<HealthWork>();
  // Health::RunCheck mutates and saves the record it is given, so it gets a copy. The
  // worker must never touch state.records; the UI thread is reading it to draw.
  work->record = state.records[state.selected_app_index];

  Job::Request req;
  req.label = "Health check: " + work->record.name;
  req.cancel_hint = "[esc] stop waiting (the launched process keeps running)";
  req.cancellable = true;
  req.work = [work](JobToken& token) {
    token.Progress("running " + DescribeHealthCommand(work->record));
    if (token.Cancelled()) {
      return;
    }
    Health::CheckOptions hopt;
    hopt.mode = Health::CheckMode::Version;
    hopt.timeout_seconds = 20.0;
    hopt.wmclass = work->record.desktop.wmclass;
    work->result = Health::RunCheck(work->record, hopt);
  };
  req.deliver = [&state, work]() {
    state.current_health_result = work->result;
    state.Refresh();
    state.current_tab = ScreenTab::Health;
    state.status_line = "health: " + work->result.status + " in " +
                        OneDecimal(work->result.elapsed_seconds) + " s";
  };

  bool started = state.job.Start(std::move(req));
  if (started) {
    state.status_line.clear();
  }
  return started;
}


// ---------------------------------------------------------------------------
// Libraries
// ---------------------------------------------------------------------------

namespace {

struct LibsWork {
  Record::AppRecord record;
  RootFS::ReadinessReport readiness;
  Libs::ScanResult scan;
  Libs::PackagePlan plan;
};

// The unmet sonames, in the order the report shows them.
std::vector<std::string> UnmetNames(const Libs::ScanResult& scan) {
  std::vector<std::string> names;
  for (const auto& u : scan.unmet) names.push_back(u.soname);
  return names;
}

} // namespace

bool StartLibsScan(AppState& state) {
  if (state.selected_app_index < 0 ||
      state.selected_app_index >= static_cast<int>(state.records.size())) {
    return false;
  }

  auto work = std::make_shared<LibsWork>();
  work->record = state.records[state.selected_app_index];

  Job::Request req;
  req.label = "Reading what " + work->record.name + " needs";
  req.cancel_hint = "[esc] cancel — a scan reads and writes nothing";
  req.cancellable = true;
  req.work = [work](JobToken& token) {
    token.Progress("checking the rootfs");
    work->readiness = RootFS::CheckReadiness(work->record.rootfs);
    if (token.Cancelled()) {
      return;
    }
    if (work->readiness.state == RootFS::Readiness::Missing ||
        work->readiness.state == RootFS::Readiness::WrongArch) {
      return; // nothing to resolve against; the screen offers to build one
    }
    auto opt = Libs::OptionsForRecord(work->record);
    // The dlopen half, for free: a library loaded by name at run time is invisible to
    // every ELF header, but the last health check's log names the ones that failed.
    std::string log = Libs::NewestHealthLog(work->record.name);
    if (!log.empty()) {
      std::ifstream f(log);
      std::stringstream ss;
      ss << f.rdbuf();
      opt.run_sonames = Libs::SonamesFromRunOutput(ss.str());
    }
    token.Progress("walking the application's ELF objects");
    work->scan = Libs::ScanApp(opt);
  };
  req.deliver = [&state, work]() {
    state.libs_readiness = work->readiness;
    state.libs_result = std::move(work->scan);
    state.libs_plan = Libs::PackagePlan {};
    state.libs_scanned = true;
    state.current_tab = ScreenTab::Libs;
    if (work->readiness.state == RootFS::Readiness::Missing ||
        work->readiness.state == RootFS::Readiness::WrongArch) {
      state.status_line = "no usable guest: " + work->readiness.summary;
    } else {
      state.status_line = std::to_string(state.libs_result.unmet.size()) + " unmet of " +
                          std::to_string(state.libs_result.all.size()) + " soname(s) · " +
                          std::to_string(state.libs_result.objects.size()) + " object(s) read";
    }
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

bool StartLibsPackages(AppState& state) {
  if (state.libs_result.unmet.empty()) {
    state.status_line = "nothing unmet to look up";
    return false;
  }
  if (state.selected_app_index < 0 ||
      state.selected_app_index >= static_cast<int>(state.records.size())) {
    return false;
  }

  auto work = std::make_shared<LibsWork>();
  work->record = state.records[state.selected_app_index];
  auto sonames = std::make_shared<std::vector<std::string>>(UnmetNames(state.libs_result));

  Job::Request req;
  req.label = "Asking guest pacman which packages own them";
  req.cancel_hint = "[esc] stop waiting (the query reads, it does not install)";
  req.cancellable = true;
  req.work = [work, sonames](JobToken& token) {
    token.Progress("pacman -F, under the emulator");
    if (token.Cancelled()) {
      return;
    }
    work->plan = Libs::MapSonamesToPackages(work->record.rootfs, *sonames);
  };
  req.deliver = [&state, work]() {
    state.libs_plan = std::move(work->plan);
    state.current_tab = ScreenTab::Libs;
    state.status_line = state.libs_plan.error.empty()
                            ? (std::to_string(state.libs_plan.packages.size()) +
                               " package(s) would cover it")
                            : state.libs_plan.error;
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

namespace {

// Starts one guest pacman action as a job. Both of them mutate the overlay, so both come
// through the confirmation panel first.
bool StartGuestPacman(AppState& state, const std::string& label,
                      std::function<Process::RunOutcome()> action) {
  auto work = std::make_shared<Process::RunOutcome>();

  Job::Request req;
  req.label = label;
  // Stopping halfway through a package transaction leaves the overlay with files from a
  // package its database does not list, which is worse than waiting.
  req.cancel_hint = "guest pacman cannot be interrupted safely";
  req.cancellable = false;
  req.work = [work, action](JobToken& token) {
    token.Progress("running guest pacman under the emulator");
    *work = action();
    if (!work->ok) {
      token.Fail(work->error);
    }
  };
  req.deliver = [&state, work]() {
    state.current_tab = ScreenTab::Libs;
    state.status_line =
        work->ok ? "guest pacman finished" : ("guest pacman failed: " + work->error);
    if (work->ok) {
      // What is unmet has changed, so the report on screen is stale. Say so rather than
      // leaving a list that no longer describes the guest.
      state.libs_scanned = false;
      state.libs_result = Libs::ScanResult {};
      state.libs_plan = Libs::PackagePlan {};
      state.status_line += " — press [l] to read the application again";
    }
  };
  if (state.job.Start(std::move(req))) {
    state.status_line.clear();
    return true;
  }
  return false;
}

} // namespace

bool PrepareLibsInstall(AppState& state) {
  if (state.job.Running()) {
    return false;
  }
  if (state.libs_plan.packages.empty()) {
    state.status_line = state.libs_plan.error.empty()
                            ? "no package set yet — press [p] to look them up"
                            : state.libs_plan.error;
    return false;
  }
  if (state.libs_readiness.state != RootFS::Readiness::Ok) {
    state.status_line = "refusing to write into this rootfs: " + state.libs_readiness.summary;
    return false;
  }
  if (state.selected_app_index < 0 ||
      state.selected_app_index >= static_cast<int>(state.records.size())) {
    return false;
  }

  std::string rootfs = state.records[state.selected_app_index].rootfs;
  std::vector<std::string> packages = state.libs_plan.packages;

  std::vector<std::string> args = {"-S", "--needed", "--noconfirm"};
  for (const auto& p : packages) args.push_back(p);

  state.pending_confirm.active = true;
  state.pending_confirm.title = "Install " + std::to_string(packages.size()) + " package(s)";
  state.pending_confirm.command =
      Process::Describe(Libs::BuildGuestPacmanCommand(rootfs, args, 1800.0));
  state.pending_confirm.warning =
      "This installs into " + state.libs_readiness.overlay_path +
      ", which every guest using this rootfs shares — including the sessions running right "
      "now. It cannot be undone from here.";
  state.pending_confirm.on_accept = [&state, rootfs, packages]() {
    StartGuestPacman(state, "Installing " + std::to_string(packages.size()) + " package(s)",
                     [rootfs, packages]() { return Libs::InstallPackages(rootfs, packages); });
  };
  return true;
}

bool PrepareLibsSync(AppState& state) {
  if (state.job.Running()) {
    return false;
  }
  if (state.libs_readiness.state != RootFS::Readiness::Ok) {
    state.status_line = "refusing to write into this rootfs: " + state.libs_readiness.summary;
    return false;
  }
  if (state.selected_app_index < 0 ||
      state.selected_app_index >= static_cast<int>(state.records.size())) {
    return false;
  }

  std::string rootfs = state.records[state.selected_app_index].rootfs;
  state.pending_confirm.active = true;
  state.pending_confirm.title = "Refresh the guest file database";
  state.pending_confirm.command =
      Process::Describe(Libs::BuildGuestPacmanCommand(rootfs, {"-Fy", "--noconfirm"}, 900.0));
  state.pending_confirm.warning =
      "Tracing a soname to a package needs the file database, and that is a download into " +
      state.libs_readiness.overlay_path + ". Nothing is installed by it.";
  state.pending_confirm.on_accept = [&state, rootfs]() {
    StartGuestPacman(state, "Refreshing the guest file database",
                     [rootfs]() { return Libs::SyncFilesDatabase(rootfs); });
  };
  return true;
}

bool PrepareRootFSBuild(AppState& state) {
  if (state.job.Running()) {
    return false;
  }
  if (!state.libs_readiness.auto_fixable) {
    state.status_line = state.libs_readiness.state == RootFS::Readiness::Ok
                            ? "the rootfs is fine; nothing to build"
                            : ("this one is yours to repair: " + state.libs_readiness.fix_hint);
    return false;
  }

  // Mirrors come from the settings file and are passed straight through. sleeve holds no
  // opinion about where packages come from; the fetcher already has one.
  auto provision = RootFS::ProvisionOptionsFromSettings();
  if (state.libs_readiness.state == RootFS::Readiness::WrongArch) {
    provision.dest = state.libs_readiness.rootfs_path;
    provision.force = true;
  } else if (!state.libs_readiness.requested.empty() &&
             state.libs_readiness.requested.find('/') == std::string::npos) {
    provision.name = state.libs_readiness.requested;
  }

  state.pending_confirm.active = true;
  state.pending_confirm.title = "Build a guest rootfs";
  state.pending_confirm.command = Process::Describe(RootFS::BuildProvisionCommand(provision));
  state.pending_confirm.warning =
      "This downloads about 860 MB and writes about 1.9 GB, and it can take a long while. "
      "It builds the base, the per-user overlay and guest pacman, and points the config at "
      "the result.";
  state.pending_confirm.on_accept = [&state, provision]() {
    auto work = std::make_shared<Process::RunOutcome>();
    Job::Request req;
    req.label = "Building a guest rootfs";
    req.cancel_hint = "a half-extracted rootfs is worse than a wait";
    req.cancellable = false;
    req.work = [work, provision](JobToken& token) {
      token.Progress("POWERarmRootFSFetcher build — this downloads a lot");
      *work = RootFS::Provision(provision);
      if (!work->ok) {
        token.Fail(work->error);
      }
    };
    req.deliver = [&state, work]() {
      state.Refresh();
      state.libs_readiness = RootFS::CheckReadiness(
          state.selected_app_index >= 0 &&
                  state.selected_app_index < static_cast<int>(state.records.size())
              ? state.records[state.selected_app_index].rootfs
              : std::string());
      state.libs_scanned = false;
      state.current_tab = ScreenTab::Libs;
      state.status_line = work->ok ? "rootfs built — press [l] to read the application again"
                                   : ("build failed: " + work->error);
    };
    if (state.job.Start(std::move(req))) {
      state.status_line.clear();
    }
  };
  return true;
}

} // namespace Sleeve::Tui
