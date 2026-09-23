// SPDX-License-Identifier: MIT
#include "Record.h"
#include "Paths.h"
#include "Backend.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <unistd.h>
#include <tiny-json.h>

namespace Sleeve::Record {

namespace fs = std::filesystem;

std::string AppRecord::GetResolvedExePath() const {
  if (exe.empty()) return "";
  if (exe[0] == '/') return exe;
  if (!source.dir.empty()) {
    return source.dir + "/" + exe;
  }
  return exe;
}

std::string AppRecord::GetResolvedIconPath() const {
  if (desktop.icon.empty()) return "";
  if (desktop.icon[0] == '/') return desktop.icon;
  if (!source.dir.empty()) {
    return source.dir + "/" + desktop.icon;
  }
  return desktop.icon;
}

static std::string QuoteForEditing(const std::string& value) {
  bool needsQuotes = value.empty();
  for (char c : value) {
    if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '\\') {
      needsQuotes = true;
      break;
    }
  }
  if (!needsQuotes) return value;

  std::string out = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static std::vector<std::string> SplitRespectingQuotes(const std::string& text) {
  std::vector<std::string> words;
  std::string current;
  bool inWord = false;
  char quote = '\0';

  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\\' && i + 1 < text.size() && quote != '\'') {
      current.push_back(text[++i]);
      inWord = true;
      continue;
    }
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      inWord = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n') {
      if (inWord) {
        words.push_back(current);
        current.clear();
        inWord = false;
      }
      continue;
    }
    current.push_back(c);
    inWord = true;
  }
  if (inWord) words.push_back(current);
  return words;
}

std::string JoinArgsForEditing(const std::vector<std::string>& args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) out += " ";
    out += QuoteForEditing(args[i]);
  }
  return out;
}

std::vector<std::string> SplitArgsFromEditing(const std::string& text) {
  return SplitRespectingQuotes(text);
}

std::string JoinEnvForEditing(const std::map<std::string, std::string>& env) {
  std::string out;
  for (const auto& [k, v] : env) {
    if (!out.empty()) out += " ";
    out += QuoteForEditing(k + "=" + v);
  }
  return out;
}

std::map<std::string, std::string> SplitEnvFromEditing(const std::string& text) {
  std::map<std::string, std::string> env;
  for (const auto& pair : SplitRespectingQuotes(text)) {
    auto eq = pair.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    env[pair.substr(0, eq)] = pair.substr(eq + 1);
  }
  return env;
}

bool IsValidAppName(const std::string& name) {
  if (name.empty() || name.size() > 128) return false;
  if (name == "." || name == "..") return false;
  if (name.front() == '.' || name.front() == '-') return false;
  for (char c : name) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == '+' || c == '@';
    if (!ok) return false;
  }
  return true;
}

AppRecord CreateFromCandidate(const Shapes::AppCandidate& cand, const std::string& defaultRootfs) {
  AppRecord rec;
  rec.format = 1;
  rec.name = cand.name;
  rec.title = cand.title;
  rec.shape = Shapes::ShapeToString(cand.shape);
  rec.cwd = cand.cwd;
  rec.args = cand.default_args;
  rec.env = cand.default_env;
  rec.presets = cand.presets;

  if (!cand.rootfs_base.empty()) {
    rec.rootfs = cand.rootfs_base;
  } else if (cand.shape == Shapes::ShapeType::Electron ||
             cand.shape == Shapes::ShapeType::Gecko ||
             cand.shape == Shapes::ShapeType::Game) {
    // Prefer the backend's desktop rootfs (the one carrying the overlay packages)
    const auto& backend = Backend::GetActiveBackend();
    std::string desktopPath = Paths::GetDataDir() + "/RootFS/" + backend.defaultRootfsDesktop;
    if (std::filesystem::exists(desktopPath)) {
      rec.rootfs = desktopPath;
    } else {
      rec.rootfs = defaultRootfs;
    }
  } else {
    rec.rootfs = defaultRootfs;
  }

  // Default AppConfig
  rec.appconfig["EnableCodeCachingWIP"] = "1";
  rec.appconfig["CodeCacheScope"] = "home";
  rec.appconfig["DisableCmpBranchFusion"] = "0";
  rec.appconfig["ProfileStats"] = "0";

  // Desktop
  const auto& backend = Backend::GetActiveBackend();
  rec.desktop.enabled = cand.desktop_enabled;
  rec.desktop.name = cand.title + " (" + backend.archName + ")";
  rec.desktop.comment = cand.title + " under " + backend.displayName;
  rec.desktop.wmclass = cand.wmclass;
  rec.desktop.categories = cand.categories;
  rec.desktop.mimetypes = cand.mimetypes;
  rec.desktop.exec_field = cand.exec_field;

  if (cand.shape == Shapes::ShapeType::Pacman) {
    rec.source.kind = "pacman";
    rec.source.pacman_package = cand.pacman_package;
    rec.exe = cand.exe_path;
    rec.desktop.icon = cand.icon_path;
  } else if (!cand.dir.empty()) {
    rec.source.kind = "dir";
    rec.source.dir = cand.dir;
    rec.source.current = cand.version;
    if (!cand.version.empty()) {
      rec.source.versions.push_back(cand.version);
    }

    // If exe is inside dir, make relative
    if (cand.exe_path.rfind(cand.dir + "/", 0) == 0) {
      rec.exe = cand.exe_path.substr(cand.dir.size() + 1);
    } else {
      rec.exe = cand.exe_path;
    }

    if (!cand.icon_path.empty()) {
      if (cand.icon_path.rfind(cand.dir + "/", 0) == 0) {
        rec.desktop.icon = cand.icon_path.substr(cand.dir.size() + 1);
      } else {
        rec.desktop.icon = cand.icon_path;
      }
    }
  } else {
    rec.source.kind = "file";
    rec.exe = cand.exe_path;
  }

  return rec;
}

static std::string EscapeString(const std::string& s) {
  std::ostringstream o;
  for (char c : s) {
    if (c == '"') o << "\\\"";
    else if (c == '\\') o << "\\\\";
    else if (c == '\b') o << "\\b";
    else if (c == '\f') o << "\\f";
    else if (c == '\n') o << "\\n";
    else if (c == '\r') o << "\\r";
    else if (c == '\t') o << "\\t";
    else if (static_cast<unsigned char>(c) <= 0x1f) {
      o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
    } else {
      o << c;
    }
  }
  return o.str();
}

std::string EmitJson(const AppRecord& r) {
  std::ostringstream ss;
  ss << "{\n";
  ss << "  \"format\": " << r.format << ",\n";
  ss << "  \"name\": \"" << EscapeString(r.name) << "\",\n";
  ss << "  \"title\": \"" << EscapeString(r.title) << "\",\n";
  ss << "  \"shape\": \"" << EscapeString(r.shape) << "\",\n";

  // source
  ss << "  \"source\": {\n";
  ss << "    \"kind\": \"" << EscapeString(r.source.kind) << "\",\n";
  ss << "    \"dir\": \"" << EscapeString(r.source.dir) << "\",\n";
  ss << "    \"current\": \"" << EscapeString(r.source.current) << "\",\n";
  ss << "    \"versions\": [";
  for (size_t i = 0; i < r.source.versions.size(); ++i) {
    ss << (i > 0 ? ", " : "") << "\"" << EscapeString(r.source.versions[i]) << "\"";
  }
  ss << "],\n";
  ss << "    \"origin\": \"" << EscapeString(r.source.origin) << "\",\n";
  ss << "    \"pacman_package\": \"" << EscapeString(r.source.pacman_package) << "\"\n";
  ss << "  },\n";

  ss << "  \"exe\": \"" << EscapeString(r.exe) << "\",\n";
  ss << "  \"cwd\": \"" << EscapeString(r.cwd) << "\",\n";

  // args
  ss << "  \"args\": [";
  for (size_t i = 0; i < r.args.size(); ++i) {
    ss << (i > 0 ? ", " : "") << "\"" << EscapeString(r.args[i]) << "\"";
  }
  ss << "],\n";

  // env
  ss << "  \"env\": {\n";
  size_t idx = 0;
  for (const auto& [k, v] : r.env) {
    ss << "    \"" << EscapeString(k) << "\": \"" << EscapeString(v) << "\""
       << (++idx < r.env.size() ? ",\n" : "\n");
  }
  ss << "  },\n";

  ss << "  \"rootfs\": \"" << EscapeString(r.rootfs) << "\",\n";
  if (r.emulator.has_value()) {
    ss << "  \"emulator\": \"" << EscapeString(*r.emulator) << "\",\n";
  } else {
    ss << "  \"emulator\": null,\n";
  }

  // appconfig
  ss << "  \"appconfig\": {\n";
  idx = 0;
  for (const auto& [k, v] : r.appconfig) {
    ss << "    \"" << EscapeString(k) << "\": \"" << EscapeString(v) << "\""
       << (++idx < r.appconfig.size() ? ",\n" : "\n");
  }
  ss << "  },\n";

  // mangohud
  ss << "  \"mangohud\": {\n";
  ss << "    \"enabled\": " << (r.mangohud.enabled ? "true" : "false") << ",\n";
  ss << "    \"config\": \"" << EscapeString(r.mangohud.config) << "\"\n";
  ss << "  },\n";

  // desktop
  ss << "  \"desktop\": {\n";
  ss << "    \"enabled\": " << (r.desktop.enabled ? "true" : "false") << ",\n";
  ss << "    \"name\": \"" << EscapeString(r.desktop.name) << "\",\n";
  ss << "    \"comment\": \"" << EscapeString(r.desktop.comment) << "\",\n";
  ss << "    \"wmclass\": \"" << EscapeString(r.desktop.wmclass) << "\",\n";
  ss << "    \"icon\": \"" << EscapeString(r.desktop.icon) << "\",\n";
  ss << "    \"categories\": \"" << EscapeString(r.desktop.categories) << "\",\n";
  ss << "    \"mimetypes\": \"" << EscapeString(r.desktop.mimetypes) << "\",\n";
  ss << "    \"exec_field\": \"" << EscapeString(r.desktop.exec_field) << "\",\n";
  ss << "    \"startup_notify\": " << (r.desktop.startup_notify ? "true" : "false") << "\n";
  ss << "  },\n";

  // presets
  ss << "  \"presets\": {\n";
  idx = 0;
  for (const auto& [k, v] : r.presets) {
    ss << "    \"" << EscapeString(k) << "\": " << (v ? "true" : "false")
       << (++idx < r.presets.size() ? ",\n" : "\n");
  }
  ss << "  },\n";

  // health
  ss << "  \"health\": {\n";
  ss << "    \"when\": \"" << EscapeString(r.health.when) << "\",\n";
  ss << "    \"mode\": \"" << EscapeString(r.health.mode) << "\",\n";
  ss << "    \"status\": \"" << EscapeString(r.health.status) << "\",\n";
  ss << "    \"unimplemented\": [";
  for (size_t i = 0; i < r.health.unimplemented.size(); ++i) {
    ss << (i > 0 ? ", " : "") << "\"" << EscapeString(r.health.unimplemented[i]) << "\"";
  }
  ss << "],\n";
  ss << "    \"notes\": [";
  for (size_t i = 0; i < r.health.notes.size(); ++i) {
    ss << (i > 0 ? ", " : "") << "\"" << EscapeString(r.health.notes[i]) << "\"";
  }
  ss << "]\n";
  ss << "  },\n";

  // generated
  ss << "  \"generated\": {\n";
  idx = 0;
  for (const auto& [k, v] : r.generated) {
    ss << "    \"" << EscapeString(k) << "\": \"" << EscapeString(v) << "\""
       << (++idx < r.generated.size() ? ",\n" : "\n");
  }
  ss << "  }\n";

  ss << "}\n";
  return ss.str();
}

static std::string GetStringProp(const json_t* obj, const char* name, const std::string& def = "") {
  const json_t* prop = json_getProperty(obj, name);
  if (!prop) return def;
  const char* val = json_getValue(prop);
  return val ? std::string(val) : def;
}

static bool GetBoolProp(const json_t* obj, const char* name, bool def = false) {
  const json_t* prop = json_getProperty(obj, name);
  if (!prop) return def;
  const char* val = json_getValue(prop);
  if (!val) return def;
  return std::string(val) == "true" || std::string(val) == "1";
}

static int GetIntProp(const json_t* obj, const char* name, int def = 0) {
  const json_t* prop = json_getProperty(obj, name);
  if (!prop) return def;
  const char* val = json_getValue(prop);
  if (!val) return def;
  try {
    return std::stoi(val);
  } catch (...) {
    return def;
  }
}

std::optional<AppRecord> LoadRecord(const std::string& name) {
  if (!IsValidAppName(name)) return std::nullopt;
  std::string path = Paths::GetRecordDir() + "/" + name + ".json";
  std::ifstream f(path);
  if (!f.is_open()) return std::nullopt;

  std::stringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();
  if (content.empty()) return std::nullopt;

  std::vector<char> buffer(content.begin(), content.end());
  buffer.push_back('\0');

  json_t mem[512];
  const json_t* root = json_create(buffer.data(), mem, 512);
  if (!root) return std::nullopt;

  AppRecord r;
  r.format = GetIntProp(root, "format", 1);
  r.name = GetStringProp(root, "name", name);
  r.title = GetStringProp(root, "title", r.name);
  r.shape = GetStringProp(root, "shape", "unknown");

  const json_t* src = json_getProperty(root, "source");
  if (src) {
    r.source.kind = GetStringProp(src, "kind", "dir");
    r.source.dir = GetStringProp(src, "dir", "");
    r.source.current = GetStringProp(src, "current", "");
    r.source.origin = GetStringProp(src, "origin", "");
    r.source.pacman_package = GetStringProp(src, "pacman_package", "");
    const json_t* vers = json_getProperty(src, "versions");
    if (vers) {
      for (const json_t* it = json_getChild(vers); it; it = json_getSibling(it)) {
        const char* val = json_getValue(it);
        if (val) r.source.versions.push_back(val);
      }
    }
  }

  r.exe = GetStringProp(root, "exe", "");
  r.cwd = GetStringProp(root, "cwd", "launcher");

  const json_t* args = json_getProperty(root, "args");
  if (args) {
    for (const json_t* it = json_getChild(args); it; it = json_getSibling(it)) {
      const char* val = json_getValue(it);
      if (val) r.args.push_back(val);
    }
  }

  const json_t* env = json_getProperty(root, "env");
  if (env) {
    for (const json_t* it = json_getChild(env); it; it = json_getSibling(it)) {
      const char* k = json_getName(it);
      const char* v = json_getValue(it);
      if (k && v) r.env[k] = v;
    }
  }

  r.rootfs = GetStringProp(root, "rootfs", "");
  const json_t* emu = json_getProperty(root, "emulator");
  if (emu && json_getType(emu) == JSON_TEXT) {
    const char* val = json_getValue(emu);
    if (val && std::string(val) != "null") {
      r.emulator = std::string(val);
    }
  }

  const json_t* ac = json_getProperty(root, "appconfig");
  if (ac) {
    for (const json_t* it = json_getChild(ac); it; it = json_getSibling(it)) {
      const char* k = json_getName(it);
      const char* v = json_getValue(it);
      if (k && v) r.appconfig[k] = v;
    }
  }

  const json_t* mh = json_getProperty(root, "mangohud");
  if (mh) {
    r.mangohud.enabled = GetBoolProp(mh, "enabled", false);
    r.mangohud.config = GetStringProp(mh, "config", "");
  }

  const json_t* dt = json_getProperty(root, "desktop");
  if (dt) {
    r.desktop.enabled = GetBoolProp(dt, "enabled", true);
    r.desktop.name = GetStringProp(dt, "name", "");
    r.desktop.comment = GetStringProp(dt, "comment", "");
    r.desktop.wmclass = GetStringProp(dt, "wmclass", "");
    r.desktop.icon = GetStringProp(dt, "icon", "");
    r.desktop.categories = GetStringProp(dt, "categories", "");
    r.desktop.mimetypes = GetStringProp(dt, "mimetypes", "");
    r.desktop.exec_field = GetStringProp(dt, "exec_field", "%F");
    r.desktop.startup_notify = GetBoolProp(dt, "startup_notify", false);
  }

  const json_t* ps = json_getProperty(root, "presets");
  if (ps) {
    for (const json_t* it = json_getChild(ps); it; it = json_getSibling(it)) {
      const char* k = json_getName(it);
      const char* v = json_getValue(it);
      if (k && v) r.presets[k] = (std::string(v) == "true" || std::string(v) == "1");
    }
  }

  const json_t* hl = json_getProperty(root, "health");
  if (hl) {
    r.health.when = GetStringProp(hl, "when", "");
    r.health.mode = GetStringProp(hl, "mode", "");
    r.health.status = GetStringProp(hl, "status", "");
    const json_t* unimplem = json_getProperty(hl, "unimplemented");
    if (unimplem) {
      for (const json_t* it = json_getChild(unimplem); it; it = json_getSibling(it)) {
        const char* val = json_getValue(it);
        if (val) r.health.unimplemented.push_back(val);
      }
    }
    const json_t* notes = json_getProperty(hl, "notes");
    if (notes) {
      for (const json_t* it = json_getChild(notes); it; it = json_getSibling(it)) {
        const char* val = json_getValue(it);
        if (val) r.health.notes.push_back(val);
      }
    }
  }

  const json_t* gen = json_getProperty(root, "generated");
  if (gen) {
    for (const json_t* it = json_getChild(gen); it; it = json_getSibling(it)) {
      const char* k = json_getName(it);
      const char* v = json_getValue(it);
      if (k && v) r.generated[k] = v;
    }
  }

  return r;
}

bool SaveRecord(const AppRecord& record) {
  if (!IsValidAppName(record.name)) return false;
  std::string dir = Paths::GetRecordDir();
  std::error_code ec;
  fs::create_directories(dir, ec);

  std::string targetPath = dir + "/" + record.name + ".json";
  std::string tmpPath = targetPath + ".tmp." + std::to_string(::getpid());

  std::string jsonStr = EmitJson(record);

  {
    std::ofstream f(tmpPath, std::ios::trunc);
    if (!f.is_open()) return false;
    f << jsonStr;
    f.flush();
    if (!f.good()) {
      fs::remove(tmpPath, ec);
      return false;
    }
  }

  fs::rename(tmpPath, targetPath, ec);
  return !ec;
}

std::vector<AppRecord> ListRecords() {
  std::vector<AppRecord> res;
  std::string dir = Paths::GetRecordDir();
  std::error_code ec;
  if (!fs::exists(dir, ec)) return res;

  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".json") {
      std::string stem = entry.path().stem().string();
      auto rec = LoadRecord(stem);
      if (rec) {
        res.push_back(*rec);
      }
    }
  }
  return res;
}

bool DeleteRecord(const std::string& name) {
  if (!IsValidAppName(name)) return false;
  std::string path = Paths::GetRecordDir() + "/" + name + ".json";
  std::error_code ec;
  return fs::remove(path, ec);
}

Settings LoadSettings() {
  Settings s;
  std::string path = Paths::GetSettingsPath();
  std::ifstream f(path);
  if (!f.is_open()) return s;

  std::stringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();
  if (content.empty()) return s;

  std::vector<char> buf(content.begin(), content.end());
  buf.push_back('\0');

  json_t mem[128];
  const json_t* root = json_create(buf.data(), mem, 128);
  if (!root) return s;

  s.apps_dir = GetStringProp(root, "apps_dir", "");
  s.last_theme = GetStringProp(root, "last_theme", "");

  const json_t* dirs = json_getProperty(root, "scan_dirs");
  if (dirs) {
    for (const json_t* it = json_getChild(dirs); it; it = json_getSibling(it)) {
      const char* val = json_getValue(it);
      if (val) s.scan_dirs.push_back(val);
    }
  }

  return s;
}

bool SaveSettings(const Settings& settings) {
  std::string path = Paths::GetSettingsPath();
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);

  std::ostringstream ss;
  ss << "{\n";
  ss << "  \"apps_dir\": \"" << EscapeString(settings.apps_dir) << "\",\n";
  ss << "  \"last_theme\": \"" << EscapeString(settings.last_theme) << "\",\n";
  ss << "  \"scan_dirs\": [";
  for (size_t i = 0; i < settings.scan_dirs.size(); ++i) {
    ss << (i > 0 ? ", " : "") << "\"" << EscapeString(settings.scan_dirs[i]) << "\"";
  }
  ss << "]\n";
  ss << "}\n";

  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp);
    if (!f.is_open()) return false;
    f << ss.str();
  }
  fs::rename(tmp, path, ec);
  return !ec;
}

} // namespace Sleeve::Record
