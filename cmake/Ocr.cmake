set(SAT_OCR_RUNTIME "${CMAKE_BINARY_DIR}/ocr-runtime")
set(SAT_ORT_INCLUDE "${CMAKE_BINARY_DIR}/_deps/ocr/ort/build/native/include")
if(NOT EXISTS "${SAT_ORT_INCLUDE}/onnxruntime_cxx_api.h" OR NOT EXISTS "${SAT_OCR_RUNTIME}/models/rec-ja.onnx")
  message(FATAL_ERROR "Run powershell -File scripts/Get-Ocr.ps1 before configuring.")
endif()
function(sat_deploy_ocr target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${SAT_OCR_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
    VERBATIM)
endfunction()
