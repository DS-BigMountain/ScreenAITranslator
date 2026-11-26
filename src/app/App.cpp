#include "Worker.h"
#include "IdleRestart.h"
#include "SourceMatching.h"
#include "TranslationPipeline.h"
#include "common/AppIdentity.h"
#include "common/Secret.h"
#include "common/Platform.h"
#include "common/Diagnostics.h"
#include "common/Trace.h"
#include "capture/Capture.h"
#include "providers/Provider.h"
#include "providers/TranslationQuality.h"
#include "settings/Store.h"
#include "settings/SettingsWindow.h"
#include "settings/WinUIRuntime.h"
#include "selection/Selection.h"
#include "overlay/Overlay.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <deque>
#include <atomic>
#include <algorithm>
namespace sat {
namespace {
constexpr UINT UiMessage = WM_APP + 1, TrayMessage = WM_APP + 2, ContinuousClickMessage = WM_APP + 3,
               ContinuousEscapeMessage = WM_APP + 4;
constexpr UINT_PTR HideTimer = 1, ContextTimer = 2, IdleTimer = 3, PollTimer = 4;
enum class Stage { Idle, Capture, Selecting, Preprocessing, Requesting, Layout, Displaying };
enum TrayCommand { OpenSettings = 100, ExitApp = 105, OpenLogs = 106 };
bool IsPointInWindow(HWND hwnd, POINT pt) {
    if (!hwnd || !IsWindowVisible(hwnd))
        return false;
    RECT r{};
    return GetWindowRect(hwnd, &r) && PtInRect(&r, pt) != FALSE;
}
std::wstring Executable() {
    std::wstring p(32768, 0);
    auto n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
    p.resize(n);
    return p;
}
class Hotkeys {
    HWND window_{};
    std::vector<int> registered_;
    Hotkey values_[4]{};
    bool suspended_{};
    void Release() {
        for (int id : registered_)
            UnregisterHotKey(window_, id);
        registered_.clear();
    }

public:
    explicit Hotkeys(HWND w) : window_(w) {}
    ~Hotkeys() {
        Release();
    }
    bool Suspended() const {
        return suspended_;
    }
    void Suspend() {
        suspended_ = true;
        Release();
    }
    std::wstring Resume() {
        if (!suspended_)
            return {};
        suspended_ = false;
        Settings saved;
        std::copy(std::begin(values_), std::end(values_), std::begin(saved.hotkeys));
        return Apply(saved);
    }
    std::wstring Apply(const Settings& s) {
        if (suspended_) {
            std::copy(std::begin(s.hotkeys), std::end(s.hotkeys), std::begin(values_));
            return {};
        }
        // Re-register as one transaction so changing an existing action can reuse its key.
        Hotkey previous[4];
        std::copy(std::begin(values_), std::end(values_), previous);
        Release();
        for (int i = 0; i < 4; ++i) {
            auto k = s.hotkeys[i];
            if (k.key && !RegisterHotKey(window_, i + 1, k.modifiers | MOD_NOREPEAT, k.key)) {
                Release();
                for (int j = 0; j < 4; ++j)
                    if (previous[j].key &&
                        RegisterHotKey(window_, j + 1, previous[j].modifiers | MOD_NOREPEAT, previous[j].key))
                        registered_.push_back(j + 1);
                const wchar_t* names[]{L"普通截图翻译", L"翻译上次区域", L"打开设置", L"连续翻译"};
                return std::wstring(names[i]) + L"快捷键已被占用，设置未保存。";
            }
            if (k.key)
                registered_.push_back(i + 1);
        }
        std::copy(std::begin(s.hotkeys), std::end(s.hotkeys), values_);
        return {};
    }
};
} // namespace
class App {
    friend class AppIdleTest;
    HWND window_{};
    bool migrateStartup_{};
    Store store_;
    Settings settings_;
    PersistentState state_;
    std::optional<FixedRegion> sessionRegion_;
    bool sessionContinuous_{};
    std::string key_;
    std::wstring recovery_;
    std::function<std::unique_ptr<ITranslationProvider>(const std::string&)> providerFactory_;
    std::unique_ptr<Hotkeys> hotkeys_;
    SettingsWindow settingsWindow_;
    Selection selection_;
    Overlay overlay_, progress_;
    TranslationContext context_;
    Worker worker_;
    Worker monitorWorker_;
    std::mutex queueMutex_;
    std::deque<std::function<void()>> queue_;
    bool shuttingDown_{};
    unsigned long long generation_{};
    Stage stage_{Stage::Idle};
    enum class ContinuousState { Inactive, Selecting, Translating, Displaying, Waiting };
    ContinuousState continuous_{ContinuousState::Inactive};
    unsigned long long continuousEpoch_{};
    inline static App* activeHookApp_{};
    HHOOK mouseHook_{}, keyHook_{};
    bool ignoreNextButtonUp_{};
    bool monitorInFlight_{};
    std::optional<Image> baselineFrame_;
    std::optional<Image> lastSample_;
    int stableCount_{};
    Monitor currentMonitor_;
    RECT anchor_{};
    bool trayAdded_{};
    UINT taskbarCreated_{};
    bool background_{};
    bool apiActive_{};
    bool usedResources_{};
    std::function<bool(const std::string&)> idleRestart_;
    std::string resume_;
    void ArmIdle() {
        if (idleRestart_ && usedResources_ && !shuttingDown_)
            SetTimer(window_, IdleTimer, 5000, nullptr);
    }
    std::string IdleSnapshot() {
        nlohmann::json value = {{"version", 1},
                                {"context", nlohmann::json::array()},
                                {"expires", context_.ExpiresAt()},
                                {"recovery", Utf8(recovery_)},
                                {"continuous", sessionContinuous_}};
        for (const auto& item : context_.Get(settings_.contextEnabled, settings_.contextSize))
            value["context"].push_back({item.original, item.translated});
        if (sessionRegion_) {
            const auto& r = *sessionRegion_;
            value["region"] = {Utf8(r.monitorId), r.x, r.y, r.width, r.height, r.monitorWidth,
                               r.monitorHeight};
        }
        value["textSession"] = nlohmann::json::parse(settingsWindow_.TextSession());
        return value.dump();
    }
    void RestoreIdle() {
        if (resume_.empty())
            return;
        auto value = nlohmann::json::parse(resume_);
        resume_.clear();
        resume_.shrink_to_fit();
        if (value.at("version") != 1)
            throw AppError("idle", "Unsupported resume state");
        recovery_ = Wide(value.at("recovery").get<std::string>());
        sessionContinuous_ = value.value("continuous", false);
        if (value.contains("region")) {
            const auto& r = value.at("region");
            sessionRegion_ = FixedRegion{Wide(r.at(0).get<std::string>()),
                                         r.at(1).get<double>(),
                                         r.at(2).get<double>(),
                                         r.at(3).get<double>(),
                                         r.at(4).get<double>(),
                                         r.at(5).get<int>(),
                                         r.at(6).get<int>()};
        }
        std::vector<ContextItem> items;
        for (const auto& item : value.at("context"))
            items.push_back({item.at(0).get<std::string>(), item.at(1).get<std::string>()});
        if (settings_.contextEnabled)
            context_.Restore(std::move(items), value.at("expires").get<ULONGLONG>());
        if (value.contains("textSession"))
            settingsWindow_.RestoreTextSession(value.at("textSession").dump());
    }
    void TryIdle() {
        KillTimer(window_, IdleTimer);
        // Hidden settings still hold unsaved drafts. Only explicitly closed windows
        // may be discarded; active requests, overlays and queued UI work also block handoff.
        if (!idleRestart_ || !usedResources_ || shuttingDown_ || stage_ != Stage::Idle || apiActive_ ||
            continuous_ != ContinuousState::Inactive || settingsWindow_.Window() || selection_.Window() ||
            overlay_.Count() || progress_.Count())
            return;
        if (!worker_.Idle() || !settingsWindow_.BackgroundIdle()) {
            SetTimer(window_, IdleTimer, 1000, nullptr);
            return;
        }
        {
            std::lock_guard lock(queueMutex_);
            if (!queue_.empty()) {
                SetTimer(window_, IdleTimer, 1000, nullptr);
                return;
            }
        }
        try {
            if (idleRestart_(IdleSnapshot()))
                DestroyWindow(window_);
        } catch (...) { /* Keep the existing controller and hotkeys if handoff fails. */
        }
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        try {
            return self->Message(h, m, w, l);
        } catch (const AppError& e) {
            self->Error(e);
        } catch (const std::exception&) {
            self->Error(AppError("app", "操作未完成"));
        }
        return 0;
    }
    void Post(std::function<void()> f) {
        std::lock_guard lock(queueMutex_);
        if (shuttingDown_)
            return;
        queue_.push_back(std::move(f));
        PostMessageW(window_, UiMessage, 0, 0);
    }
    void Dispatch() {
        std::deque<std::function<void()>> items;
        {
            std::lock_guard lock(queueMutex_);
            items.swap(queue_);
        }
        for (auto& f : items)
            f();
    }
    void SetStage(Stage stage) {
        stage_ = stage;
        UpdateTray();
        if (stage == Stage::Idle)
            ArmIdle();
        else
            KillTimer(window_, IdleTimer);
    }
    void UpdateTray() {
        NOTIFYICONDATAW ni{sizeof(ni)};
        ni.hWnd = window_;
        ni.uID = 1;
        if (!settings_.tray) {
            if (trayAdded_)
                Shell_NotifyIconW(NIM_DELETE, &ni);
            trayAdded_ = false;
            return;
        }
        ni.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        ni.uCallbackMessage = TrayMessage;
        ni.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                 LR_SHARED));
        if (!ni.hIcon)
            ni.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcsncpy_s(ni.szTip, AppName, _TRUNCATE);
        if (Shell_NotifyIconW(trayAdded_ ? NIM_MODIFY : NIM_ADD, &ni))
            trayAdded_ = true;
    }
    void ContextChanged() {
        KillTimer(window_, ContextTimer);
        context_.Clear();
    }
    void SaveState() {
        try {
            store_.SaveState(state_);
        } catch (const AppError& e) {
            settingsWindow_.Status(Wide(Sanitize(e.what(), key_)));
        }
    }
    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION && activeHookApp_) {
            const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            const bool down = wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                              wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN;
            const bool up = wParam == WM_LBUTTONUP || wParam == WM_RBUTTONUP ||
                            wParam == WM_MBUTTONUP || wParam == WM_XBUTTONUP;
            if (down || up)
                activeHookApp_->HandleHookMouse(down, info->pt);
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
    static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION && activeHookApp_ && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            if (info->vkCode == VK_ESCAPE &&
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0 &&
                (GetAsyncKeyState(VK_MENU) & 0x8000) == 0 &&
                (GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
                (GetAsyncKeyState(VK_RWIN) & 0x8000) == 0 &&
                activeHookApp_->HandleHookEscape())
                return 1;
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
    void HandleHookMouse(bool down, POINT pt) {
        if (continuous_ == ContinuousState::Inactive || continuous_ == ContinuousState::Selecting)
            return;
        const bool onControl = overlay_.IsControlPoint(pt) || progress_.IsControlPoint(pt) ||
                               IsPointInWindow(settingsWindow_.Window(), pt);
        if (down) {
            ignoreNextButtonUp_ = onControl;
            if (!onControl)
                PostMessageW(window_, ContinuousClickMessage, 1, 0);
        } else {
            const bool ignored = ignoreNextButtonUp_;
            ignoreNextButtonUp_ = false;
            if (!ignored && !onControl)
                PostMessageW(window_, ContinuousClickMessage, 0, 0);
        }
    }
    bool HandleHookEscape() {
        if (continuous_ == ContinuousState::Inactive)
            return false;
        PostMessageW(window_, ContinuousEscapeMessage, 0, 0);
        return true;
    }
    void InstallContinuousHooks() {
        activeHookApp_ = this;
        if (!mouseHook_)
            mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
        if (!keyHook_)
            keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    }
    void RemoveContinuousHooks() {
        if (mouseHook_) {
            UnhookWindowsHookEx(mouseHook_);
            mouseHook_ = nullptr;
        }
        if (keyHook_) {
            UnhookWindowsHookEx(keyHook_);
            keyHook_ = nullptr;
        }
        if (activeHookApp_ == this)
            activeHookApp_ = nullptr;
        ignoreNextButtonUp_ = false;
    }
    void ShowContinuousBadge(const std::wstring& status) {
        if (!Valid(anchor_) || currentMonitor_.id.empty())
            return;
        overlay_.ShowContinuousBadge(
            anchor_, currentMonitor_, status, [this] { Post([this] { ExitContinuous(); }); }, settings_,
            [this] { Post([this] { Begin(false, true, true); }); });
    }
    void ResetContinuousStability() {
        ++continuousEpoch_;
        monitorWorker_.Cancel();
        monitorInFlight_ = false;
        baselineFrame_.reset();
        lastSample_.reset();
        stableCount_ = 0;
        SetTimer(window_, PollTimer, 100, nullptr);
    }
    void EnterContinuousWaiting(const wchar_t* badgeText = L"连续翻译中 · 等待翻页") {
        continuous_ = ContinuousState::Waiting;
        SetStage(Stage::Idle);
        ShowContinuousBadge(badgeText);
        ResetContinuousStability();
    }
    void ExitContinuous() {
        KillTimer(window_, PollTimer);
        ++continuousEpoch_;
        monitorWorker_.Cancel();
        continuous_ = ContinuousState::Inactive;
        monitorInFlight_ = false;
        baselineFrame_.reset();
        lastSample_.reset();
        stableCount_ = 0;
        RemoveContinuousHooks();
        overlay_.CloseContinuousBadge();
        Cancel();
    }
    void Error(const AppError& e) {
        DiagnosticContext context(generation_);
        DiagnosticSpan failure("ui_error");
        failure.Fail(e);
        std::wstring message = Wide(Sanitize(e.what(), key_));
        settingsWindow_.Status(message);
        if (continuous_ != ContinuousState::Inactive) {
            progress_.Close();
            SetStage(Stage::Idle);
            continuous_ = ContinuousState::Displaying;
            ShowContinuousBadge(L"连续翻译异常 · " + message);
            return;
        }
        Notice(message);
    }
    void Notice(const std::wstring& text) {
        try {
            if (currentMonitor_.id.empty())
                currentMonitor_ = MonitorAtCursor();
            if (!Valid(anchor_)) {
                anchor_ = currentMonitor_.rect;
                anchor_.left += 40;
                anchor_.top += 40;
                anchor_.right = anchor_.left + 420;
                anchor_.bottom = anchor_.top + 70;
            }
            overlay_.Notice(text, anchor_, currentMonitor_, [this] { Post([this] { CloseOverlay(); }); });
            SetTimer(window_, HideTimer, 4000, nullptr);
        } catch (...) { /* The settings window retains the current error if rendering fails. */
        }
        progress_.Close();
        SetStage(Stage::Idle);
    }
    void StartProgress(unsigned long long id, const Image& screenshot) {
        if (id != generation_)
            return;
        if (!progress_.Count()) {
            RECT local = anchor_;
            OffsetRect(&local, -currentMonitor_.rect.left, -currentMonitor_.rect.top);
            const bool continuous = continuous_ != ContinuousState::Inactive;
            progress_.Progress(
                anchor_, currentMonitor_, Crop(screenshot, local),
                [this, id] {
                    Post([this, id] {
                        if (id == generation_) {
                            if (continuous_ != ContinuousState::Inactive)
                                ExitContinuous();
                            else
                                Cancel();
                        }
                    });
                },
                settings_, continuous,
                [this, id, continuous] {
                    Post([this, id, continuous] {
                        if (id == generation_)
                            Begin(false, true, continuous);
                    });
                });
            overlay_.CloseContinuousBadge();
        }
        SetStage(Stage::Preprocessing);
    }
    void CloseOverlay() {
        KillTimer(window_, HideTimer);
        overlay_.Close();
        progress_.Close();
        SetStage(Stage::Idle);
    }
    void Cancel() {
        if (!worker_.Idle()) {
            DiagnosticContext context(generation_);
            DiagnosticEvent("cancel_requested");
        }
        ++generation_;
        worker_.Cancel();
        if (apiActive_) {
            apiActive_ = false;
            settingsWindow_.ApiFinished({}, L"API 操作已取消。", false);
        }
        selection_.Close();
        CloseOverlay();
    }
    bool CommitSelection(const Monitor& monitor, std::optional<RECT> chosen, bool reselect, bool continuous) {
        if (!chosen)
            return false;
        auto region = NormalizeRegion(monitor, *chosen);
        anchor_ = *chosen;
        sessionRegion_ = std::move(region);
        sessionContinuous_ = continuous;
        if (reselect)
            ContextChanged();
        return true;
    }
    void RunTranslationTask(unsigned long long id, const Monitor& monitor, RECT region,
                            std::shared_ptr<Image> existing = nullptr) {
        auto options = settings_;
        auto history = context_.Get(options.contextEnabled, options.contextSize);
        auto secret = std::make_shared<Secret>(key_);
        const bool recordBaseline = continuous_ != ContinuousState::Inactive && !baselineFrame_;
        const auto epoch = continuousEpoch_;
        worker_.Submit([this, id, epoch, monitor, region, options, history, secret, recordBaseline,
                        existing = std::move(existing)](std::stop_token stop) mutable {
            DiagnosticContext diagnosticContext(id);
            try {
                ComScope com;
                if (!existing) {
                    DiagnosticSpan capture("capture");
                    try {
                        CheckStop(stop);
                        DwmFlush();
                        existing = std::make_shared<Image>(CaptureMonitor(monitor, stop));
                        CheckStop(stop);
                        capture.End("ok", {{"width", existing->width}, {"height", existing->height}});
                    } catch (const Cancelled&) {
                        capture.End("cancelled");
                        throw;
                    } catch (const AppError& e) {
                        capture.Fail(e);
                        throw;
                    } catch (...) {
                        capture.End("error");
                        throw AppError("capture", "屏幕截图失败");
                    }
                } else if (recordBaseline) {
                    DwmFlush();
                }
                if (recordBaseline) {
                    try {
                        auto baseline = CaptureRegionQuick(monitor, region, stop);
                        Post([this, id, epoch, baseline = std::move(baseline)]() mutable {
                            if (id == generation_ && epoch == continuousEpoch_ &&
                                continuous_ == ContinuousState::Translating && !baselineFrame_)
                                baselineFrame_ = std::move(baseline);
                        });
                    } catch (const Cancelled&) {
                        throw;
                    } catch (...) {
                    }
                }
                Translate(id, monitor, std::move(existing), region, options, history, secret->value, stop);
            } catch (const Cancelled&) {
            } catch (const AppError& e) {
                Post([this, id, e] {
                    if (id == generation_)
                        Error(e);
                });
            } catch (...) {
                Post([this, id] {
                    if (id == generation_)
                        Error(AppError("translation", "翻译处理失败"));
                });
            }
        });
    }
    void OnContinuousClick(bool down) {
        if (continuous_ == ContinuousState::Inactive || continuous_ == ContinuousState::Selecting)
            return;
        if (down) {
            if (continuous_ == ContinuousState::Displaying || continuous_ == ContinuousState::Translating) {
                ++generation_;
                worker_.Cancel();
                CloseOverlay();
                EnterContinuousWaiting();
            } else if (continuous_ == ContinuousState::Waiting) {
                ResetContinuousStability();
            }
        } else if (continuous_ == ContinuousState::Waiting) {
            ResetContinuousStability();
        }
    }
    void HandleMonitorSample(Image sample) {
        if (continuous_ == ContinuousState::Translating || continuous_ == ContinuousState::Displaying) {
            if (!baselineFrame_)
                return;
            if (FramesMatch(*baselineFrame_, sample))
                return;
            // Any region pixel change during translation or display exits display/translating and waits
            // for the new page to settle across 3 consecutive 0.1 s checks.
            ++generation_;
            worker_.Cancel();
            CloseOverlay();
            baselineFrame_.reset();
            lastSample_ = std::move(sample);
            stableCount_ = 0;
            continuous_ = ContinuousState::Waiting;
            SetStage(Stage::Idle);
            ShowContinuousBadge(L"连续翻译中 · 检测到画面变化");
            return;
        }
        if (continuous_ != ContinuousState::Waiting)
            return;
        if (!lastSample_) {
            lastSample_ = std::move(sample);
            stableCount_ = 0;
            return;
        }
        if (FramesMatch(*lastSample_, sample)) {
            ++stableCount_;
            lastSample_ = std::move(sample);
            if (stableCount_ >= 3) {
                baselineFrame_ = std::move(lastSample_);
                lastSample_.reset();
                stableCount_ = 0;
                continuous_ = ContinuousState::Translating;
                ShowContinuousBadge(L"连续翻译中 · 正在识别文字...");
                SetStage(Stage::Capture);
                RunTranslationTask(++generation_, currentMonitor_, anchor_);
            }
        } else {
            stableCount_ = 0;
            lastSample_ = std::move(sample);
        }
    }
    void OnPollTimer() {
        if (continuous_ == ContinuousState::Inactive || continuous_ == ContinuousState::Selecting ||
            monitorInFlight_ || !Valid(anchor_))
            return;
        if (continuous_ == ContinuousState::Translating && !baselineFrame_)
            return;
        if (continuous_ == ContinuousState::Displaying && (!baselineFrame_ || !overlay_.CaptureExcluded()))
            return;
        monitorInFlight_ = true;
        const auto epoch = continuousEpoch_;
        const auto monitor = currentMonitor_;
        const auto region = anchor_;
        monitorWorker_.Submit([this, epoch, monitor, region](std::stop_token stop) {
            try {
                ComScope com;
                auto sample = CaptureRegionQuick(monitor, region, stop);
                CheckStop(stop);
                Post([this, epoch, sample = std::move(sample)]() mutable {
                    if (epoch != continuousEpoch_ || continuous_ == ContinuousState::Inactive ||
                        continuous_ == ContinuousState::Selecting)
                        return;
                    monitorInFlight_ = false;
                    HandleMonitorSample(std::move(sample));
                });
            } catch (const Cancelled&) {
            } catch (...) {
                Post([this, epoch] {
                    if (epoch == continuousEpoch_)
                        monitorInFlight_ = false;
                });
            }
        });
    }
    void Begin(bool fixed, bool reselect = false, std::optional<bool> continuousOverride = std::nullopt) {
        usedResources_ = true;
        const bool continuous =
            continuousOverride.value_or(fixed && !reselect && sessionRegion_ ? sessionContinuous_ : false);
        ExitContinuous();
        auto id = generation_;
        if (key_.empty()) {
            ShowSettings();
            Notice(L"请先在 API 页面填写密钥并保存设置");
            return;
        }
        auto monitors = EnumerateMonitors();
        std::optional<RECT> region;
        if (fixed && !reselect && sessionRegion_) {
            region = RestoreRegion(*sessionRegion_, monitors);
            if (region) {
                auto it = std::find_if(monitors.begin(), monitors.end(),
                                       [&](auto& m) { return m.id == sessionRegion_->monitorId; });
                if (it != monitors.end())
                    currentMonitor_ = *it;
            } else {
                sessionRegion_.reset();
                SaveState();
            }
        }
        if (!region)
            currentMonitor_ = MonitorAtCursor();
        auto monitor = currentMonitor_;
        anchor_ = region.value_or(monitor.rect);
        // Hide settings before the compositor is flushed so it cannot enter the frozen screenshot.
        if (settingsWindow_.Window())
            ShowWindow(settingsWindow_.Window(), SW_HIDE);
        SetStage(Stage::Capture);
        if (region) {
            if (continuous) {
                sessionContinuous_ = true;
                continuous_ = ContinuousState::Translating;
                InstallContinuousHooks();
                SetTimer(window_, PollTimer, 100, nullptr);
            }
            RunTranslationTask(id, monitor, *region);
            return;
        }
        if (continuous)
            continuous_ = ContinuousState::Selecting;
        worker_.Submit([this, id, monitor, reselect, continuous](std::stop_token stop) {
            DiagnosticContext diagnosticContext(id);
            DiagnosticSpan capture("capture");
            try {
                ComScope com;
                CheckStop(stop);
                DwmFlush();
                auto image = std::make_shared<Image>(CaptureMonitor(monitor, stop));
                CheckStop(stop);
                capture.End("ok", {{"width", image->width}, {"height", image->height}});
                Post([this, id, monitor, image, reselect, continuous] {
                    if (id != generation_)
                        return;
                    SetStage(Stage::Selecting);
                    selection_.Show(
                        monitor, image, [this, id, monitor, image, reselect, continuous](std::optional<RECT> chosen) {
                            if (id != generation_)
                                return;
                            if (chosen) {
                                anchor_ = *chosen;
                                try {
                                    StartProgress(id, *image);
                                } catch (const AppError& e) {
                                    Error(e);
                                    return;
                                } catch (...) {
                                    Error(AppError("overlay", "无法显示翻译进度"));
                                    return;
                                }
                            }
                            Post([this, id, monitor, image, chosen, reselect, continuous] {
                                if (id != generation_)
                                    return;
                                if (!CommitSelection(monitor, chosen, reselect, continuous)) {
                                    if (continuous)
                                        ExitContinuous();
                                    else
                                        SetStage(Stage::Idle);
                                    return;
                                }
                                if (continuous) {
                                    continuous_ = ContinuousState::Translating;
                                    InstallContinuousHooks();
                                    SetTimer(window_, PollTimer, 100, nullptr);
                                }
                                RunTranslationTask(id, monitor, *chosen, image);
                            });
                        });
                });
            } catch (const Cancelled&) {
                capture.End("cancelled");
            } catch (const AppError& e) {
                capture.Fail(e);
                Post([this, id, e] {
                    if (id == generation_)
                        Error(e);
                });
            } catch (...) {
                capture.End("error");
                Post([this, id] {
                    if (id == generation_)
                        Error(AppError("capture", "屏幕截图失败"));
                });
            }
        });
    }
    void TranslateCropped(unsigned long long id, const Monitor& monitor, Image cropped, RECT region,
                          const Settings& options, const std::vector<ContextItem>& history,
                          const std::string& key, std::stop_token stop) {
        auto prepared = PrepareTranslation(
            std::move(cropped), region, monitor, options, history, key, stop, providerFactory_,
            [this, id](TranslationPhase phase) {
                Post([this, id, phase] {
                    if (id != generation_)
                        return;
                    // One fixed line per stage: the recognition pass names itself, and the request that
                    // follows reports translation. Placement finishes so quickly that it keeps the last line.
                    if (phase == TranslationPhase::Recognizing) {
                        progress_.SetProgressText(L"文字识别中 · 点击 × 取消");
                        SetStage(Stage::Preprocessing);
                    } else if (phase == TranslationPhase::Requesting) {
                        progress_.SetProgressText(L"正在翻译内容 · 可点击「关闭」或按 Esc 取消");
                        SetStage(Stage::Requesting);
                    } else {
                        SetStage(Stage::Layout);
                    }
                });
            });
        Post([this, id, prepared = std::move(prepared), options, monitor] {
            DisplayTranslation(id, prepared, options, monitor);
        });
    }
    void Translate(unsigned long long id, const Monitor& monitor, std::shared_ptr<Image> screenshot,
                   RECT region, const Settings& options, const std::vector<ContextItem>& history,
                   const std::string& key, std::stop_token stop) {
        DiagnosticContext context(id);
        // The record starts before any other preparation work so the first measured stage lands in it.
        TraceSession trace("screen_translation",
                           {{"provider", options.provider},
                            {"model", options.model},
                            {"route", EffectiveTranslationMode(options) == TranslationOcrText ? "text"
                                                                                               : "vision"},
                            {"quality", options.quality},
                            {"ocr_mode", options.ocrMode},
                            {"target_language", options.screenTargetLanguage},
                            {"timeout_s", options.timeoutSeconds},
                            {"translate_all", options.translateAll},
                            {"context_enabled", options.contextEnabled},
                            {"context_items", history.size()},
                            {"proxy_mode", options.proxyMode},
                            {"prompt", options.prompt}});
        Post([this, id, screenshot] { StartProgress(id, *screenshot); });
        RECT local = region;
        OffsetRect(&local, -monitor.rect.left, -monitor.rect.top);
        auto cropped = Crop(*screenshot, local);
        screenshot.reset();
        TranslateCropped(id, monitor, std::move(cropped), region, options, history, key, stop);
    }
    void RetranslateOverlay(int newOcrMode, const std::string& newTargetLanguage) {
        if (key_.empty()) {
            overlay_.SetWarning(L"请先在设置中填写密钥");
            return;
        }
        auto sourceImage = overlay_.SourceImage();
        if (!sourceImage || sourceImage->width <= 0 || sourceImage->height <= 0 || !Valid(anchor_)) {
            overlay_.SetWarning(L"当前视图缺少原图，请重新框选");
            return;
        }
        Settings updated = settings_;
        updated.ocrMode = newOcrMode;
        updated.screenTargetLanguage = newTargetLanguage;
        updated.textTargetLanguage = newTargetLanguage;
        try {
            ValidateSettings(updated);
            store_.SaveSettings(updated);
        } catch (const AppError& e) {
            overlay_.SetWarning(Wide(e.what()));
            return;
        }
        settings_ = std::move(updated);
        settingsWindow_.SyncScreenLanguages(settings_.ocrMode, settings_.screenTargetLanguage);
        const auto region = anchor_;
        const auto monitor = currentMonitor_;
        Cancel();
        anchor_ = region;
        currentMonitor_ = monitor;
        const auto id = generation_;
        const auto options = settings_;
        const auto history = context_.Get(options.contextEnabled, options.contextSize);
        auto secret = std::make_shared<Secret>(key_);
        const bool passthrough = continuous_ != ContinuousState::Inactive;
        progress_.Progress(
            region, monitor, *sourceImage,
            [this, id] {
                Post([this, id] {
                    if (id == generation_) {
                        if (continuous_ != ContinuousState::Inactive)
                            ExitContinuous();
                        else
                            Cancel();
                    }
                });
            },
            options, passthrough,
            [this, id, passthrough] {
                Post([this, id, passthrough] {
                    if (id == generation_)
                        Begin(false, true, passthrough);
                });
            });
        SetStage(Stage::Preprocessing);
        worker_.Submit([this, id, monitor, sourceImage, region, options, history, secret](std::stop_token stop) {
            DiagnosticContext context(id);
            try {
                ComScope com;
                Image copy = *sourceImage;
                TranslateCropped(id, monitor, std::move(copy), region, options, history, secret->value, stop);
            } catch (const Cancelled&) {
            } catch (const AppError& e) {
                Post([this, id, e] {
                    if (id == generation_)
                        Error(e);
                });
            } catch (...) {
                Post([this, id] {
                    if (id == generation_)
                        Error(AppError("translation", "屏幕翻译失败"));
                });
            }
        });
    }
    void DisplayTranslation(unsigned long long id, const PreparedTranslation& prepared,
                            const Settings& options, const Monitor& monitor) {
        if (id != generation_)
            return;
        const bool passthrough = continuous_ != ContinuousState::Inactive;
        if (prepared.result.segments.empty()) {
            if (passthrough) {
                progress_.Close();
                SetStage(Stage::Idle);
                continuous_ = ContinuousState::Displaying;
                ShowContinuousBadge(L"连续翻译中 · 当前区域未识别到文字");
                return;
            }
            Notice(L"未识别到文字");
            return;
        }
        const auto& result = prepared.result;
        const auto& blocks = prepared.blocks;
        const auto ocrMs = prepared.ocrMs, apiMs = prepared.apiMs, total = prepared.totalMs;
        const bool textOnly = prepared.textOnly;
        DiagnosticContext context(id);
        DiagnosticSpan display("display");
        overlay_.Show(
            blocks, options, monitor.dpi,
            [this, id] {
                Post([this, id] {
                    if (id == generation_) {
                        if (continuous_ != ContinuousState::Inactive)
                            ExitContinuous();
                        else
                            CloseOverlay();
                    }
                });
            },
            [this, id] {
                Post([this, id] {
                    if (id == generation_ && stage_ == Stage::Displaying)
                        Begin(false, true, continuous_ != ContinuousState::Inactive);
                });
            },
            passthrough ? std::function<void(RECT)>{}
                        : [this, id, monitor](RECT region) {
                              Post([this, id, monitor, region] {
                                  if (id != generation_ || stage_ != Stage::Displaying)
                                      return;
                                  // The fixed path closes the old overlay before capturing fresh screen pixels.
                                  sessionRegion_ = NormalizeRegion(monitor, region);
                                  sessionContinuous_ = false;
                                  Begin(true);
                              });
                          },
            passthrough);
        progress_.Close();
        SetStage(Stage::Displaying);
        // The toolbar carries one one-off vision pass: it reads the same selection straight from the image
        // and lands as another switchable view, leaving the saved translation flow untouched.
        overlay_.SetAiRequest([this] { RequestAiView(); });
        overlay_.SetLanguageHandler([this, id](int nextOcrMode, std::string nextTarget) {
            Post([this, id, nextOcrMode, nextTarget = std::move(nextTarget)] {
                if (id == generation_ && stage_ == Stage::Displaying)
                    RetranslateOverlay(nextOcrMode, nextTarget);
            });
        });
        if (passthrough) {
            overlay_.CloseContinuousBadge();
            continuous_ = ContinuousState::Displaying;
        } else {
            // The result takes over the screen: everything outside the selection dims and keeps its input, and
            // the toolbar's close button or Esc ends the view.
            try {
                overlay_.ShowFocusShade();
            } catch (const AppError& e) {
                overlay_.SetWarning(Wide(e.what()));
            }
        }
        if (options.contextEnabled) {
            context_.Add(result, options.contextSize);
            SetTimer(window_, ContextTimer, 30 * 60 * 1000, nullptr);
        }
        settingsWindow_.Status(std::wstring(result.untranslatedSegments || result.ignoredSegments
                                                ? L"翻译结果含异常 · "
                                                : L"翻译完成 · ") +
                               (textOnly ? L"文字翻译" : L"视觉翻译 + 本地定位") + L" · OCR " +
                               std::to_wstring(ocrMs) +
                               L" ms · API " + std::to_wstring(apiMs) + L" ms · 请求 " +
                               std::to_wstring(result.requestCount) + L" 次 · 本次处理 " +
                               std::to_wstring(total) + L" ms");
        display.End();
    }
    // One vision pass over the same selection. The request reads the captured image directly, carries its
    // own upload bound, and comes back as a view the toolbar switches to beside the recognized result. It
    // writes nothing to the settings, so the next translation runs the recognition route again.
    void RequestAiView() {
        if (key_.empty()) {
            overlay_.SetWarning(L"请先在设置中填写密钥");
            return;
        }
        if (!Valid(anchor_)) {
            overlay_.SetWarning(L"请先框选区域");
            return;
        }
        auto sourceImage = overlay_.SourceImage();
        if (!sourceImage || sourceImage->width <= 0 || sourceImage->height <= 0) {
            overlay_.SetWarning(L"当前视图缺少原图，请重新框选");
            return;
        }
        const auto id = generation_;
        const auto region = anchor_;
        const auto monitor = currentMonitor_;
        auto options = settings_;
        auto secret = std::make_shared<Secret>(key_);
        worker_.Submit([this, id, region, monitor, options, secret,
                        sourceImage = std::move(sourceImage)](std::stop_token stop) {
            DiagnosticContext context(id);
            try {
                ComScope com;
                Image cropped = *sourceImage;
                Settings routed = options;
                routed.translationMode = TranslationAiVision;
                // A long edge of 1600 px keeps a full-screen selection inside one request, while a dialog
                // smaller than that uploads at its captured size.
                routed.quality = 1;
                const auto connection = SelectTranslationConnection(routed);
                auto provider = providerFactory_(connection.provider);
                auto data = Base64(EncodeJpeg(cropped, routed.quality, stop));
                auto result = provider->TranslateLocated(data, {}, connection, secret->value, stop, {});
                FlagUntranslatedPassages(result);
                if (result.segments.empty())
                    throw AppError("translation", "模型没有返回可翻译文字");
                // A vision answer describes passages, the local detector describes screen rows. Pairing
                // them adopts the measured rows wherever both agree, which is the placement the main line
                // already uses; passages only the model sees keep its rectangle.
                data.clear();
                data.shrink_to_fit();
                try {
                    auto detected = DetectOcrText(cropped, stop, options.ocrMode);
                    std::erase_if(detected.regions, [](const auto& region) { return region.text.empty(); });
                    const std::string screenHint =
                        (options.ocrMode == OcrLocalAuto || options.ocrMode == OcrLatin)
                            ? std::string{}
                            : std::string(TargetIdFromOcrMode(options.ocrMode));
                    PopulateDetectedLanguages(result, detected.regions, options.ocrMode, screenHint);
                    if (!detected.regions.empty()) {
                        auto report =
                            AttachSourceGeometry(result, cropped.width, cropped.height, detected.regions);
                        result.uncoveredLines = std::move(report.uncoveredLines);
                    }
                } catch (const Cancelled&) {
                    throw;
                } catch (...) {
                    const std::string screenHint =
                        (options.ocrMode == OcrLocalAuto || options.ocrMode == OcrLatin)
                            ? std::string{}
                            : std::string(TargetIdFromOcrMode(options.ocrMode));
                    PopulateDetectedLanguages(result, {}, options.ocrMode, screenHint);
                    // Recognition assists placement here: the model rectangles stand when it yields nothing.
                }
                CheckStop(stop);
                Post([this, id, region, monitor, result = std::move(result),
                      cropped = std::move(cropped)] {
                    if (id != generation_ || stage_ != Stage::Displaying)
                        return;
                    try {
                        // The source stays with the result: the override layer rebuilds the artwork behind
                        // each replaced passage, which needs the captured pixels.
                        auto block =
                            BuildOverlay(result, std::move(cropped), region, monitor, settings_, true).front();
                        overlay_.ShowAiResult(std::move(block));
                    } catch (const AppError& e) {
                        overlay_.SetWarning(Wide(e.what()));
                    }
                });
            } catch (const Cancelled&) {
            } catch (const AppError& e) {
                Post([this, id, e] {
                    if (id == generation_)
                        overlay_.SetWarning(Wide(e.what()));
                });
            } catch (...) {
                Post([this, id] {
                    if (id == generation_)
                        overlay_.SetWarning(L"AI 识图未完成，请重试");
                });
            }
        });
    }
    std::wstring Apply(const Settings& next, const std::string& key) {
        ValidateSettings(next);
        if (!next.tray && !next.hotkeys[2].key)
            return L"关闭托盘前，请设置“打开设置”快捷键。";
        Settings updated = next;
        updated.encryptedKey = ProtectSecret(key);
        auto hotkeyError = hotkeys_->Apply(updated);
        if (!hotkeyError.empty())
            return hotkeyError;
        try {
            SetStartup(updated.startup, Executable());
            store_.SaveSettings(updated);
        } catch (...) {
            hotkeys_->Apply(settings_);
            try {
                SetStartup(settings_.startup, Executable());
            } catch (...) {
            }
            throw;
        }
        ExitContinuous();
        ContextChanged();
        settings_ = std::move(updated);
        // A saved toggle takes effect immediately: the next translation writes or skips its record.
        if (settings_.traceEnabled != TraceEnabled())
            InitializeTrace(store_.Root() / L"logs" / L"trace", settings_.traceEnabled, 30);
        if (!key_.empty())
            SecureZeroMemory(key_.data(), key_.size());
        key_ = key;
        recovery_.clear();
        SaveState();
        UpdateTray();
        return {};
    }
    void ApiOperation(bool fetch, const Settings& input, const std::string& secretText) {
        usedResources_ = true;
        Settings options = input;
        options.encryptedKey.clear();
        ValidateSettings(options);
        Cancel();
        apiActive_ = true;
        settingsWindow_.ApiBusy(true);
        auto id = generation_;
        auto secret = std::make_shared<Secret>(secretText);
        worker_.Submit([this, id, fetch, options, secret](std::stop_token stop) {
            DiagnosticContext context(id);
            DiagnosticSpan diagnostic(fetch ? "model_list" : "connection_test",
                                      {{"provider", options.provider},
                                       {"model", options.model},
                                       {"timeout_s", options.timeoutSeconds}});
            try {
                ComScope com;
                auto started = std::chrono::steady_clock::now();
                std::vector<std::string> models;
                std::wstring message;
                if (fetch) {
                    models = FetchModels(options, secret->value, stop);
                    message = L"已获取 " + std::to_wstring(models.size()) +
                              L" 个模型。请选择模型，再点击测试 API；模型列表不代表图片识别能力。";
                } else {
                    TranslationResult result;
                    if (EffectiveTranslationMode(options) == TranslationAiVision) {
                        auto vision = SelectTranslationConnection(options);
                        result = TestVisionApi(vision, secret->value, stop);
                        message = L"AI 识图翻译连接自检通过。";
                    } else {
                        result = TestTextApi(options, secret->value, stop);
                        message = L"文字翻译连接自检通过。";
                    }
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
                    message += L"耗时 " + std::to_wstring(ms) + L" ms。";
                    for (const auto& segment : result.segments) {
                        message += L"\r\n" + Wide(segment.original) + L" → " + Wide(segment.translated);
                        if (message.size() > 1200)
                            break;
                    }
                }
                CheckStop(stop);
                diagnostic.End();
                Post([this, id, fetch, models = std::move(models), message] {
                    if (id != generation_)
                        return;
                    apiActive_ = false;
                    settingsWindow_.ApiFinished(models, message, fetch);
                });
            } catch (const Cancelled&) {
                diagnostic.End("cancelled");
            } catch (const AppError& e) {
                diagnostic.Fail(e);
                auto message = Wide(Sanitize(e.what(), secret->value));
                Post([this, id, fetch, message] {
                    if (id != generation_)
                        return;
                    apiActive_ = false;
                    settingsWindow_.ApiFinished({}, L"操作失败：" + message, fetch);
                });
            } catch (...) {
                diagnostic.End("error");
                Post([this, id, fetch] {
                    if (id != generation_)
                        return;
                    apiActive_ = false;
                    settingsWindow_.ApiFinished({}, L"操作失败，请检查 API 地址、密钥和所选模型。", fetch);
                });
            }
        });
    }
    void ShowSettings(bool visible = true) {
        if (visible && continuous_ != ContinuousState::Inactive)
            ExitContinuous();
        usedResources_ = true;
        KillTimer(window_, IdleTimer);
        SettingsCallbacks cb;
        cb.apply = [this](const Settings& s, const std::string& k) { return Apply(s, k); };
        cb.hotkeyEditing = [this](bool editing) {
            if (editing)
                hotkeys_->Suspend();
            else if (auto error = hotkeys_->Resume(); !error.empty())
                settingsWindow_.Status(error);
        };
        cb.closed = [this](RECT r, int tab, int settingsTab) {
            if (apiActive_)
                Cancel();
            state_.window = r;
            state_.tab = tab;
            state_.settingsTab = settingsTab;
            SaveState();
            ArmIdle();
        };
        cb.api = [this](bool fetch, const Settings& s, const std::string& secret) {
            ApiOperation(fetch, s, secret);
        };
        cb.targetLanguage = [this](const std::string& language) {
            auto updated = settings_;
            updated.textTargetLanguage = language;
            updated.screenTargetLanguage = language;
            store_.SaveSettings(updated);
            settings_.textTargetLanguage = language;
            settings_.screenTargetLanguage = language;
        };
        cb.workbenchLanguages = [this](const std::string& source, const std::string& target) {
            auto updated = settings_;
            updated.sourceLanguage = source.empty() ? "auto" : source;
            updated.screenTargetLanguage = target;
            updated.textTargetLanguage = target;
            ValidateSettings(updated);
            store_.SaveSettings(updated);
            settings_ = std::move(updated);
        };
        cb.traceChanged = [this](bool enabled) {
            auto updated = settings_;
            updated.traceEnabled = enabled;
            store_.SaveSettings(updated);
            settings_.traceEnabled = enabled;
            if (settings_.traceEnabled != TraceEnabled())
                InitializeTrace(store_.Root() / L"logs" / L"trace", settings_.traceEnabled, 30);
        };
        cb.exportLogs = [this](HWND owner) { return ExportRecentLogs(owner); };
        cb.action = [this](SettingsAction action) {
            switch (action) {
            case SettingsAction::ClearContext:
                ContextChanged();
                break;
            case SettingsAction::Translate:
                Begin(false, false, false);
                break;
            case SettingsAction::ContinuousTranslate:
                Begin(false, false, true);
                break;
            case SettingsAction::FixedTranslate:
                Begin(true);
                break;
            }
        };
        settingsWindow_.Show(settings_, key_, state_, std::move(cb), visible);
        if (!recovery_.empty())
            settingsWindow_.Status(recovery_);
    }
    void TrayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu)
            throw AppError("tray", "无法创建托盘菜单");
        AppendMenuW(menu, MF_STRING, OpenSettings, L"打开设置");
        AppendMenuW(menu, MF_STRING, OpenLogs, L"导出日志");
        AppendMenuW(menu, MF_STRING, ExitApp, L"退出");
        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(window_);
        int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        Action(action);
    }
    std::wstring ExportRecentLogs(HWND parent = nullptr) {
        const auto logsRoot = store_.Root() / L"logs";
        if (!HasExportableTraces(logsRoot))
            return L"当前暂无可导出的翻译日志，请先开启“记录日志”并进行翻译。";
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t fileBuf[32768]{};
        swprintf_s(fileBuf, L"翻译日志-%04u%02u%02u-%02u%02u%02u.zip", st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = parent && IsWindow(parent) ? parent : window_;
        ofn.lpstrFilter = L"ZIP 压缩包 (*.zip)\0*.zip\0全部文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = DWORD(std::size(fileBuf));
        ofn.lpstrDefExt = L"zip";
        ofn.lpstrTitle = L"选择翻译日志保存位置（导出最近 30 次）";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&ofn))
            return {};
        std::filesystem::path outPath(fileBuf);
        if (outPath.extension().empty())
            outPath += L".zip";
        const auto res = ExportRecentTracesArchive(logsRoot, outPath, 30);
        if (res.sessions > 0)
            return L"已导出最近 " + std::to_wstring(res.sessions) + L" 次翻译日志至：" + outPath.wstring();
        return L"已导出诊断日志至：" + outPath.wstring();
    }
    void Action(int action) {
        switch (action) {
        case OpenSettings:
            ShowSettings();
            break;
        case ExitApp:
            DestroyWindow(window_);
            break;
        case OpenLogs: {
            try {
                SetForegroundWindow(window_);
                const auto msg = ExportRecentLogs(settingsWindow_.Window());
                if (!msg.empty()) {
                    if (settingsWindow_.Window() && IsWindowVisible(settingsWindow_.Window()))
                        settingsWindow_.Status(msg);
                    else
                        Notice(msg);
                }
            } catch (const AppError& e) {
                Notice(Wide(e.what()));
            } catch (...) {
                Notice(L"导出日志失败，请检查保存路径权限。");
            }
            break;
        }
        }
    }
    LRESULT Message(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == taskbarCreated_) {
            trayAdded_ = false;
            UpdateTray();
            return 0;
        }
        switch (m) {
        case UiMessage:
            Dispatch();
            return 0;
        case ContinuousClickMessage:
            OnContinuousClick(w != 0);
            return 0;
        case ContinuousEscapeMessage:
            ExitContinuous();
            return 0;
        case WM_HOTKEY:
            if (hotkeys_->Suspended())
                return 0;
            switch (w) {
            case 1:
                Begin(false, false, false);
                break;
            case 2:
                Begin(true);
                break;
            case 3:
                ShowSettings();
                break;
            case 4:
                Begin(false, false, true);
                break;
            }
            return 0;
        case TrayMessage:
            if (l == WM_LBUTTONDBLCLK)
                ShowSettings();
            else if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU)
                TrayMenu();
            return 0;
        case WM_COPYDATA:
            ShowSettings();
            return TRUE;
        case WM_TIMER:
            if (w == HideTimer) {
                if (!overlay_.Dragging())
                    CloseOverlay();
            } else if (w == ContextTimer)
                ContextChanged();
            else if (w == IdleTimer)
                TryIdle();
            else if (w == PollTimer)
                OnPollTimer();
            return 0;
        case WM_DISPLAYCHANGE:
            ExitContinuous();
            currentMonitor_ = {};
            if (sessionRegion_ && !RestoreRegion(*sessionRegion_, EnumerateMonitors())) {
                sessionRegion_.reset();
                SaveState();
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (w)
                DestroyWindow(h);
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            ExitContinuous();
            {
                std::lock_guard lock(queueMutex_);
                shuttingDown_ = true;
            }
            worker_.Stop();
            settingsWindow_.Close();
            SaveState();
            if (trayAdded_) {
                NOTIFYICONDATAW ni{sizeof(ni)};
                ni.hWnd = h;
                ni.uID = 1;
                Shell_NotifyIconW(NIM_DELETE, &ni);
            }
            hotkeys_.reset();
            PostQuitMessage(0);
            return 0;
        }
        }
        return DefWindowProcW(h, m, w, l);
    }

public:
    explicit App(
        bool background, std::filesystem::path dataRoot = {},
        std::function<std::unique_ptr<ITranslationProvider>(const std::string&)> factory = CreateProvider,
        std::function<bool(const std::string&)> idleRestart = {}, std::string resume = {})
        : migrateStartup_(dataRoot.empty()), store_(std::move(dataRoot)),
          providerFactory_(std::move(factory)), background_(background), idleRestart_(std::move(idleRestart)),
          resume_(std::move(resume)) {}
    ~App() {
        RemoveContinuousHooks();
        monitorWorker_.Stop();
        worker_.Stop();
        if (!key_.empty())
            SecureZeroMemory(key_.data(), key_.size());
    }
    int Run() {
        InitializeDiagnostics(store_.Root() / L"logs");
        DiagnosticEvent("application_start", {{"resumed", !resume_.empty()}});
        try {
            settings_ = store_.LoadSettings();
        } catch (const AppError&) {
            recovery_ =
                L"配置读取失败；请检查 %LOCALAPPDATA%\\ScreenAITranslator\\config.json，原文件已保留。";
        }
        // Payload tracing shares the log root; every screen translation adds one directory there.
        InitializeTrace(store_.Root() / L"logs" / L"trace", settings_.traceEnabled, 30);
        try {
            state_ = store_.LoadState();
        } catch (const AppError& e) {
            recovery_ = Wide(Sanitize(e.what(), key_));
        }
        try {
            key_ = UnprotectSecret(settings_.encryptedKey);
        } catch (const AppError&) {
            recovery_ = L"无法解密 API Key，请重新填写并保存。";
        }
        RestoreIdle();
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SAT.Controller";
        wc.lpfnWndProc = Proc;
        RegisterClassW(&wc);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"ScreenAITranslator.Controller",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
        if (!window_)
            return 1;
        hotkeys_ = std::make_unique<Hotkeys>(window_);
        auto error = hotkeys_->Apply(settings_);
        if (!error.empty()) {
            recovery_ = error;
            settings_.tray = true;
        }
        constexpr auto startupPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        wchar_t startupCommand[32768]{};
        DWORD bytes = sizeof(startupCommand);
        const bool registeredStartup =
            RegGetValueW(HKEY_CURRENT_USER, startupPath, L"ScreenAITranslator", RRF_RT_REG_SZ, nullptr,
                         startupCommand, &bytes) == ERROR_SUCCESS;
        if (!std::filesystem::exists(store_.Root() / L"config.json")) {
            settings_.startup = registeredStartup;
        }
        if (migrateStartup_ && registeredStartup) {
            const auto executable = Executable();
            const auto legacy = std::filesystem::path(executable).parent_path() / L"ScreenAITranslator.exe";
            const auto previousCommand = L"\"" + legacy.wstring() + L"\" --background";
            if (CompareStringOrdinal(startupCommand, -1, previousCommand.c_str(), -1, TRUE) == CSTR_EQUAL) {
                try {
                    SetStartup(true, executable);
                } catch (const AppError& e) {
                    if (recovery_.empty())
                        recovery_ = L"开机启动项更新失败，请重新保存设置。";
                    DiagnosticEvent("startup_migration_failed", {{"error", Sanitize(e.what(), key_)}});
                }
            }
        }
        if (!settings_.tray && !settings_.hotkeys[2].key)
            settings_.tray = true;
        UpdateTray();
        const auto now = GetTickCount64();
        if (context_.ExpiresAt() > now)
            SetTimer(window_, ContextTimer, static_cast<UINT>(context_.ExpiresAt() - now), nullptr);
        // Installer supplies startup registration; explicit Save updates it for portable runs.
        if (!background_)
            ShowSettings();
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (WinUIRuntime::ProcessMessage(msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }
};
} // namespace sat
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&controls);
    const bool background = wcsstr(command, L"--background") != nullptr;
    HANDLE mutex{};
    int exitCode = 0;
    try {
        auto resume = sat::ReceiveIdleReplacement(command);
        mutex = resume.instance;
        if (!mutex) {
            mutex = CreateMutexW(nullptr, FALSE, L"Local\\ScreenAITranslator.SingleInstance");
            if (!mutex)
                return 1;
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                if (!background) {
                    if (auto existing = FindWindowW(L"SAT.Controller", L"ScreenAITranslator.Controller")) {
                        COPYDATASTRUCT cd{};
                        DWORD_PTR result{};
                        SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd),
                                            SMTO_ABORTIFHUNG, 1000, &result);
                    }
                }
                CloseHandle(mutex);
                return 0;
            }
        }
        sat::WinUIRuntime ui(true);
        sat::App app(
            background, {}, sat::CreateProvider,
            [mutex](const std::string& payload) {
                return sat::StartIdleReplacement(sat::Executable(), mutex, payload);
            },
            std::move(resume.payload));
        exitCode = app.Run();
    } catch (...) {
        exitCode = 1;
    }
    if (mutex)
        CloseHandle(mutex);
    return exitCode;
}
