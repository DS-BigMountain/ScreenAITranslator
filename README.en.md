# Screen AI Translator

[简体中文](README.md) | **English**

A native Windows application for translating screen content into Chinese. After a screen region is selected, the application identifies the source text positions, requests translations from the configured model, and overlays the results at those positions. Users select either OCR text translation or AI image translation; the application does not switch between them automatically. The settings interface uses WinUI 3, while screen capture and transparent overlays use Win32 / Direct2D.

## Main features

- **Region selection**: Drag to select a screen region. On release, the selected image remains fixed within the region and a status indicator is displayed until the translation appears. Processing can be canceled.
- **In-place overlays**: Translations follow the positions of source lines and paragraphs, with automatic font size estimation and matching of the dominant source text color.
- **Reading views**: Switch between the translated overlay, the original capture, and the full-text view, or copy the translation. Switching views does not send another model request.
- **Drag to translate a new region**: Drag the border of the translation frame to move it. On release, the application captures and translates the new region.
- **Translate the last region**: Reuse the last region submitted during the current session, such as a fixed subtitle area, dialog, or section of a page.
- **Display and controls**: Select a common font or specify an installed font, use a fixed or automatic font size, set text color and background opacity, enable automatic hiding, and configure keyboard shortcuts.

## Requirements and downloads

Requires Windows 10 version 1809 or later on x64. Translation requires a network connection and a user-provided API key. General-purpose language models must support the application's JSON response protocol; dedicated translation services use their native protocols. AI image translation requires a model that accepts image input. OCR text translation can use a text model or a dedicated translation service.

Download the installer or portable package from [GitHub Releases](https://github.com/DS-BigMountain/ScreenAITranslator/releases/latest). The installer provides a Simplified Chinese setup wizard. For the portable package, extract all files before running `ScreenAITranslator.exe`.

The packages include the WinUI 3 runtime, ONNX Runtime, local OCR models, icons, and third-party licenses. No separate Python, PaddlePaddle, or GPU environment is required. The executable must remain alongside its supporting runtimes, icons, and `models` directory; copying only the EXE is insufficient.

## Usage

1. Open **Model connections (模型连接)**, select a service, and enter the API endpoint, key, and an available model name or deployment ID. Model names can be entered manually or retrieved from the service; DeepL and Azure Translator do not require model selection. Switching models within the same service retains the key. Each service stores its own connection settings and restores its endpoint, model, and key when selected again. A service without an existing configuration starts with an empty key. The primary and vision connections are managed separately. Changes are written to disk when settings are saved; closing the window without saving discards the current changes.
2. Select a translation mode under **Translation and preferences (翻译与偏好)**. In OCR mode, also select the source language. In AI mode, configure a separate vision connection if needed. Click **Test connection (测试连接)** to validate only the connection and input type used by the current mode. The test uses a small built-in sample and does not upload a desktop capture; it may incur API charges. A model's presence in the model list does not establish image input support.
3. Save the settings, then click **Select region to translate (框选翻译)** under **Translation and preferences**, or use a configured keyboard shortcut.
4. Drag to select a region and release the mouse button to start translation. Press Esc or right-click to cancel selection. During processing, click × on the status box to cancel.
5. Use the toolbar beside the region to switch between **Translated overlay (原位译文)**, **Original image (查看原图)**, and **Full-text view (全文阅读)**, or select **Copy translation (复制译文)**, **Select another region (重新框选)**, or **Close (关闭)**.

## Translation modes

Select a mode under **Translation and preferences → Translation mode (翻译模式)**. Both modes produce Chinese translations. The mode can also be changed through **Recognition method (识别方法)** in the tray icon's context menu. The current mode is marked as selected; a change is saved automatically and applies to the next translation.

| Mode | Local processing | Content sent to the model |
| --- | --- | --- |
| OCR text translation | Built-in OCR detects text positions and recognizes text | Recognized text and region IDs only; no screenshots or coordinates are uploaded. The model returns translations only. |
| AI image translation | No local OCR | The entire selected image; the model recognizes and translates the text and returns source text positions. |

OCR mode supports English, Japanese, and Korean without system language packs. **Automatic detection (local) (自动识别（本地）)** compares the built-in recognizers as needed; selecting a language manually can reduce this overhead. Text with low recognition confidence is still sent for text translation, without an automatic switch to image input. The toolbar reports the number of unrecognized lines and regions with low confidence. If no translatable text is obtained, processing stops with a message; the user can switch to AI mode manually. Text missed by OCR cannot be recovered through a text request, and recognition confidence is not equivalent to accuracy.

AI mode does not submit local text boxes or OCR results. It is intended for content recognized directly by a vision model. Image resolution can be set to 1280, 1600, or 2200 px. Positions returned by the model are used for in-place overlays; positioning and recognition quality depend on the model. Translations can also be read in the full-text view.

AI mode handles translations and positions separately. Text boxes that intersect the capture are clipped locally. Missing, entirely out-of-bounds, or invalid positions are excluded from the overlay, while their translations remain available in the full-text view. If all positions are unusable, the full-text view opens automatically. When the model omits a translation, the full-text view retains and explicitly marks the source text. The status reports the number of empty or invalid segments. An enclosing JSON code block can be parsed locally without an additional request.

AI mode explicitly provides the uploaded image's pixel width and height to the model and requests pixel coordinates, which are then mapped to overlays at the capture dimensions. Invalid segments or overflowing translations do not cancel other overlays. Segments without usable positions remain available in the full-text view. Model position estimates may still be offset; the application does not guarantee exact alignment for every item.

Both modes check for clearly untranslated passages in a foreign language. Results that fail this check retain the source text and are marked, without covering the original image. If all positions are unusable or all translations fail the check, the full-text view opens automatically. This check does not replace an assessment of semantic translation quality.

If a response cannot be parsed, the application reports the specific format error without automatically repeating translation of the entire image. When an API is known not to support strict JSON response formatting, the application may adjust the format and retry. OCR requests for missing region IDs are handled separately.

OCR mode always uses the primary connection. AI mode uses the primary connection by default; enabling **Use a separate connection for AI image translation (AI 识图翻译使用独立连接)** selects the corresponding model and key instead. Connection tests follow the current mode. Completion status includes the selected mode, OCR time, API time, request count, and total processing time. API time includes network transfer, model response, parsing, and retries.

OCR models are loaded for each operation. Detection and recognition use memory in separate stages, and model sessions are released after processing. Approximately five seconds after the settings window, selection, and translation have been closed and background work has finished, the application hands control to a background process to release runtime resources. The tray icon, shortcuts, last region, and unexpired context are retained. If the handoff fails, the original process remains active. A settings window that is hidden but still open retains its draft and prevents this handoff.

## Service connections and networking

| Service | API | Configuration |
| --- | --- | --- |
| DeepSeek, OpenAI, custom OpenAI Compatible | Chat Completions | Enter a model name available to the account. Image input support depends on the model. |
| Alibaba Cloud Model Studio, Volcengine Ark, Moonshot, Zhipu, SiliconFlow | Each platform's compatible API | Official endpoints are preset. Deployment-based APIs require a deployment ID; regional endpoints can be adjusted. |
| Google Gemini | Native generateContent | Use a Gemini API key and model name. |
| Anthropic Claude | Native Messages | Use an Anthropic API key and model name. |
| DeepL | Native text translation | Free defaults to `https://api-free.deepl.com/v2`; Pro uses `https://api.deepl.com/v2`. |
| Microsoft Azure Translator | Native text translation | Enter the resource key. Regional resources also require an Azure region identifier. |
| Alibaba Cloud Qwen-MT | Dedicated machine translation protocol | Defaults to `qwen-mt-turbo`; use a model available to the account. |

DeepL and Azure Translator submit paragraphs in batches and validate the response count and source order. Qwen-MT uses dedicated translation parameters for each paragraph, with up to three concurrent requests. When many text regions are present, latency depends on paragraph count and service rate limits. Dedicated services always translate all input and do not execute custom prompts intended for general-purpose language models. DeepL can use recent source text as context; Azure and Qwen-MT currently do not receive historical context.

Network settings offer **System proxy (系统代理)**, **Direct connection (直接连接)**, and **Custom HTTP proxy (自定义 HTTP 代理)**. Enter a custom proxy as `host:port`; HTTPS is carried through HTTP CONNECT. SOCKS and proxy authentication are not supported. The primary connection, vision connection, and model lists share these network settings. Connections are not reused across different hosts or proxy configurations. Requests have an overall timeout and support cancellation; automatic redirects are disabled and TLS certificate verification remains enabled. Native services retry transient rate limits or gateway errors at most once, within the overall timeout. A long Retry-After interval results in an error instead of continued waiting.

**Translate all recognized text (完整翻译所有已识别文字)** is enabled by default. In OCR mode, the application checks each recognized region ID and makes at most one additional request for missing translations. Completed translations are retained; any remaining omissions produce an error. AI mode requests translation of all visible text, with completeness dependent on the model. A complete response does not trigger an additional verification request. Disable this option when custom instructions intentionally request only part of the content.

## Model request parameters

Reasoning is disabled by default for language model requests. The primary connection and separate vision connection each provide a **Request parameters (JSON) (请求参数（JSON）)** editor. Parameters are saved per service connection, and switching services restores the corresponding draft. Leaving the editor empty or entering `{}` uses the default configuration with reasoning disabled. Models that require reasoning may reject these settings. In that case, select another model or explicitly supply a supported configuration; the application does not automatically fall back to service defaults.

Enter API request body fields, rather than a prompt or an SDK `extra_body` wrapper. Objects are merged recursively, with explicit fields taking precedence; `null` removes the corresponding default field. The application manages the model, message content, streaming setting, and JSON response format. **Format and validate JSON (格式化并校验 JSON)** checks syntax; the service validates field support. API keys remain in the API key field. These parameters apply to language model translation and connection tests, but not to model lists, DeepL, Azure Translator, or Qwen-MT.

For example, to enable reasoning at high effort in DeepSeek:

```json
{
  "thinking": {"type": "enabled"},
  "reasoning_effort": "high"
}
```

Other APIs use their own official fields: OpenAI Chat Completions uses `reasoning_effort`; Qwen hybrid reasoning models use `enable_thinking`; Claude uses `thinking`; the native Gemini API uses `generationConfig.thinkingConfig`. Supported values depend on the selected model. The minimum reasoning level in Gemini 3 and Gemini 2.5 Pro is not treated as disabled; users must explicitly provide a supported configuration.

Parameter definitions are available in the official documentation for [OpenAI](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create), [DeepSeek](https://api-docs.deepseek.com/guides/thinking_mode), [Qwen](https://help.aliyun.com/zh/model-studio/deep-thinking), [Claude](https://platform.claude.com/docs/en/api/messages), and [Gemini](https://ai.google.dev/api/generate-content).

## Diagnostic logs

The application records local diagnostic logs automatically. Right-click the tray icon and select **Open diagnostic logs (打开诊断日志)**. Logs are stored in `%LOCALAPPDATA%\ScreenAITranslator\logs`; the current file is `diagnostic.jsonl`. After reproducing an issue, this file and the rotated logs in the same directory can be provided for investigation.

Logs include timestamps, task IDs, OCR and layout times, text or image routing and its reason, model names, request counts, network transfer stages, remaining time limits, missing region IDs, and error categories. Files use UTF-8 JSON Lines with UTC timestamps. They do not record keys, request headers, endpoint addresses, screenshots, prompts, source text, or translations. Each file is limited to 2 MiB; the current file and three rotated files are retained. A log write failure does not interrupt translation.

## Layout and reading

OCR mode retains local source text positions and associates translations through region IDs. AI mode uses positions returned by the model. Both modes convert positions to physical capture pixels for in-place layout.

Automatic font sizing is estimated from glyph height, and translations are measured using DirectWrite. Each translation is constrained to its paragraph area. Overflowing content can be scrolled at the blue marker or read in the full-text view. Fixed font size mode does not reduce the font size automatically. Regions with overlap conflicts retain the original image; their translations remain available in the full-text view.

With **Match source text color (匹配原文文字颜色)** enabled, the application extracts the dominant text color of each paragraph to preserve, where possible, the colors of links, body text, and gray annotations. Each paragraph uses one dominant color; mixed colors within a paragraph are not reconstructed character by character. Disabling this setting uses the manual color. If the original color cannot be extracted reliably, a fallback color that meets contrast requirements is used. The font list provides common font names and uses fonts installed on the computer; Windows substitutes missing fonts.

Results remain visible by default. If automatic hiding is enabled, hiding is suspended during dragging. Disabling in-place overlays uses a translation box for the selected region. Clicking the body text neither closes the result nor passes the click to the underlying window.

Layout and background processing are primarily intended for horizontal text on solid backgrounds. Complex backgrounds, substantial errors in model coordinates, and recognition errors can affect the overlay. The application does not provide generative reconstruction of complex backgrounds.

## Moving and reusing regions

Move the pointer to the border of the translation frame. When the move cursor appears, hold the left mouse button and drag. During dragging, only the border is shown and the interior is transparent. On release, the application captures the new position at the original capture dimensions and translates it. Movement is restricted to the current monitor. Right-clicking or an interrupted drag restores the original position and content. A click, minor pointer movement, or a drag back to the original position does not trigger translation.

**Translate the last region (翻译上次区域)** uses the last region submitted during the current session. Completing a normal selection or releasing a border drag updates this region; canceling a selection or drag does not replace it. Once submitted, the region remains available for retry even if translation fails or is canceled. Resizing the full-text window does not change the original capture dimensions used for subsequent translation.

The region is stored only in memory and is cleared when the application exits. If no region is available, translating the last region starts manual selection. Closing the translation or settings window does not exit the tray application. Region reuse does not perform automatic polling.

## Data and model services

Text translation sends recognized text to the configured service without uploading a screenshot. AI image translation sends a capture of the selected region without running or submitting local OCR. When context is enabled for general-purpose models, relevant source text and translations from prior requests are also sent. API keys for both the primary and vision connections are encrypted locally using Windows DPAPI. Billing, data handling, and availability are determined by the respective service provider.
