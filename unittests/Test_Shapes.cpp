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

TEST_CASE("Shapes: A pacman package only claims the desktop files it owns", "[sleeve][shapes]") {
  // `gnupg` claimed org.gnupg.pinentry-qt.desktop because the name matched. pinentry owns
  // it, and the two packages produced two records for one binary.
  std::string overlay = "/tmp/sleeve_test_shapes_pacman";
  fs::remove_all(overlay);
  fs::create_directories(overlay + "/usr/share/applications");
  fs::create_directories(overlay + "/usr/bin");

  {
    std::ofstream f(overlay + "/usr/share/applications/org.gnupg.pinentry-qt.desktop");
    f << "[Desktop Entry]\n"
      << "Name=Pinentry\n"
      << "Exec=/usr/bin/pinentry-qt\n"
      << "Categories=Utility;\n";
  }
  { std::ofstream f(overlay + "/usr/bin/pinentry-qt"); f << "x"; }

  std::string pacmanLocal = overlay + "/var/lib/pacman/local";
  fs::create_directories(pacmanLocal + "/pinentry-1.3.3-1");
  fs::create_directories(pacmanLocal + "/gnupg-2.4.9-3");

  {
    std::ofstream f(pacmanLocal + "/pinentry-1.3.3-1/desc");
    f << "%NAME%\npinentry\n\n%VERSION%\n1.3.3-1\n";
  }
  {
    std::ofstream f(pacmanLocal + "/pinentry-1.3.3-1/files");
    f << "%FILES%\nusr/bin/pinentry-qt\nusr/share/applications/org.gnupg.pinentry-qt.desktop\n";
  }
  {
    std::ofstream f(pacmanLocal + "/gnupg-2.4.9-3/desc");
    f << "%NAME%\ngnupg\n\n%VERSION%\n2.4.9-3\n";
  }
  {
    std::ofstream f(pacmanLocal + "/gnupg-2.4.9-3/files");
    f << "%FILES%\nusr/bin/gpg\nusr/share/man/man1/gpg.1.gz\n";
  }

  auto owner = DetectPacmanPackage(overlay, pacmanLocal + "/pinentry-1.3.3-1/desc");
  REQUIRE(owner.has_value());
  CHECK(owner->name == "pinentry");
  CHECK(owner->exe_path == overlay + "/usr/bin/pinentry-qt");

  auto stranger = DetectPacmanPackage(overlay, pacmanLocal + "/gnupg-2.4.9-3/desc");
  CHECK_FALSE(stranger.has_value());

  SECTION("a package shipping several entries is named after itself") {
    fs::create_directories(pacmanLocal + "/avahi-1-1");
    for (const char* name : {"avahi-discover", "bssh", "bvnc"}) {
      std::ofstream f(overlay + "/usr/share/applications/" + name + ".desktop");
      f << "[Desktop Entry]\nName=" << name << "\nExec=/usr/bin/" << name << "\n";
      std::ofstream b(overlay + "/usr/bin/" + name);
      b << "x";
    }
    {
      std::ofstream f(pacmanLocal + "/avahi-1-1/desc");
      f << "%NAME%\navahi\n\n%VERSION%\n1-1\n";
    }
    {
      std::ofstream f(pacmanLocal + "/avahi-1-1/files");
      f << "%FILES%\nusr/share/applications/avahi-discover.desktop\n"
        << "usr/share/applications/bssh.desktop\nusr/share/applications/bvnc.desktop\n";
    }
    auto avahi = DetectPacmanPackage(overlay, pacmanLocal + "/avahi-1-1/desc");
    REQUIRE(avahi.has_value());
    CHECK(avahi->exe_path == overlay + "/usr/bin/avahi-discover");
  }

  fs::remove_all(overlay);
}

TEST_CASE("Shapes: Gecko profile argument is stored unquoted", "[sleeve][shapes]") {
  // Quoting is the generator's job. An argument carrying its own quotes either reaches
  // argv with them attached or gets quoted twice.
  std::string tmpDir = "/tmp/sleeve_test_shapes_gecko_args";
  fs::remove_all(tmpDir);
  fs::create_directories(tmpDir);
  {
    std::ofstream f(tmpDir + "/application.ini");
    f << "[App]\nName=Firefox\nVersion=156.0\n";
  }
  std::string exe = tmpDir + "/firefox";
  { std::ofstream f(exe); f << "#!/bin/sh\n"; }

  auto cand = DetectDirectoryShape(tmpDir, {exe});
  REQUIRE(cand.has_value());
  REQUIRE(cand->default_args.size() == 2);
  CHECK(cand->default_args[0] == "--profile");
  CHECK(cand->default_args[1] == "$HOME/.mozilla/firefox-arm64");
  CHECK(cand->default_args[1].find('"') == std::string::npos);

  fs::remove_all(tmpDir);
}
