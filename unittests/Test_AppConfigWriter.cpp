// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "AppConfigWriter.h"
#include "Record.h"

using namespace Sleeve;

TEST_CASE("AppConfigWriter: Known option validation", "[AppConfigWriter]") {
  REQUIRE(AppConfigWriter::IsValidConfigOption("EnableCodeCachingWIP"));
  REQUIRE(AppConfigWriter::IsValidConfigOption("RootFS"));
  REQUIRE(AppConfigWriter::IsValidConfigOption("DisableCmpBranchFusion"));
  REQUIRE(AppConfigWriter::IsValidConfigOption("ProfileStats"));
  REQUIRE(AppConfigWriter::IsValidConfigOption("CodeCacheScope"));
  REQUIRE(AppConfigWriter::IsValidConfigOption("MaxInst"));

  REQUIRE_FALSE(AppConfigWriter::IsValidConfigOption("ThisOptionDoesNotExist"));
  REQUIRE_FALSE(AppConfigWriter::IsValidConfigOption("FooBarBaz"));
}

TEST_CASE("AppConfigWriter: GenerateAppConfigContent creates valid config", "[AppConfigWriter]") {
  Record::AppRecord rec;
  rec.name = "testapp";
  rec.rootfs = "/path/to/rootfs";
  rec.appconfig["EnableCodeCachingWIP"] = "1";
  rec.appconfig["CodeCacheScope"] = "home";
  rec.appconfig["DisableCmpBranchFusion"] = "0";
  rec.appconfig["ProfileStats"] = "1";

  std::string json = AppConfigWriter::GenerateAppConfigContent(rec);
  REQUIRE(json.find("\"Config\":") != std::string::npos);
  REQUIRE(json.find("\"RootFS\": \"/path/to/rootfs\"") != std::string::npos);
  REQUIRE(json.find("\"EnableCodeCachingWIP\": \"1\"") != std::string::npos);
  REQUIRE(json.find("\"CodeCacheScope\": \"home\"") != std::string::npos);
  REQUIRE(json.find("\"DisableCmpBranchFusion\": \"0\"") != std::string::npos);
  REQUIRE(json.find("\"ProfileStats\": \"1\"") != std::string::npos);
}

TEST_CASE("AppConfigWriter: Merging preserves unmanaged keys and extra sections", "[AppConfigWriter]") {
  std::string existingJson = R"({
  "Config": {
    "MaxInst": "2000",
    "SmallTSCScale": "0",
    "RootFS": "/old/rootfs"
  },
  "ThunksDB": {
    "libGL": 1
  },
  "AppOverrides": {
    "some_pattern": {
      "Option": "1"
    }
  }
})";

  Record::AppRecord rec;
  rec.name = "testapp";
  rec.rootfs = "/new/rootfs";
  rec.appconfig["EnableCodeCachingWIP"] = "1";
  rec.appconfig["CodeCacheScope"] = "rootfs";

  std::string merged = AppConfigWriter::GenerateAppConfigContent(rec, existingJson);

  // Managed keys updated
  REQUIRE(merged.find("\"RootFS\": \"/new/rootfs\"") != std::string::npos);
  REQUIRE(merged.find("\"EnableCodeCachingWIP\": \"1\"") != std::string::npos);
  REQUIRE(merged.find("\"CodeCacheScope\": \"rootfs\"") != std::string::npos);

  // Unmanaged keys preserved in Config
  REQUIRE(merged.find("\"MaxInst\": \"2000\"") != std::string::npos);
  REQUIRE(merged.find("\"SmallTSCScale\": \"0\"") != std::string::npos);

  // Extra sections preserved
  REQUIRE(merged.find("\"ThunksDB\":") != std::string::npos);
  REQUIRE(merged.find("\"libGL\": 1") != std::string::npos);
  REQUIRE(merged.find("\"AppOverrides\":") != std::string::npos);
  REQUIRE(merged.find("\"some_pattern\":") != std::string::npos);
}

TEST_CASE("AppConfigWriter: GenerateUserConfigRootFS updates RootFS", "[AppConfigWriter]") {
  std::string existing = R"({
  "Config": {
    "RootFS": "ArchLinuxARM-m2",
    "Other": "val"
  }
})";

  std::string updated = AppConfigWriter::GenerateUserConfigRootFS("ArchLinuxARM-vk", existing);
  REQUIRE(updated.find("\"RootFS\": \"ArchLinuxARM-vk\"") != std::string::npos);
  REQUIRE(updated.find("\"Other\": \"val\"") != std::string::npos);
}
