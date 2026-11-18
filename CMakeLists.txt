# Three's Company: CLAP, VST3 and AU (macOS) / CLAP and VST3 (Windows), with clap-wrapper.
#
# macOS:    sh build-all-mac.sh            (needs CMake: brew install cmake)
# Windows:  done by GitHub Actions (.github/workflows/build.yml); by hand:
#           cmake -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL -DWEBVIEW2_DIR=<nuget package folder>
#           cmake --build build --config Release
#
# The quick CLAP-only Mac build (build-mac.sh) still works and does not need CMake.
cmake_minimum_required(VERSION 3.21)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS")
project(ThreesCompany VERSION 0.18.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
# clap and clap-wrapper (MIT). clap-wrapper downloads the VST3 SDK (MIT since 3.8) and Apple's AudioUnitSDK itself.
set(CLAP_WRAPPER_OUTPUT_NAME clap-wrapper-target)
set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE)
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE)
FetchContent_Declare(clap GIT_REPOSITORY https://github.com/free-audio/clap.git GIT_TAG 1.2.7 GIT_SHALLOW ON)
FetchContent_Declare(clap-wrapper GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
                     GIT_TAG 2a4da4db3665ca18dd0830c5dec4303d3765efec)
FetchContent_MakeAvailable(clap clap-wrapper)

# ----- the plugin itself, as a static library the formats are built from
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_gui.py
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_library(threes-company-impl STATIC src/tc_clap.cpp)
target_include_directories(threes-company-impl PUBLIC
  src libs/sst-filters-include libs/sst-basic-blocks-include libs/simde)
target_link_libraries(threes-company-impl PUBLIC clap)
target_compile_definitions(threes-company-impl PUBLIC TC_SEPARATE_ENTRY=1)

if(APPLE)
  string(TIMESTAMP TC_BUILD_TAG "%s")
  target_sources(threes-company-impl PRIVATE src/tc_gui_mac.mm)
  set_source_files_properties(src/tc_gui_mac.mm PROPERTIES COMPILE_FLAGS "-fno-objc-arc -DTC_OBJC_SUFFIX=${TC_BUILD_TAG}")
  target_link_libraries(threes-company-impl PUBLIC "-framework Cocoa" "-framework WebKit")
  set(TC_FORMATS CLAP VST3 AUV2)
elseif(WIN32)
  # WebView2 SDK: the Microsoft.Web.WebView2 NuGet package, unpacked (the workflow downloads it)
  set(WEBVIEW2_DIR "" CACHE PATH "Unpacked Microsoft.Web.WebView2 NuGet package")
  if(NOT WEBVIEW2_DIR)
    message(FATAL_ERROR "Set WEBVIEW2_DIR to the unpacked Microsoft.Web.WebView2 NuGet package")
  endif()
  target_sources(threes-company-impl PRIVATE src/tc_gui_win.cpp)
  target_include_directories(threes-company-impl PRIVATE ${WEBVIEW2_DIR}/build/native/include)
  target_link_libraries(threes-company-impl PUBLIC ${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib version shlwapi ole32 shell32 uuid)
  target_compile_definitions(threes-company-impl PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
  set(TC_FORMATS CLAP VST3)
else()
  set(TC_FORMATS CLAP VST3)
endif()

make_clapfirst_plugins(
  TARGET_NAME threes-company
  IMPL_TARGET threes-company-impl
  OUTPUT_NAME "Threes Company"
  ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/tc_entry.cpp"
  BUNDLE_IDENTIFIER "com.otiskanno.threes-company"
  BUNDLE_VERSION ${PROJECT_VERSION}
  WINDOWS_FOLDER_VST3 TRUE
  PLUGIN_FORMATS ${TC_FORMATS}
  COPY_AFTER_BUILD FALSE
  AUV2_MANUFACTURER_NAME "Otis Kanno"
  AUV2_MANUFACTURER_CODE "OtKa"
  AUV2_SUBTYPE_CODE "ThCo"
  AUV2_INSTRUMENT_TYPE "aufx"
)
