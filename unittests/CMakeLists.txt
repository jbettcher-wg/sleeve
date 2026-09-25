add_executable(SleeveTests
  Test_ElfInspect.cpp
  Test_Shapes.cpp
  Test_Generate.cpp
  Test_FileWriter.cpp
  Test_Diff.cpp
  Test_RootFS.cpp
  Test_Theme.cpp
  Test_AppConfigWriter.cpp
  Test_Health.cpp
  Test_Libs.cpp
  Test_Record.cpp
  Test_Scanner.cpp
  # The TUI's background-work layer. It deliberately depends on nothing but the standard
  # library, so the start/progress/cancel/result contract is testable without a terminal.
  ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Tui/Job.cpp
  Test_Job.cpp
)

target_link_libraries(SleeveTests PRIVATE
  SleeveCore
  Catch2::Catch2WithMain
)

target_include_directories(SleeveTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../Source
  ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Core
  ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Tui
  ${CMAKE_CURRENT_SOURCE_DIR}/../External/tiny-json
)

include(CTest)
include(Catch)
catch_discover_tests(SleeveTests TEST_SUFFIX ".SleeveTest")
