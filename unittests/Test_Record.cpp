// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "Record.h"
#include "Paths.h"
#include "TestEnv.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Sleeve;

TEST_CASE("Record: Serialization and deserialization round trip", "[Record]") {
  // Records are written under $HOME; without this the suite writes into the records the
  // user actually launches apps with.
  SleeveTest::ScopedTestHome home("record-roundtrip");

  Record::AppRecord original;
  original.format = 1;
  original.name = "roundtrip-test";
  original.title = "Roundtrip Test App";
  original.shape = "electron";
  original.source.kind = "dir";
  original.source.dir = "/tmp/roundtrip";
  original.source.current = "1.0.0";
  original.source.versions = {"1.0.0", "1.1.0"};
  original.exe = "bin/test";
  original.cwd = "install";
  original.args = {"--no-sandbox", "--enable-feature"};
  original.env["FOO"] = "BAR";
  original.rootfs = "/tmp/rootfs";
  original.emulator = "/tmp/dev-powerarm";
  original.appconfig["EnableCodeCachingWIP"] = "1";
  original.appconfig["CodeCacheScope"] = "all";
  original.mangohud.enabled = true;
  original.mangohud.config = "fps,fex_stats";
  original.desktop.enabled = true;
  original.desktop.name = "Test App";
  original.desktop.wmclass = "TestApp";
  original.desktop.categories = "Utility;";
  original.desktop.startup_notify = true;
  original.presets["electron-no-sandbox"] = true;
  original.health.when = "2026-09-21T00:00:00";
  original.health.mode = "--version";
  original.health.status = "ok";
  original.health.unimplemented = {"0xd53bd0a0"};
  original.health.notes = {"Ran normally"};
  original.generated["/tmp/test"] = "sha256:abc";

  std::string json = Record::EmitJson(original);
  REQUIRE_FALSE(json.empty());

  // Save to disk and load back
  REQUIRE(Record::SaveRecord(original));

  auto loaded = Record::LoadRecord("roundtrip-test");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->name == original.name);
  REQUIRE(loaded->title == original.title);
  REQUIRE(loaded->shape == original.shape);
  REQUIRE(loaded->source.kind == original.source.kind);
  REQUIRE(loaded->source.dir == original.source.dir);
  REQUIRE(loaded->source.current == original.source.current);
  REQUIRE(loaded->source.versions == original.source.versions);
  REQUIRE(loaded->exe == original.exe);
  REQUIRE(loaded->cwd == original.cwd);
  REQUIRE(loaded->args == original.args);
  REQUIRE(loaded->env == original.env);
  REQUIRE(loaded->rootfs == original.rootfs);
  REQUIRE(loaded->emulator == original.emulator);
  REQUIRE(loaded->appconfig == original.appconfig);
  REQUIRE(loaded->mangohud.enabled == original.mangohud.enabled);
  REQUIRE(loaded->mangohud.config == original.mangohud.config);
  REQUIRE(loaded->desktop.enabled == original.desktop.enabled);
  REQUIRE(loaded->desktop.name == original.desktop.name);
  REQUIRE(loaded->desktop.wmclass == original.desktop.wmclass);
  REQUIRE(loaded->desktop.categories == original.desktop.categories);
  REQUIRE(loaded->desktop.startup_notify == original.desktop.startup_notify);
  REQUIRE(loaded->presets == original.presets);
  REQUIRE(loaded->health.status == original.health.status);
  REQUIRE(loaded->health.unimplemented == original.health.unimplemented);
  REQUIRE(loaded->health.notes == original.health.notes);
  REQUIRE(loaded->generated == original.generated);

  // Clean up
  Record::DeleteRecord("roundtrip-test");
}

TEST_CASE("Record: startup_notify defaults to false for records that predate it", "[Record]") {
  SleeveTest::ScopedTestHome home("record-startup-notify-default");

  Record::AppRecord rec;
  rec.name = "legacy";
  rec.title = "Legacy";
  rec.exe = "/usr/bin/true";
  REQUIRE(rec.desktop.startup_notify == false);
  REQUIRE(Record::SaveRecord(rec));

  // Strip the key the way a record written by an older sleeve would not have it.
  std::string path = Paths::GetRecordDir() + "/legacy.json";
  std::string content;
  {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    content = ss.str();
  }
  auto pos = content.find("\"startup_notify\"");
  REQUIRE(pos != std::string::npos);
  auto lineEnd = content.find('\n', pos);
  auto lineStart = content.rfind('\n', pos) + 1;
  content.erase(lineStart, lineEnd - lineStart + 1);
  // Remove the now-trailing comma on the previous key.
  auto comma = content.rfind(",\n", lineStart);
  if (comma != std::string::npos) content.erase(comma, 1);
  {
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  auto loaded = Record::LoadRecord("legacy");
  REQUIRE(loaded.has_value());
  CHECK(loaded->desktop.startup_notify == false);
}

TEST_CASE("Record: names that could escape the record directory are refused", "[Record]") {
  SleeveTest::ScopedTestHome home("record-name-validation");

  CHECK(Record::IsValidAppName("code"));
  CHECK(Record::IsValidAppName("antigravity-ide"));
  CHECK(Record::IsValidAppName("v4l-utils"));
  CHECK(Record::IsValidAppName("firefox.dev"));

  CHECK_FALSE(Record::IsValidAppName(""));
  CHECK_FALSE(Record::IsValidAppName(".."));
  CHECK_FALSE(Record::IsValidAppName("../../etc/passwd"));
  CHECK_FALSE(Record::IsValidAppName("has space"));
  CHECK_FALSE(Record::IsValidAppName("sub/dir"));
  CHECK_FALSE(Record::IsValidAppName(".hidden"));
  CHECK_FALSE(Record::IsValidAppName("-dash"));

  Record::AppRecord bad;
  bad.name = "../escape";
  bad.exe = "/usr/bin/true";
  CHECK_FALSE(Record::SaveRecord(bad));
  CHECK_FALSE(Record::LoadRecord("../escape").has_value());
  CHECK_FALSE(std::filesystem::exists(Paths::GetRecordDir() + "/../escape.json"));
}

TEST_CASE("Record: Editing args as one line keeps arguments whole", "[Record]") {
  // The options screen joins args into a line and splits them back. Splitting on
  // whitespace turned one argument into two every time the screen was opened, without
  // the user changing anything.
  std::vector<std::string> args = {
    "--no-sandbox",
    "$HOME/.mozilla/firefox-arm64",
    "--user-data-dir=/home/x/My Apps/data",
    "--title=say \"hi\"",
  };

  std::string line = Record::JoinArgsForEditing(args);
  CHECK(Record::SplitArgsFromEditing(line) == args);

  SECTION("a plain line still splits the obvious way") {
    auto parsed = Record::SplitArgsFromEditing("  --a   --b=c  ");
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0] == "--a");
    CHECK(parsed[1] == "--b=c");
  }

  SECTION("single quotes group too") {
    auto parsed = Record::SplitArgsFromEditing("--dir='My Apps'");
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0] == "--dir=My Apps");
  }

  SECTION("an empty line is no arguments, not one empty argument") {
    CHECK(Record::SplitArgsFromEditing("").empty());
    CHECK(Record::SplitArgsFromEditing("   ").empty());
  }
}

TEST_CASE("Record: Editing env as one line keeps values whole", "[Record]") {
  std::map<std::string, std::string> env = {
    {"MOZ_ENABLE_WAYLAND", "1"},
    {"EXTRA_PATH", "/home/x/My Apps"},
    {"OPTS", "--a=1 --b=2"},
  };

  std::string line = Record::JoinEnvForEditing(env);
  CHECK(Record::SplitEnvFromEditing(line) == env);

  SECTION("a value may contain an equals sign") {
    auto parsed = Record::SplitEnvFromEditing("K=a=b");
    REQUIRE(parsed.count("K") == 1);
    CHECK(parsed.at("K") == "a=b");
  }

  SECTION("a word with no equals sign is not an assignment") {
    CHECK(Record::SplitEnvFromEditing("nonsense").empty());
    CHECK(Record::SplitEnvFromEditing("=novalue").empty());
  }
}
