set(SAT_OCR_RUNTIME "${CMAKE_BINARY_DIR}/ocr-runtime")
set(SAT_ORT_INCLUDE "${CMAKE_BINARY_DIR}/_deps/ocr/ort/build/native/include")
# The mobile weights are mandatory: they remain the documented fallback for every
# environment that ships without the optional strongest-available models.
foreach(model IN ITEMS det.onnx rec-ja.onnx rec-ko.onnx)
  if(NOT EXISTS "${SAT_OCR_RUNTIME}/models/${model}")
    message(FATAL_ERROR "Run powershell -File scripts/Get-Ocr.ps1 before configuring.")
  endif()
endforeach()
if(NOT EXISTS "${SAT_ORT_INCLUDE}/onnxruntime_cxx_api.h")
  message(FATAL_ERROR "Run powershell -File scripts/Get-Ocr.ps1 before configuring.")
endif()
# Optional strongest-available weights. Their presence upgrades detection,
# recognition and text-line orientation in place; absence keeps the mobile path.
foreach(model IN ITEMS det-server.onnx rec-ja-server.onnx rec-en-server.onnx rec-ko-server.onnx cls.onnx
                       manga_det.onnx manga_rec.onnx ppocrv6_dict.txt)
  if(EXISTS "${SAT_OCR_RUNTIME}/models/${model}")
    message(STATUS "Bundled OCR enhancement available: ${model}")
  endif()
endforeach()
# Build-tree targets receive the mobile fallback set plus the small orientation
# classifier, which keeps the 60-second OCR regression budget stable and keeps
# detector output byte-identical to the previous release for layout regression
# tests. The delivery package copies the complete "${SAT_OCR_RUNTIME}" through
# scripts/build.ps1, so shipped builds prefer the server detector and PP-OCRv6
# recognizer automatically.
function(sat_deploy_ocr target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${SAT_OCR_RUNTIME}/licenses" "$<TARGET_FILE_DIR:${target}>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SAT_OCR_RUNTIME}/onnxruntime.dll"
            "$<TARGET_FILE_DIR:${target}>/onnxruntime.dll"
    VERBATIM)
  foreach(model IN ITEMS det.onnx rec-ja.onnx rec-en.onnx rec-ko.onnx cls.onnx)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SAT_OCR_RUNTIME}/models/${model}"
              "$<TARGET_FILE_DIR:${target}>/models/${model}"
      VERBATIM)
  endforeach()
  # Manga engine (PP-OCRv6_manga): optional weights plus the recognition dictionary that ships
  # beside them. A build without them routes every capture to the general engine.
  foreach(model IN ITEMS manga_det.onnx manga_rec.onnx ppocrv6_dict.txt)
    if(EXISTS "${SAT_OCR_RUNTIME}/models/${model}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SAT_OCR_RUNTIME}/models/${model}"
                "$<TARGET_FILE_DIR:${target}>/models/${model}"
        VERBATIM)
    endif()
  endforeach()
endfunction()
