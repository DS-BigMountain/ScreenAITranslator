set(SAT_PACKAGES "${CMAKE_BINARY_DIR}/_deps/packages")
set(SAT_FOUNDATION "${SAT_PACKAGES}/Microsoft.WindowsAppSDK.Foundation.1.8.260803002")
set(SAT_INTERACTIVE "${SAT_PACKAGES}/Microsoft.WindowsAppSDK.InteractiveExperiences.1.8.260708001")
set(SAT_WINUI "${SAT_PACKAGES}/Microsoft.WindowsAppSDK.WinUI.1.8.260803003")
if(NOT EXISTS "${CMAKE_BINARY_DIR}/_deps/winui-projection/complete.stamp" OR NOT EXISTS "${CMAKE_BINARY_DIR}/_deps/winui-app.manifest")
  message(FATAL_ERROR "Run powershell -File scripts/Get-WinUI.ps1 before configuring.")
endif()
add_library(sat_winui INTERFACE)
target_include_directories(sat_winui INTERFACE
  "${CMAKE_BINARY_DIR}/_deps/winui-projection"
  "${SAT_FOUNDATION}/include" "${SAT_INTERACTIVE}/include" "${SAT_WINUI}/include")
target_link_libraries(sat_winui INTERFACE windowsapp
  "${SAT_FOUNDATION}/lib/native/x64/Microsoft.WindowsAppRuntime.lib"
  "${SAT_INTERACTIVE}/lib/native/win10-x64/Microsoft.UI.Dispatching.lib")
set(SAT_RUNTIME "${CMAKE_BINARY_DIR}/winui-runtime")
file(MAKE_DIRECTORY "${SAT_RUNTIME}")
foreach(package IN ITEMS "${SAT_FOUNDATION}" "${SAT_INTERACTIVE}" "${SAT_WINUI}")
  file(COPY "${package}/runtimes-framework/win-x64/native/" DESTINATION "${SAT_RUNTIME}")
endforeach()
configure_file("${SAT_RUNTIME}/Microsoft.UI.Xaml.Controls.pri" "${SAT_RUNTIME}/resources.pri" COPYONLY)
# Windows App SDK 本机组件使用动态 CRT；将可再发行库随程序部署。
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  file(COPY "${runtime}" DESTINATION "${SAT_RUNTIME}")
endforeach()
function(sat_deploy_winui target)
  target_compile_definitions(${target} PRIVATE SAT_WINUI_MANIFEST="${CMAKE_BINARY_DIR}/_deps/winui-app.manifest")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${SAT_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
    VERBATIM)
endfunction()
