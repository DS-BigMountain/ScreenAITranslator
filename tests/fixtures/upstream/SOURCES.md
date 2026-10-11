# 韩语 OCR 补充样本

样本取自 PaddlePaddle/PaddleOCR 项目公开文档。原文件保留原样，仅用于测试，不进入交付包。识别断言及内存生命周期测试在内存中裁取左侧插画或指示牌区域，排除右侧已有识别结果。左侧仍包含文档原有的彩色检测标注，不将其视为未标注原图或漫画质量验收依据。

- `korean-sign.jpg`：插画与韩文手写风格文字，https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/docs/images/korean_1.jpg ，SHA256 `9C3AD15C00B6173D889E719FE4106314DA3B9FC8FC51F546279B946371C0D286`。
- `korean-scene.jpg`：实拍韩语指示牌，https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/docs/version2.x/ppocr/blog/images/korean_0.jpg ，SHA256 `6367339DD0B20168FB1ABDEAA067CCA1F2094AFFB9EC672B35E915D9C0D21CF1`。

项目许可：https://github.com/PaddlePaddle/PaddleOCR/blob/main/LICENSE （Apache-2.0）。英文、日文质量回归继续优先使用 `tests/fixtures/user/` 中的用户截图。
