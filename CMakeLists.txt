cmake_minimum_required(VERSION 3.20)
project(sleeve CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SLEEVE_NAME "sleeve")
set(SLEEVE_LEGACY_NAMES "armory")

# FTXUI
add_subdirectory(External/ftxui EXCLUDE_FROM_ALL)

# Core library
set(SLEEVE_CORE_SRCS
  Source/Core/Backend.cpp
  Source/Core/Paths.cpp
  Source/Core/ElfInspect.cpp
  Source/Core/Shapes.cpp
  Source/Core/Scanner.cpp
  Source/Core/Record.cpp
  Source/Core/Generate.cpp
  Source/Core/FileWriter.cpp
  Source/Core/Diff.cpp
  Source/Core/RootFS.cpp
  Source/Core/Theme.cpp
  Source/Core/Process.cpp
  Source/Core/AppConfigWriter.cpp
  Source/Core/Health.cpp
  External/tiny-json/tiny-json.c
)

add_library(SleeveCore STATIC ${SLEEVE_CORE_SRCS})
target_compile_definitions(SleeveCore PUBLIC
  SLEEVE_NAME="${SLEEVE_NAME}"
  SLEEVE_LEGACY_NAMES="${SLEEVE_LEGACY_NAMES}"
)
target_include_directories(SleeveCore PUBLIC
  Source
  Source/Core
  External/tiny-json
)

# Main executable
set(SLEEVE_TUI_SRCS
  Source/Tui/Job.cpp
  Source/Tui/App.cpp
  Source/Tui/MainScreen.cpp
  Source/Tui/ScanScreen.cpp
  Source/Tui/OptionsScreen.cpp
  Source/Tui/PreviewModal.cpp
  Source/Tui/HealthScreen.cpp
)

add_executable(sleeve
  Source/Main.cpp
  ${SLEEVE_TUI_SRCS}
)

target_compile_definitions(sleeve PRIVATE
  SLEEVE_NAME="${SLEEVE_NAME}"
  SLEEVE_LEGACY_NAMES="${SLEEVE_LEGACY_NAMES}"
)

target_include_directories(sleeve PRIVATE
  Source
  Source/Core
  Source/Tui
  External/tiny-json
)

target_link_libraries(sleeve PRIVATE
  SleeveCore
  ftxui::component
  ftxui::dom
  ftxui::screen
)

install(TARGETS sleeve RUNTIME DESTINATION bin)

# Unit tests
enable_testing()
find_package(Catch2 3 QUIET)
if (NOT Catch2_FOUND)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  add_subdirectory(External/Catch2 EXCLUDE_FROM_ALL)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/External/Catch2/extras/")
endif()
add_subdirectory(unittests)
