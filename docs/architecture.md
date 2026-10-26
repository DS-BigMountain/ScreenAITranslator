# 代码结构与诊断流程

本项目将桌面交互、翻译处理、服务协议和本地存储分开组织。主线程管理窗口及任务状态；工作线程执行截图处理、OCR、网络请求与排版，结果通过消息队列提交主线程。

| 模块 | 职责与入口 |
| --- | --- |
| `src/app/App.cpp` | 托盘、快捷键、框选、取消、结果显示和空闲进程交接 |
| `src/app/TranslationPipeline.cpp` | 单次翻译的 OCR、路由选择、图片编码、API 调用与排版 |
| `src/app/Worker.h` | 工作任务调度与停止令牌 |
| `src/capture/` | 屏幕截图、文字检测、OCR 识别、图片编码 |
| `src/providers/Provider.cpp` | 兼容接口、异步 HTTP、JSON 校验、覆盖完整性与补译 |
| `src/providers/ProviderServices.inc` | 原生服务协议；在 Provider 的内部实现上下文中编译 |
| `src/providers/RequestParameters.*` | 默认关闭思考、请求参数 JSON 校验与合并、响应协议字段保护 |
| `src/providers/VisionResponse.*` | 上传图片尺寸读取、视觉坐标转换、译文解析和局部异常统计 |
| `src/overlay/` | 区域排版、原图与译文显示、工具条操作 |
| `src/settings/` | 设置界面、配置校验、密钥保护与持久化 |
| `src/common/Diagnostics.*` | 有界诊断日志、任务关联、阶段计时及错误分类 |
| `src/common/Secret.h` | 临时凭据的生命周期及缓冲区清理 |

`PrepareTranslation` 接收裁剪后的图片及本次设置快照，返回 `PreparedTranslation`。该结果包含译文、排版数据和阶段耗时；函数本身不创建窗口。`App::DisplayTranslation` 在确认任务仍有效后更新界面。取消和失败沿原调用链传播，日志不得改变翻译结果或重试策略。

翻译模式由 `Settings::translationMode` 显式决定。OCR 模式调用 `DetectOcrText`，保留已识别文字，只将文字及编号发送给主连接；低置信度不触发图片上传，空识别结果停止请求。AI 模式跳过本地 OCR，向所选视觉连接提交整图，使用模型返回的位置排版。`SelectTranslationConnection` 只依据模式及独立连接设置选择连接；`route_selected` 记录选择原因。

## 日志解释

AI 直传模式不对内容解析错误重复提交整图。视觉解析独立于 OCR 编号协议：位置与图片相交时裁切；位置不可用时通过 `positionUnavailable` 阻止局部覆盖，完整文字保留在全文阅读中。缺少译文时设置 `untranslated`，保留原文并标记，且不写入已完成翻译上下文。`vision_response_summary` 记录调整、无法定位、缺少译文和无效片段数量，`response_rejected` 记录不含正文的错误代码。语法损坏、重复字段及资源限制仍返回解析错误。

每行日志是一个独立 JSON 对象。`time_utc` 表示 UTC 时间；同一次进程运行中的 `pid` 与 `operation` 关联一项操作，`span` 与 `parent` 关联嵌套阶段。`stage_begin` 和 `stage_end` 记录阶段边界，`duration_ms` 使用单调时钟，避免系统时钟校正影响耗时。

| 阶段或事件 | 含义 |
| --- | --- |
| `ocr_detector_load`、`ocr_detect` | 检测模型加载与文字框检测 |
| `ocr_recognizer_load`、`ocr_recognize` | 识别模型加载与文字识别；AI 模式无本地 OCR 步骤 |
| `ocr_language_selected` | 自动模式各识别器最终采用的行数；同一截图可包含多个识别器的结果 |
| `ocr_fallback` | 本地 OCR 异常或保留部分结果，不表示切换到 AI |
| `route_selected` | 实际连接、模型、文字或图片路径及选择原因 |
| `api_attempt` | 一次服务调用及其剩余总时限 |
| `http_send` | 发送请求，可能包含解析地址、建立连接、TLS 与上传 |
| `http_headers` | 等待响应头；可能包含服务端排队与生成时间 |
| `http_body` | 读取正文；`response_bytes` 表示已接收字节数 |
| `coverage_missing`、`retry` | 缺失区域编号、已保留结果数量及重试原因 |
| `layout`、`display` | 排版与窗口显示 |
| `translation_quality` | 明显未完成翻译的段落数量；不追加网络请求 |
| `vision_response_summary` | 图片坐标范围、坐标单位与片段统计；`phase=pixels` 表示上传图像素坐标，内部再映射至原图 |

`outcome=interrupted` 表示该阶段未正常结束，应结合相同任务的错误事件或取消事件解释。网络阶段只能反映客户端可观测边界，不能单独区分服务端排队与模型推理。补译与协议重试共用本次 API 总时限。

日志只接收允许的元数据字段，不持久化请求正文、响应正文、接口地址、密钥、截图、上下文或原文译文。当前文件及三个轮转文件共约 8 MiB；写入异常不向翻译流程传播。

## 修改与验证

源文件和测试文件遵循根目录 `.clang-format`，第三方代码及生成文件不参与格式化。新增行为应放入对应职责模块，避免向界面回调继续叠加处理步骤。

验证按影响范围选择目标：`DiagnosticsTests` 检查日志边界与轮转；`ProviderTests` 检查本地 HTTP 协议、超时、取消和补译；OCR 与排版回归使用 `tests/fixtures/user/` 中的真实截图。默认不运行全量测试，不自动创建桌面顶层测试窗口。
