// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "Shapes.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace Sleeve::Shapes;

TEST_CASE("Shapes: Electron detection", "[sleeve][shapes]") {
  std::string tmpDir = "/tmp/sleeve_test_shapes_electron";
  fs::create_directories(tmpDir + "/resources/app/resources/linux");

  // Create product.json
  {
    std::ofstream f(tmpDir + "/resources/app/product.json");
    f << "{\n"
      << "  \"nameShort\": \"Antigravity IDE\",\n"
      << "  \"applicationName\": \"antigravity-ide\",\n"
      << "  \"ideVersion\": \"2.5.5\"\n"
      << "}\n";
  }

  // Create dummy executable
  std::string exe = tmpDir + "/antigravity-ide";
  {
    std::ofstream f(exe);
    f << "#!/bin/sh\n";
  }

  auto cand = DetectDirectoryShape(tmpDir, {exe});
  REQUIRE(cand.has_value());
  CHECK(cand->shape == ShapeType::Electron);
  CHECK(cand->name == "antigravity-ide");
  CHECK(cand->title == "Antigravity IDE");
  CHECK(cand->wmclass == "Antigravity IDE");
  CHECK(cand->version == "2.5.5");
  REQUIRE(!cand->default_args.empty());
  CHECK(cand->default_args[0] == "--no-sandbox");
  CHECK(cand->default_env.count("ELECTRON_OZONE_PLATFORM_HINT") == 1);

  fs::remove_all(tmpDir);
}

TEST_CASE("Shapes: Gecko detection", "[sleeve][shapes]") {
  std::string tmpDir = "/tmp/sleeve_test_shapes_gecko";
  fs::create_directories(tmpDir);

  // Create application.ini
  {
    std::ofstream f(tmpDir + "/application.ini");
    f << "[App]\n"
      << "Name=Firefox\n"
      << "Version=156.0\n";
  }

  std::string exe = tmpDir + "/firefox";
  {
    std::ofstream f(exe);
    f << "#!/bin/sh\n";
  }

  auto cand = DetectDirectoryShape(tmpDir, {exe});
  REQUIRE(cand.has_value());
  CHECK(cand->shape == ShapeType::Gecko);
  CHECK(cand->name == "firefox");
  CHECK(cand->title == "Firefox");
  CHECK(cand->version == "156.0");
  REQUIRE(cand->default_args.size() >= 2);
  CHECK(cand->default_args[0] == "--profile");
  CHECK(cand->default_env.count("MOZ_ENABLE_WAYLAND") == 1);

  fs::remove_all(tmpDir);
}

TEST_CASE("Shapes: Game detection", "[sleeve][shapes]") {
  std::string tmpDir = "/tmp/sleeve_test_shapes_game";
  fs::create_directories(tmpDir + "/data");
  fs::create_directories(tmpDir + "/bin/arm64");

  std::string exe = tmpDir + "/bin/arm64/factorio";
  {
    std::ofstream f(exe);
    f << "#!/bin/sh\n";
  }

  auto cand = DetectDirectoryShape(tmpDir, {exe});
  REQUIRE(cand.has_value());
  CHECK(cand->shape == ShapeType::Game);
  CHECK(cand->cwd == "install");
  CHECK(cand->categories == "Game;");

  fs::remove_all(tmpDir);
}
