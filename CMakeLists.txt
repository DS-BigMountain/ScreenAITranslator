cmake_minimum_required(VERSION 3.24)
project(ScreenAITranslator VERSION 1.0.0 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Windows x64 is required")
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc)
endif()
add_library(sat_core
 src/common/Platform.cpp
 src/capture/Capture.cpp
 src/providers/Provider.cpp
 src/settings/Store.cpp)
target_include_directories(sat_core PUBLIC src third_party)
target_link_libraries(sat_core PUBLIC d3d11 dxgi windowscodecs ole32 oleaut32 crypt32 winhttp shcore shell32 advapi32 user32 gdi32)
add_library(sat_ui
 src/selection/Selection.cpp src/overlay/SpatialLayout.cpp
 src/overlay/Overlay.cpp src/overlay/OverlayToolbar.cpp src/settings/SettingsWindow.cpp)
target_link_libraries(sat_ui PUBLIC sat_core comctl32 d2d1 dwrite dwmapi comdlg32 msimg32)
add_executable(ScreenAITranslator WIN32
 src/app/App.cpp
 resources/app.rc)
target_link_libraries(ScreenAITranslator PRIVATE sat_ui)
target_link_options(ScreenAITranslator PRIVATE /MANIFEST:NO)
enable_testing()
add_executable(CoreTests tests/CoreTests.cpp tests/CaptureTests.cpp tests/StoreTests.cpp tests/WorkerTests.cpp)
target_link_libraries(CoreTests PRIVATE sat_core)
add_test(NAME CoreTests COMMAND CoreTests)
add_executable(ProviderTests tests/ProviderTests.cpp)
target_link_libraries(ProviderTests PRIVATE sat_core ws2_32)
add_test(NAME ProviderTests COMMAND ProviderTests)
add_executable(LiveApiDiagnostic tests/LiveApiDiagnostic.cpp)
target_link_libraries(LiveApiDiagnostic PRIVATE sat_core)
add_executable(CaptureLiveTests tests/CaptureTests.cpp)
target_compile_definitions(CaptureLiveTests PRIVATE SAT_CAPTURE_TEST_MAIN)
target_link_libraries(CaptureLiveTests PRIVATE sat_core)
add_executable(CapturePixelTests tests/CapturePixelTests.cpp src/selection/Selection.cpp)
target_link_libraries(CapturePixelTests PRIVATE sat_core msimg32 dwmapi)
add_executable(NativeTests tests/NativeTests.cpp resources/app.rc)
target_link_libraries(NativeTests PRIVATE sat_ui)
target_link_options(NativeTests PRIVATE /MANIFEST:NO)
add_test(NAME NativeTests COMMAND NativeTests)
add_executable(AppIntegrationTests tests/AppIntegrationTests.cpp resources/app.rc)
target_link_libraries(AppIntegrationTests PRIVATE sat_ui psapi)
target_link_options(AppIntegrationTests PRIVATE /MANIFEST:NO)
add_test(NAME AppIntegrationTests COMMAND AppIntegrationTests)
set_tests_properties(CoreTests ProviderTests NativeTests AppIntegrationTests PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
install(TARGETS ScreenAITranslator RUNTIME DESTINATION .)





configure_file(tests/fixtures/installer-en.png test-fixtures/installer-en.png COPYONLY)
