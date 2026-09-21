// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "Record.h"
#include "Paths.h"

#include <filesystem>

using namespace Sleeve;

TEST_CASE("Record: Serialization and deserialization round trip", "[Record]") {
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
  REQUIRE(loaded->presets == original.presets);
  REQUIRE(loaded->health.status == original.health.status);
  REQUIRE(loaded->health.unimplemented == original.health.unimplemented);
  REQUIRE(loaded->health.notes == original.health.notes);
  REQUIRE(loaded->generated == original.generated);

  // Clean up
  Record::DeleteRecord("roundtrip-test");
}
