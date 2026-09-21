// SPDX-License-Identifier: MIT
#include "AppConfigWriter.h"
#include "Paths.h"
#include "FileWriter.h"
#include "Diff.h"

#include <unordered_set>
#include <map>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <tiny-json.h>

namespace Sleeve::AppConfigWriter {

namespace fs = std::filesystem;

static const std::unordered_set<std::string> KnownOptions = {
#define OPT_BASE(type, group, enum, json, default) #json,
#define OPT_BOOL(group, enum, json, default) #json,
#define OPT_UINT8(group, enum, json, default) #json,
#define OPT_INT32(group, enum, json, default) #json,
#define OPT_UINT32(group, enum, json, default) #json,
#define OPT_UINT64(group, enum, json, default) #json,
#define OPT_STR(group, enum, json, default) #json,
#define OPT_STRARRAY(group, enum, json, default) #json,
#define OPT_STRENUM(group, enum, json, default) #json,
#include "ConfigValues.inl"
#undef OPT_BASE
#undef OPT_BOOL
#undef OPT_UINT8
#undef OPT_INT32
#undef OPT_UINT32
#undef OPT_UINT64
#undef OPT_STR
#undef OPT_STRARRAY
#undef OPT_STRENUM
};

bool IsValidConfigOption(std::string_view name) {
  return KnownOptions.count(std::string(name)) > 0;
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
      o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
    } else {
      o << c;
    }
  }
  return o.str();
}

static void SerializeJsonValue(std::ostringstream& ss, const json_t* item, int indent = 0) {
  if (!item) return;
  std::string ind(indent * 2, ' ');
  switch (json_getType(item)) {
    case JSON_OBJ: {
      ss << "{\n";
      for (const json_t* child = json_getChild(item); child; child = json_getSibling(child)) {
        ss << ind << "  \"" << EscapeString(json_getName(child) ? json_getName(child) : "") << "\": ";
        SerializeJsonValue(ss, child, indent + 1);
        if (json_getSibling(child)) ss << ",";
        ss << "\n";
      }
      ss << ind << "}";
      break;
    }
    case JSON_ARRAY: {
      ss << "[";
      bool first = true;
      for (const json_t* child = json_getChild(item); child; child = json_getSibling(child)) {
        if (!first) ss << ", ";
        first = false;
        SerializeJsonValue(ss, child, indent + 1);
      }
      ss << "]";
      break;
    }
    case JSON_TEXT:
      ss << "\"" << EscapeString(json_getValue(item) ? json_getValue(item) : "") << "\"";
      break;
    case JSON_BOOLEAN:
    case JSON_INTEGER:
    case JSON_REAL:
      ss << (json_getValue(item) ? json_getValue(item) : "0");
      break;
    case JSON_NULL:
      ss << "null";
      break;
  }
}

std::string GenerateAppConfigContent(const Record::AppRecord& record, const std::string& existingContent) {
  std::map<std::string, std::string> configPairs;
  std::vector<std::pair<std::string, std::string>> otherSections; // name, serialized json

  if (!existingContent.empty()) {
    std::vector<char> buffer(existingContent.begin(), existingContent.end());
    buffer.push_back('\0');
    std::vector<json_t> mem(2048);
    const json_t* root = json_create(buffer.data(), mem.data(), mem.size());
    if (root && json_getType(root) == JSON_OBJ) {
      for (const json_t* sec = json_getChild(root); sec; sec = json_getSibling(sec)) {
        const char* secName = json_getName(sec);
        if (!secName) continue;
        if (std::string(secName) == "Config") {
          if (json_getType(sec) == JSON_OBJ) {
            for (const json_t* item = json_getChild(sec); item; item = json_getSibling(item)) {
              const char* k = json_getName(item);
              const char* v = json_getValue(item);
              if (k && v) {
                configPairs[k] = v;
              }
            }
          }
        } else {
          // Other top-level section (e.g. ThunksDB, AppOverrides)
          std::ostringstream ss;
          SerializeJsonValue(ss, sec, 1);
          otherSections.push_back({secName, ss.str()});
        }
      }
    }
  }

  // Apply managed keys
  if (!record.rootfs.empty()) {
    configPairs["RootFS"] = record.rootfs;
  }
  for (const auto& [k, v] : record.appconfig) {
    configPairs[k] = v;
  }

  // Format the JSON
  std::ostringstream out;
  out << "{\n";
  out << "  \"Config\": {\n";
  size_t idx = 0;
  for (const auto& [k, v] : configPairs) {
    out << "    \"" << EscapeString(k) << "\": \"" << EscapeString(v) << "\"";
    if (++idx < configPairs.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  }";

  for (const auto& [name, serialized] : otherSections) {
    out << ",\n";
    out << "  \"" << EscapeString(name) << "\": " << serialized;
  }

  out << "\n}\n";
  return out.str();
}

bool WriteAppConfig(Record::AppRecord& record, bool dryRun, std::string* outDiff) {
  std::string targetPath = Paths::GetAppConfigDir() + "/" + record.name + ".json";
  std::string existingContent;

  std::ifstream in(targetPath);
  if (in.is_open()) {
    std::stringstream ss;
    ss << in.rdbuf();
    existingContent = ss.str();
  }

  std::string newContent = GenerateAppConfigContent(record, existingContent);

  if (outDiff) {
    *outDiff = Diff::UnifiedDiff(existingContent, newContent, targetPath + " (current)", targetPath + " (new)");
  }

  if (dryRun) {
    return true;
  }

  if (!FileWriter::AtomicWrite(targetPath, newContent, 0644)) {
    return false;
  }

  record.generated[targetPath] = FileWriter::ComputeSha256(newContent);
  return true;
}

std::string GenerateUserConfigRootFS(const std::string& newRootfs, const std::string& existingContent) {
  std::map<std::string, std::string> configPairs;
  std::vector<std::pair<std::string, std::string>> otherSections;

  if (!existingContent.empty()) {
    std::vector<char> buffer(existingContent.begin(), existingContent.end());
    buffer.push_back('\0');
    std::vector<json_t> mem(2048);
    const json_t* root = json_create(buffer.data(), mem.data(), mem.size());
    if (root && json_getType(root) == JSON_OBJ) {
      for (const json_t* sec = json_getChild(root); sec; sec = json_getSibling(sec)) {
        const char* secName = json_getName(sec);
        if (!secName) continue;
        if (std::string(secName) == "Config") {
          if (json_getType(sec) == JSON_OBJ) {
            for (const json_t* item = json_getChild(sec); item; item = json_getSibling(item)) {
              const char* k = json_getName(item);
              const char* v = json_getValue(item);
              if (k && v) {
                configPairs[k] = v;
              }
            }
          }
        } else {
          std::ostringstream ss;
          SerializeJsonValue(ss, sec, 1);
          otherSections.push_back({secName, ss.str()});
        }
      }
    }
  }

  configPairs["RootFS"] = newRootfs;

  std::ostringstream out;
  out << "{\n";
  out << "  \"Config\": {\n";
  size_t idx = 0;
  for (const auto& [k, v] : configPairs) {
    out << "    \"" << EscapeString(k) << "\": \"" << EscapeString(v) << "\"";
    if (++idx < configPairs.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  }";

  for (const auto& [name, serialized] : otherSections) {
    out << ",\n";
    out << "  \"" << EscapeString(name) << "\": " << serialized;
  }

  out << "\n}\n";
  return out.str();
}

bool SetUserConfigRootFS(const std::string& newRootfs, bool dryRun, std::string* outDiff) {
  std::string targetPath = Paths::GetUserConfigPath();
  std::string existingContent;

  std::ifstream in(targetPath);
  if (in.is_open()) {
    std::stringstream ss;
    ss << in.rdbuf();
    existingContent = ss.str();
  }

  std::string newContent = GenerateUserConfigRootFS(newRootfs, existingContent);

  if (outDiff) {
    *outDiff = Diff::UnifiedDiff(existingContent, newContent, targetPath + " (current)", targetPath + " (new)");
  }

  if (dryRun) {
    return true;
  }

  return FileWriter::AtomicWrite(targetPath, newContent, 0644);
}

} // namespace Sleeve::AppConfigWriter
