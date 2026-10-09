cmake_minimum_required(VERSION 3.24)
project(ScreenAITranslator VERSION 1.3.0 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Windows x64 is required")
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
include(cmake/WinUI.cmake)
include(cmake/Ocr.cmake)
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc)
endif()
add_library(sat_core
 src/common/Platform.cpp
 src/capture/Capture.cpp
 src/capture/TextDetection.cpp
 src/capture/BundledOcr.cpp
 src/providers/Provider.cpp
 src/settings/Store.cpp)
target_include_directories(sat_core PUBLIC src third_party)
target_include_directories(sat_core SYSTEM PRIVATE "${SAT_ORT_INCLUDE}")
target_include_directories(sat_core PRIVATE "${CMAKE_BINARY_DIR}/_deps/winui-projection")
target_link_libraries(sat_core PUBLIC windowsapp)
target_link_libraries(sat_core PUBLIC d3d11 dxgi windowscodecs ole32 oleaut32 runtimeobject crypt32 winhttp shcore shell32 advapi32 user32 gdi32)
add_library(sat_ui
 src/selection/Selection.cpp src/overlay/SpatialLayout.cpp
 src/overlay/Overlay.cpp src/overlay/OverlayToolbar.cpp src/settings/SettingsWindow.cpp src/settings/WinUIRuntime.cpp)
target_link_libraries(sat_ui PUBLIC sat_core sat_winui comctl32 d2d1 dwrite dwmapi comdlg32 msimg32)
add_executable(ScreenAITranslator WIN32
 src/app/App.cpp
 resources/app.rc)
target_link_libraries(ScreenAITranslator PRIVATE sat_ui)
target_link_options(ScreenAITranslator PRIVATE /MANIFEST:NO)
sat_deploy_winui(ScreenAITranslator)
enable_testing()
add_executable(CoreTests tests/CoreTests.cpp tests/CaptureTests.cpp tests/StoreTests.cpp tests/WorkerTests.cpp)
target_link_libraries(CoreTests PRIVATE sat_core)
add_test(NAME CoreTests COMMAND CoreTests)
add_executable(ProviderTests tests/ProviderTests.cpp)
target_link_libraries(ProviderTests PRIVATE sat_core ws2_32)
add_test(NAME ProviderTests COMMAND ProviderTests)
add_executable(LiveApiDiagnostic tests/LiveApiDiagnostic.cpp)
target_link_libraries(LiveApiDiagnostic PRIVATE sat_core)
add_executable(TranslationBenchmark tests/TranslationBenchmark.cpp)
target_link_libraries(TranslationBenchmark PRIVATE sat_ui)
add_executable(CaptureLiveTests tests/CaptureTests.cpp)
target_compile_definitions(CaptureLiveTests PRIVATE SAT_CAPTURE_TEST_MAIN)
target_link_libraries(CaptureLiveTests PRIVATE sat_core)
add_executable(CapturePixelTests tests/CapturePixelTests.cpp src/selection/Selection.cpp)
target_link_libraries(CapturePixelTests PRIVATE sat_core msimg32 dwmapi)
add_executable(NativeTests tests/NativeTests.cpp resources/app.rc)
target_link_libraries(NativeTests PRIVATE sat_ui)
target_link_options(NativeTests PRIVATE /MANIFEST:NO)
sat_deploy_winui(NativeTests)
add_test(NAME NativeTests COMMAND NativeTests)
add_executable(TextLayoutTests tests/TextLayoutTests.cpp)
target_link_libraries(TextLayoutTests PRIVATE sat_ui)
add_test(NAME TextLayoutTests COMMAND TextLayoutTests)
add_executable(BundledOcrTests tests/BundledOcrTests.cpp)
target_link_libraries(BundledOcrTests PRIVATE sat_core)
add_test(NAME BundledOcrTests COMMAND BundledOcrTests)
set_tests_properties(BundledOcrTests TextLayoutTests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
foreach(with_runtime IN ITEMS OFF ON)
  add_test(NAME MissingOcrRuntime_${with_runtime} COMMAND ${CMAKE_COMMAND}
    "-DTEST_BINARY=$<TARGET_FILE:BundledOcrTests>"
    "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/installer-en.png"
    "-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/test-output"
    "-DOCR_RUNTIME=${SAT_OCR_RUNTIME}/onnxruntime.dll" "-DWITH_RUNTIME=${with_runtime}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TestMissingOcr.cmake")
  set_tests_properties(MissingOcrRuntime_${with_runtime} PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
endforeach()
add_test(NAME MissingOcrRecognizer COMMAND ${CMAKE_COMMAND}
  "-DTEST_BINARY=$<TARGET_FILE:BundledOcrTests>"
  "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/installer-en.png"
  "-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/test-output"
  "-DOCR_RUNTIME=${SAT_OCR_RUNTIME}/onnxruntime.dll" "-DWITH_RUNTIME=ON" "-DWITH_DETECTOR=ON"
  "-DDETECTOR_MODEL=${SAT_OCR_RUNTIME}/models/det.onnx"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TestMissingOcr.cmake")
set_tests_properties(MissingOcrRecognizer PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
add_executable(WinUISettingsTests tests/WinUISettingsTests.cpp resources/app.rc)
target_link_libraries(WinUISettingsTests PRIVATE sat_ui)
target_link_options(WinUISettingsTests PRIVATE /MANIFEST:NO)
sat_deploy_winui(WinUISettingsTests)
add_test(NAME WinUISettingsTests COMMAND WinUISettingsTests)
add_executable(AppIntegrationTests tests/AppIntegrationTests.cpp resources/app.rc)
target_link_libraries(AppIntegrationTests PRIVATE sat_ui psapi)
target_link_options(AppIntegrationTests PRIVATE /MANIFEST:NO)
sat_deploy_winui(AppIntegrationTests)
add_test(NAME AppIntegrationTests COMMAND AppIntegrationTests)
set_tests_properties(CoreTests ProviderTests NativeTests WinUISettingsTests AppIntegrationTests PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
install(TARGETS ScreenAITranslator RUNTIME DESTINATION .)
install(DIRECTORY "${SAT_RUNTIME}/" DESTINATION .)

configure_file(tests/fixtures/installer-en.png test-fixtures/installer-en.png COPYONLY)
configure_file(tests/fixtures/news-ja.png test-fixtures/news-ja.png COPYONLY)
configure_file(tests/fixtures/news-ja-translations.json test-fixtures/news-ja-translations.json COPYONLY)

foreach(target IN ITEMS ScreenAITranslator CoreTests ProviderTests LiveApiDiagnostic TranslationBenchmark CaptureLiveTests CapturePixelTests NativeTests TextLayoutTests BundledOcrTests WinUISettingsTests AppIntegrationTests)
  sat_deploy_ocr(${target})
endforeach()
install(DIRECTORY "${SAT_OCR_RUNTIME}/" DESTINATION .)
