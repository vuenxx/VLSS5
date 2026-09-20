#include "App.h"
#include "SettingsWindow.h"
#include "ConfigManager.h"
#include "RTSSManager.h"
#include "GpuSelector.h"
#include "resource.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <algorithm>
#include <cmath>

// Global pointer so the static OverlayWndProc can reach the App.
static App* g_appInstance = nullptr;

static constexpr wchar_t kOverlayClass[] = L"VLSS5Overlay";

// ==========================================================================
// Ctor / Dtor
// ==========================================================================

App::App(HINSTANCE hInstance) : m_hInstance(hInstance)
{
    g_appInstance = this;

    // Register the overlay window class once.
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = nullptr;              // no GDI background — D3D owns it
    wc.lpszClassName = kOverlayClass;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

App::~App()
{
    StopOverlay();
    g_appInstance = nullptr;
}

// ==========================================================================
// InitD3D  (called once per session or on GPU switch)
// ==========================================================================

bool App::InitD3D()
{
    if (m_device) return true; // already done

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL obtained  = {};

    // Bu cihaz YAKALAMA + SUNUM cihazidir; monitoru suren kart olmalidir.
    // DLSS ayri bir GPU'da kosuyorsa o adapter D3D12Interop tarafindan secilir.
    const std::wstring& preferredGpu = ConfigManager::Get().Config().selectedGpu;
    ComPtr<IDXGIAdapter1> chosenAdapter = GpuSelector::FindAdapter(preferredGpu);

    D3D_DRIVER_TYPE driverType = chosenAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

    if (chosenAdapter)
    {
        DXGI_ADAPTER_DESC1 desc{};
        chosenAdapter->GetDesc1(&desc);
        DLSS_Log("[App] Yakalama/sunum D3D11 cihazi su GPU uzerinde: %ls", desc.Description);
    }
    else
    {
        DLSS_Log("[App] Initializing D3D11 device on default hardware adapter.");
    }

    HRESULT hr = D3D11CreateDevice(
        chosenAdapter.Get(),
        driverType,
        nullptr,
        flags,
        &requested, 1,
        D3D11_SDK_VERSION,
        &m_device, &obtained, &m_context);

    if (FAILED(hr) && chosenAdapter)
    {
        DLSS_Log("[App] Selected GPU initialization failed (0x%08X), falling back to default hardware adapter.", hr);
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            &requested, 1,
            D3D11_SDK_VERSION,
            &m_device, &obtained, &m_context);
    }

    if (FAILED(hr)) return false;

    // GPU Thread Priority: Boost our queue priority so overlay work doesn't starve
    // behind a game pegging the GPU at 90-100%.
    // Priority range: -7 to +7 (default is 0). Maximum is +7,
    // guaranteeing overlay rendering & preemption even when the game stresses the GPU to 100%.
    static constexpr INT kDefaultGpuPriority = 7;
    ComPtr<IDXGIDevice2> dxgiDevice2;
    if (SUCCEEDED(m_device.As(&dxgiDevice2)) && dxgiDevice2)
    {
        dxgiDevice2->SetGPUThreadPriority(kDefaultGpuPriority);
    }

    return true;
}

void App::SetPreferredGpu(const std::wstring& gpuName)
{
    if (m_state == AppState::Menu)
    {
        if (m_device)
        {
            m_context.Reset();
            m_device.Reset();
            DLSS_Log("[App] Preferred GPU set to '%ls'. Reset D3D11 device for next session.", gpuName.c_str());
        }
    }
}

// ==========================================================================
// CreateOverlayWindow
// ==========================================================================

// ComputeOverlayRect — overlay'in kaplamasi gereken ekran dikdortgeni
RECT App::ComputeOverlayRect() const
{
    RECT r = {};
    if (!m_targetHwnd) return r;

    if (m_fullscreenStretch)
    {
        // Tam Ekran Yap: hedefin bulundugu monitorun TAMAMI.
        // rcMonitor (rcWork degil) kullaniliyor; gorev cubugu da kapanmali.
        HMONITOR mon = MonitorFromWindow(m_targetHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (mon && GetMonitorInfoW(mon, &mi))
            return mi.rcMonitor;
    }

    // Klasik mod: hedefin gercek piksel siniri.
    // DWMWA_EXTENDED_FRAME_BOUNDS gorunmez DWM yeniden boyutlandirma golgesini ayiklar.
    if (FAILED(DwmGetWindowAttribute(
            m_targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
    {
        GetWindowRect(m_targetHwnd, &r); // fallback
    }
    return r;
}

// -----------------------------------------------------------------------
// ApplyOverlayComposition
//
// Olculen (x64/Release/vlss5_logs.log, 17:41-17:43):
//   render  ort ~1.7 ms   <- tum boru hatti: yakalama, downscale, optik akis,
//                            DLSS-NR degerlendirmesi, kompozit
//   present ort 40-95 ms  <- yalnizca DXGI Present()
//   patolojik durumda present = 1000.2-1000.7 ms VE ayni anda WGC kare araligi
//   da 1000 ms: tum kompozisyon zinciri 1 Hz'e dusuyor.
//
// Yani darbogaz boru hattinda degil, DWM kompozisyonunda. Bu fonksiyon overlay'in
// DWM icin hangi sinifta oldugunu degistirir, boylece hangi modun bu makinede ve
// bu oyunda daha ucuz oldugu olculebilir.
// -----------------------------------------------------------------------
void App::ApplyOverlayComposition(int mode)
{
    if (!m_overlayHwnd) return;
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    if (m_overlayMode == mode) return;

    LONG_PTR ex = GetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE);

    if (mode == 2)
    {
        // LAYERED tamamen kalkar. WS_EX_TRANSPARENT hit-test gecirgenligi icin
        // LAYERED'a bagli degildir, fare gecirgenligi korunur.
        ex &= ~WS_EX_LAYERED;
        SetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE, ex);
    }
    else
    {
        if (!(ex & WS_EX_LAYERED))
        {
            ex |= WS_EX_LAYERED;
            SetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE, ex);
        }
        // 254: goze 255 ile ayni ama DWM altindaki oyunu "tamamen kapatildi"
        // saymaz, oyun kendini throttle etmez. Bedeli tam ekran alpha karisimi.
        // 255: DWM pencereyi opak sayabilir ve daha ucuz bir yol secebilir.
        SetLayeredWindowAttributes(m_overlayHwnd, 0, (mode == 0) ? 254 : 255, LWA_ALPHA);
    }

    // Ex-style degisiminin kompozitore islemesi icin cerceve yeniden hesaplanmali.
    SetWindowPos(m_overlayHwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    static const char* kNames[3] = { "0 Uyumlu (layered, alpha 254)",
                                     "1 Opak (layered, alpha 255)",
                                     "2 En Hizli (layered yok)" };
    DLSS_Log("[App] Overlay kompozisyon modu: %s", kNames[mode]);

    m_overlayMode = mode;
}

bool App::CreateOverlayWindow(HWND targetHwnd)
{
    RECT r = ComputeOverlayRect();
    m_lastTargetRect = r;
    m_lastMonitor    = MonitorFromWindow(targetHwnd, MONITOR_DEFAULTTONEAREST);

    int x = r.left;
    int y = r.top;
    int w = r.right  - r.left;
    int h = r.bottom - r.top;

    // WS_EX_TRANSPARENT — mouse events pass through to the target app.
    // WS_EX_LAYERED     — REQUIRED by Windows for WS_EX_TRANSPARENT hit-test passthrough.
    // WS_EX_NOACTIVATE  — overlay never steals keyboard focus.
    // WS_EX_TOPMOST     — always above the target window.
    m_overlayHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kOverlayClass,
        L"VLSS5 Overlay",
        WS_POPUP,               // no title bar, no border
        x, y, w, h,
        nullptr, nullptr, m_hInstance, nullptr);

    if (!m_overlayHwnd) return false;

    // Kompozisyon sinifi artik yapilandirilabilir; ayrinti icin Dlss5Config::overlayMode.
    ApplyOverlayComposition(ConfigManager::Get().Config().overlayMode);

    // Ekran görüntüsü (SS), Win+Shift+S, PrintScreen ve kayıt araçlarında DLSS 5 çıktısının
    // net şekilde görünmesi için WDA_NONE kullanıyoruz.
    // WGC hedef oyunu doğrudan pencere tanıtıcısı (CreateForWindow) ile izole yakaladığından
    // döngü (feedback loop) riski yoktur.
    SetWindowDisplayAffinity(m_overlayHwnd, WDA_NONE);

    // Show + position without activating.
    SetWindowPos(m_overlayHwnd, HWND_TOPMOST,
        x, y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    return true;
}

// ==========================================================================
// StartOverlay
// ==========================================================================

bool App::StartOverlay(HWND menuHwnd, HWND targetHwnd, bool vsync, bool dlss, bool fps,
                       bool fullscreenStretch)
{
    if (m_state == AppState::Capturing) return false;

    m_menuHwnd  = menuHwnd;
    m_targetHwnd = targetHwnd;
    m_fullscreenStretch = fullscreenStretch;

    // ---- 1. D3D11 device (reused across sessions) ----
    if (!InitD3D()) return false;

    // Hedef pencerenin baslik cubugunu kaldir. 
    // "Tam Ekran Yap" aciksa VEYA pencereli moddaysa (kullanici talebi).
    StripTargetBorders();

    // ---- 2. Create the overlay window ----
    if (!CreateOverlayWindow(targetHwnd)) return false;

    // ---- 3. Start WGC capture ----
    m_captureManager = std::make_unique<CaptureManager>();
    if (!m_captureManager->Start(targetHwnd, m_device.Get()))
    {
        DestroyWindow(m_overlayHwnd); m_overlayHwnd = nullptr;
        return false;
    }

    // Initial capture dimensions (may be updated on first frame).
    int capW = m_captureManager->GetWidth();
    int capH = m_captureManager->GetHeight();
    if (capW <= 0 || capH <= 0)
    {
        RECT r = m_lastTargetRect;
        capW = r.right  - r.left;
        capH = r.bottom - r.top;
    }

    // Cikis cozunurlugu YALNIZCA Tam Ekran modunda ayrica belirtilir (monitor boyutu).
    // Klasik modda 0/0 geciyoruz: Renderer cikisi pipeline'a esitler ve nokta ornekleme
    // ile birebir davranis aynen korunur. (Pencere dikdortgeni ile WGC yakalama boyutu
    // DPI/kenarlik yuzunden bir-iki piksel sapabilir; bunu gerdirme sanip lineer filtreye
    // dusmek klasik modda gereksiz bulaniklik yaratirdi.)
    const int outW = m_fullscreenStretch ? (m_lastTargetRect.right  - m_lastTargetRect.left) : 0;
    const int outH = m_fullscreenStretch ? (m_lastTargetRect.bottom - m_lastTargetRect.top)  : 0;

    if (m_fullscreenStretch)
    {
        DLSS_Log("[App] Tam Ekran Yap ACIK: yakalama %dx%d -> cikis %dx%d (olcek %.2fx/%.2fx)",
            capW, capH, outW, outH,
            capW > 0 ? (double)outW / capW : 0.0,
            capH > 0 ? (double)outH / capH : 0.0);
    }

    // ---- 4. Init renderer (swap chain for the overlay window) ----
    m_renderer = std::make_unique<Renderer>();
    if (!m_renderer->Init(m_device.Get(), m_overlayHwnd, capW, capH, outW, outH))
    {
        m_captureManager->Stop();
        DestroyWindow(m_overlayHwnd); m_overlayHwnd = nullptr;
        return false;
    }

    m_renderer->SetVSyncEnabled(vsync);
    m_renderer->SetFpsEnabled(fps);

    const auto& initialCfg = ConfigManager::Get().Config();
    m_renderer->SetBoostFactor(initialCfg.boostFactor);
    m_renderer->SetSplitScreen(initialCfg.splitScreen, initialCfg.splitPos);

    if (m_renderer->GetDLSSManager())
    {
        m_renderer->GetDLSSManager()->SetEnabled(dlss);
    }
    if (m_renderer->GetDLSSNRManager())
    {
        m_renderer->GetDLSSNRManager()->SetEnabled(dlss);
    }

    SettingsWindow::SetOnConfigChanged([this](const Dlss5Config& cfg) {
        if (m_renderer)
        {
            m_renderer->SetBoostFactor(cfg.boostFactor);
            m_renderer->SetSplitScreen(cfg.splitScreen, cfg.splitPos);
            ApplyOverlayComposition(cfg.overlayMode);
            if (m_renderer->GetDLSSNRManager())
            {
                m_renderer->GetDLSSNRManager()->ApplyConfig(cfg);
            }
        }
    });

    m_fpsEnabled  = fps;
    m_dlssEnabled = dlss;
    m_prevVlssDown = false;

    // Give keyboard focus back to the target app so that Insert (ReShade),
    // game input, etc. work exactly as if VLSS5 were not here.
    // We use AttachThreadInput for reliability (avoids the "foreground lock").
    {
        DWORD myTid     = GetCurrentThreadId();
        DWORD targetTid = GetWindowThreadProcessId(m_targetHwnd, nullptr);
        if (myTid != targetTid)
        {
            AttachThreadInput(myTid, targetTid, TRUE);
            SetForegroundWindow(m_targetHwnd);
            AttachThreadInput(myTid, targetTid, FALSE);
        }
        else
        {
            SetForegroundWindow(m_targetHwnd);
        }
    }

    m_prevStopKeyDown = false;
    m_overlayFocused  = false;
    m_prevFocusDown      = false;

    // Initialize FPS tracking for our overlay window
    QueryPerformanceFrequency(&m_fpsFreq);
    QueryPerformanceCounter(&m_fpsLastTime);
    m_fpsFrames = 0;
    m_fpsCurrent    = 0;
    m_renderer->UpdateOSD(m_context.Get(), 0, 0, false, nullptr, 0, true, m_calibMessage);

    // Log system state (process priority, power throttling, battery) at session start
    LogSystemInfo();

    // Start watchdog thread (500 ms polling, 2 s hang threshold)
    StartWatchdog();

    m_state            = AppState::Capturing;
    m_running          = true;
    m_overlayHidden    = false;
    m_sessionStartTime = GetTickCount64();
    return true;
}

// ==========================================================================
// Run  — 1:1 synchronized render loop with zero frame-drop & zero CPU burn
// ==========================================================================

void App::Run()
{
    timeBeginPeriod(1);

    // MMCSS & Thread Priority: Register render thread with MMCSS ("Games" or "Capture")
    // and elevate thread priority so scheduling jitter is eliminated during high GPU load.
    DWORD taskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    if (!hMmcss)
    {
        taskIndex = 0;
        hMmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    while (m_running)
    {
        // 1. Process all pending Win32 messages (non-blocking)
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_running = false;
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (msg.message == WM_HOTKEY)
            {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!m_running) break;

        // 2. Safety: stop if target window was closed
        if (!IsWindow(m_targetHwnd))
        {
            m_running = false;
            break;
        }

        // 3. Update input, hotkeys, window position
        Update();
        if (!m_running) break;

        // 4. If overlay is hidden (Alt+Tab / minimized / target not focused),
        // drain any pending captured frame and throttle loop to save GPU/CPU.
        if (m_overlayHidden)
        {
            if (m_captureManager && m_captureManager->IsNewFrameAvailable())
            {
                ID3D11ShaderResourceView* srv = m_captureManager->AcquireCurrentFrameSRV(m_device.Get());
                if (srv)
                {
                    m_captureManager->ReleaseCurrentFrame();
                }
            }
            Sleep(25);
            continue;
        }

        // 5. Zero-Copy: If a new frame is ready from the game, acquire direct SRV and render immediately!
        if (m_captureManager && m_captureManager->IsNewFrameAvailable())
        {
            ID3D11ShaderResourceView* srv = m_captureManager->AcquireCurrentFrameSRV(m_device.Get());
            if (srv)
            {
                // Kare uretimine baslamadan once sunum kuyrugunda yer acilmasini bekle.
                // Bu bekleme olmadan backpressure Present() icinde 31-143 ms'lik sert
                // blokaj olarak patliyordu (log: render=0.9 ms, present=143 ms).
                m_renderer->WaitForPresentReady();

                Render(srv);
                m_captureManager->ReleaseCurrentFrame();
            }
        }
        else
        {
            // Ultra-low-latency event wait: wake up immediately when WGC posts a new frame
            // or when a Win32 message arrives. Failsafe timeout = 2ms.
            HANDLE hEvent = m_captureManager ? m_captureManager->GetFrameEvent() : nullptr;
            if (hEvent)
            {
                MsgWaitForMultipleObjectsEx(1, &hEvent, 2, QS_ALLINPUT, MWMO_ALERTABLE);
            }
            else
            {
                YieldProcessor();
            }
        }
    }

    if (hMmcss)
    {
        AvRevertMmThreadCharacteristics(hMmcss);
        hMmcss = nullptr;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    timeEndPeriod(1);
    StopOverlay();
}

// ==========================================================================
// UpdateOverlayPosition — throttled DWM checks to avoid IPC lag
// ==========================================================================

void App::UpdateOverlayPosition()
{
    if (!m_targetHwnd || !m_overlayHwnd || m_overlayHidden) return;

    // Tam Ekran Yap: overlay hedefin degil, monitorun dikdortgenini kaplar.
    // Hedef pencere icinde hareket ettikce yeniden konumlandirmaya gerek yok;
    // yalnizca BASKA bir monitore tasinirsa guncelliyoruz.
    if (m_fullscreenStretch)
    {
        HMONITOR mon = MonitorFromWindow(m_targetHwnd, MONITOR_DEFAULTTONEAREST);
        if (mon == m_lastMonitor) return;

        m_lastMonitor = mon;
        RECT fr = ComputeOverlayRect();
        if (fr.right <= fr.left || fr.bottom <= fr.top) return;

        m_lastTargetRect = fr;
        UINT flags = m_overlayFocused ? 0 : SWP_NOACTIVATE;
        SetWindowPos(m_overlayHwnd, HWND_TOPMOST,
            fr.left, fr.top, fr.right - fr.left, fr.bottom - fr.top, flags);

        // Monitor degisti -> cikis cozunurlugu degismis olabilir; swap chain'i yenile.
        if (m_renderer && m_captureManager)
        {
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            m_renderer->Resize(m_device.Get(),
                m_captureManager->GetWidth(), m_captureManager->GetHeight(),
                fr.right - fr.left, fr.bottom - fr.top);
        }
        return;
    }

    // Fast check: GetWindowRect is instantaneous (< 0.0001 ms, local Win32 call, no IPC)
    RECT winRect = {};
    GetWindowRect(m_targetHwnd, &winRect);

    static DWORD lastDwmCheck = 0;
    DWORD now = GetTickCount();

    static RECT lastRawRect = {};
    bool rectChanged = (winRect.left   != lastRawRect.left  ||
                        winRect.top    != lastRawRect.top   ||
                        winRect.right  != lastRawRect.right ||
                        winRect.bottom != lastRawRect.bottom);

    // If window position has not changed and checked recently, skip expensive DWM query!
    if (!rectChanged && (now - lastDwmCheck < 100))
    {
        return;
    }
    lastDwmCheck = now;
    lastRawRect  = winRect;

    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(
            m_targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
    {
        r = winRect;
    }

    // Only call SetWindowPos when position/size actually changed.
    if (r.left   != m_lastTargetRect.left  ||
        r.top    != m_lastTargetRect.top   ||
        r.right  != m_lastTargetRect.right ||
        r.bottom != m_lastTargetRect.bottom)
    {
        m_lastTargetRect = r;
        int w = r.right  - r.left;
        int h = r.bottom - r.top;
        UINT flags = m_overlayFocused ? 0 : SWP_NOACTIVATE;
        SetWindowPos(m_overlayHwnd, HWND_TOPMOST,
            r.left, r.top, w, h, flags);
    }
}

// ==========================================================================
// RecreateCaptureSizedResources
// ==========================================================================

void App::RecreateCaptureSizedResources()
{
    int w = m_captureManager->GetWidth();
    int h = m_captureManager->GetHeight();

    // Unbind RTV before resize
    m_context->OMSetRenderTargets(0, nullptr, nullptr);

    // Yakalama boyutu degisti (oyun penceresi yeniden boyutlandi). Tam Ekran modunda
    // cikis monitor cozunurlugunda SABIT kalir; klasik modda 0/0 ile pipeline'a esitlenir.
    const int outW = m_fullscreenStretch ? (m_lastTargetRect.right  - m_lastTargetRect.left) : 0;
    const int outH = m_fullscreenStretch ? (m_lastTargetRect.bottom - m_lastTargetRect.top)  : 0;

    m_renderer->Resize(m_device.Get(), w, h, outW, outH);
}

// ==========================================================================
// CheckStopKey — called every frame, zero hooks, zero interference
// ==========================================================================

void App::CheckStopKey()
{
    const bool down = InputForwarder::IsStopKeyDown();

    // Edge detection: trigger on the LEADING edge (press, not hold).
    if (down && !m_prevStopKeyDown)
        m_running = false;

    m_prevStopKeyDown = down;
}

// ==========================================================================
// CheckF8FocusToggle — F8 toggles focus between Overlay and Target App
// ==========================================================================

void App::CheckF8FocusToggle()
{
    if (!m_overlayHwnd) return;

    auto& cfg = ConfigManager::Get().Config();
    const bool focusDown = (GetAsyncKeyState(cfg.vkFocus) & 0x8000) != 0;

    // Trigger ONLY on leading edge (new press)
    if (focusDown && !m_prevFocusDown)
    {
        if (!m_overlayFocused)
        {
            // === FOCUS TO OVERLAY (uygulamayı boşver, overlaya dön) ===
            m_overlayFocused = true;

            // 1. Remove WS_EX_TRANSPARENT & WS_EX_NOACTIVATE
            LONG_PTR exStyle = GetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE);
            exStyle &= ~WS_EX_TRANSPARENT;
            exStyle &= ~WS_EX_NOACTIVATE;
            SetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE, exStyle);

            SetWindowPos(m_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

            // 2. Bring overlay to foreground so all keys/clicks go to overlay
            DWORD myTid = GetCurrentThreadId();
            DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            if (myTid != fgTid)
            {
                AttachThreadInput(myTid, fgTid, TRUE);
                SetForegroundWindow(m_overlayHwnd);
                SetActiveWindow(m_overlayHwnd);
                SetFocus(m_overlayHwnd);
                AttachThreadInput(myTid, fgTid, FALSE);
            }
            else
            {
                SetForegroundWindow(m_overlayHwnd);
                SetActiveWindow(m_overlayHwnd);
                SetFocus(m_overlayHwnd);
            }
        }
        else
        {
            // === FOCUS BACK TO TARGET APP (uygulamaya tekrar odaklan) ===
            m_overlayFocused = false;

            // 1. Re-apply WS_EX_TRANSPARENT & WS_EX_NOACTIVATE
            LONG_PTR exStyle = GetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE);
            exStyle |= (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
            SetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE, exStyle);

            SetWindowPos(m_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);

            // 2. Give focus back to the target game window
            if (m_targetHwnd && IsWindow(m_targetHwnd))
            {
                DWORD myTid     = GetCurrentThreadId();
                DWORD targetTid = GetWindowThreadProcessId(m_targetHwnd, nullptr);
                if (myTid != targetTid)
                {
                    AttachThreadInput(myTid, targetTid, TRUE);
                    SetForegroundWindow(m_targetHwnd);
                    SetActiveWindow(m_targetHwnd);
                    SetFocus(m_targetHwnd);
                    AttachThreadInput(myTid, targetTid, FALSE);
                }
                else
                {
                    SetForegroundWindow(m_targetHwnd);
                    SetActiveWindow(m_targetHwnd);
                    SetFocus(m_targetHwnd);
                }
            }
        }
    }

    m_prevFocusDown = focusDown;
}

// ==========================================================================
// CheckFocusAndMinimize — Auto-Hide overlay when target window loses focus
// ==========================================================================

void App::CheckFocusAndMinimize()
{
    if (!m_targetHwnd || !m_overlayHwnd) return;

    // 1. Is target window minimized or closed/invisible?
    bool isMinimized = (IsIconic(m_targetHwnd) != 0) || (!IsWindowVisible(m_targetHwnd));

    // 2. Determine foreground window and process
    HWND fg = GetForegroundWindow();

    DWORD targetPid = 0;
    GetWindowThreadProcessId(m_targetHwnd, &targetPid);
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);

    // Target is considered focused if:
    // - Foreground is the target window itself
    // - Foreground belongs to the same process as target (e.g. child/modal dialog)
    // - Foreground is our overlay window (e.g. F8 focus mode)
    // - Foreground is our settings window (INSERT hotkey)
    // - Grace period of 500ms at session launch while Windows switches focus
    bool isTargetFocused = (fg == m_targetHwnd ||
                            fg == m_overlayHwnd ||
                            (targetPid != 0 && fgPid == targetPid) ||
                            (SettingsWindow::IsOpen() && fg == SettingsWindow::GetHwnd()) ||
                            (GetTickCount64() - m_sessionStartTime < 500));

    bool shouldShow = isTargetFocused && !isMinimized;

    if (!shouldShow)
    {
        if (!m_overlayHidden)
        {
            ShowWindow(m_overlayHwnd, SW_HIDE);
            m_overlayHidden = true;
            DLSS_Log("[App] Target lost focus / minimized (fg=%p, iconic=%d) -> Overlay hidden", fg, isMinimized ? 1 : 0);
        }
    }
    else
    {
        if (m_overlayHidden)
        {
            // Restore overlay visibility without stealing keyboard focus
            ShowWindow(m_overlayHwnd, m_overlayFocused ? SW_SHOW : SW_SHOWNOACTIVATE);
            SetWindowPos(m_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            m_overlayHidden = false;
            DLSS_Log("[App] Target regained focus -> Overlay restored");

            // Reset temporal history so DLSS doesn't try to interpolate across the focus-loss gap
            if (m_renderer && m_renderer->GetDLSSNRManager())
            {
                m_renderer->GetDLSSNRManager()->ResetHistory();
            }

            // Force immediate position update on restore
            UpdateOverlayPosition();
        }
    }
}

// ==========================================================================
// FPS Kalibrasyonu
// ==========================================================================
//
// AMAC: Oyunun kare uretim hizini (RTSS FPS limiti ile) VLSS5'in isleyebildigi
// hiza kilitlemek. Giris cikistan hizli olursa WGC kareleri kuyrukta birikip
// atilir; bu da girdi gecikmesi ve stutter demektir.
//
// YONTEM: Limiti kademeli yukselt, VLSS5'in ayak uyduramadigi noktayi bul, bir
// miktar geri in. Her adim kCalibStepMs bekler.
//
// EMNIYETLER (hepsi gercek hata raporundan dogdu):
//   1. kCalibMinFps TABANI  -- FineDown eskiden sinirsiz asagi sayiyordu. RTSS'te
//      0 = SINIRSIZ oldugu icin sayac 0'i gectigi anda oyun tam hiza cikiyor,
//      desenkron kalicilasiyor ve hedef -1, -2, ... diye eksiye kaciyordu.
//      Kullanicinin gordugu "-30 FPS" tam olarak buydu.
//   2. RTSS UYGULANMIYOR TESPITI -- limit yazildigi halde giris FPS'i hedefin
//      cok uzerinde kaliyorsa RTSS profili gercekten uygulamiyordur. Eskiden bu
//      durum sonsuz desenkron olarak okunup (1)'deki kacisi tetikliyordu.
//   3. BAYAT OLCUM KORUMASI -- giris FPS'i yalnizca yeni WGC karesi gelince,
//      cikis FPS'i yalnizca render calisinca guncellenir. Ikisi de donmus olabilir;
//      bayat degerle karar vermek yanlis desenkron uretir. Bayatsa tik atlanir.
//   4. OTURMA (settle) SURESI -- limit degistikten hemen sonraki olcum hala eski
//      rejime aittir; bir tik bekleyip olcum penceresini sifirliyoruz.
//   5. ADIM TAVANI -- her ne olursa olsun kCalibMaxSteps adimdan sonra durur.
//   6. GERI YUKLEME -- iptal/hata/overlay kapanisinda kullanicinin kalibrasyon
//      oncesi limiti geri yazilir; oyun yarim kalmis bir limitte takili kalmaz.

bool App::CalibInSync(int inFps, int outFps)
{
    // Sabit "<= 1" toleransi fazla dardi: 0.5 sn'lik pencerede tamsayiya yuvarlanan
    // FPS dogal olarak +-2 oynar ve sahte desenkron uretirdi. Yuzdesel tolerans
    // yuksek FPS'te de dogru calisir.
    const int tol = (std::max)(2, static_cast<int>(inFps * 0.05 + 0.5));
    return std::abs(inFps - outFps) <= tol;
}

int App::GetMonitorRefreshHz(HWND hwnd)
{
    HMONITOR mon = MonitorFromWindow(hwnd ? hwnd : GetDesktopWindow(),
                                     MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
    {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency > 1)
        {
            return static_cast<int>(dm.dmDisplayFrequency);
        }
    }
    return 60; // makul varsayilan
}

int App::GetOutputFpsFresh() const
{
    // m_fpsCurrent yalnizca Render() icinde 500 ms'de bir yenilenir. Overlay gizliyken
    // ya da render dururken deger donar kalir; bayatsa 0 don.
    if (m_fpsLastUpdateTick == 0) return 0;
    if (GetTickCount64() - m_fpsLastUpdateTick > 1000) return 0;
    return m_fpsCurrent;
}

std::wstring App::ResolveTargetExeName() const
{
    if (!m_targetHwnd) return L"";

    DWORD pid = 0;
    GetWindowThreadProcessId(m_targetHwnd, &pid);
    if (pid == 0) return L"";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return L"";

    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(hProcess, 0, exePath, &size);
    CloseHandle(hProcess);

    return ok ? std::wstring(PathFindFileNameW(exePath)) : std::wstring();
}

bool App::CalibApplyLimit(int fps)
{
    if (!RTSSManager::Get().SetFramerateLimit(m_calibExeName, fps))
    {
        CalibAbort(L"HATA: RTSS Profili yazilamadi! Klasoru kontrol edin.");
        return false;
    }

    m_calibTargetFps = fps;

    // Emniyet 4: yeni limit henuz gecerli degil; olcum penceresini sifirla ve
    // bir sonraki tiki atla ki eski rejimin kareleri karara karismasin.
    if (m_captureManager) m_captureManager->ResetInputFpsWindow();
    m_calibSettleTicks = 1;
    m_calibNotApplied  = 0;

    DLSS_Log("[Calib] Limit uygulandi: %d FPS (%ls)", fps, m_calibExeName.c_str());
    return true;
}

void App::CalibRestoreOriginalLimit()
{
    // Emniyet 6
    if (m_calibExeName.empty()) return;
    RTSSManager::Get().SetFramerateLimit(m_calibExeName, m_calibOriginalLimit);
    DLSS_Log("[Calib] Orijinal limit geri yuklendi: %d FPS (%ls)",
             m_calibOriginalLimit, m_calibExeName.c_str());
}

void App::CalibAbort(const std::wstring& msg)
{
    CalibRestoreOriginalLimit();
    m_calibState        = CalibState::Done;
    m_calibMessage      = msg;
    m_calibMessageTimer = GetTickCount64();
    DLSS_Log("[Calib] DURDURULDU: %ls", msg.c_str());
}

void App::CalibFinish(int fps)
{
    if (fps < 0) fps = 0;

    RTSSManager::Get().SetFramerateLimit(m_calibExeName, fps);
    m_calibTargetFps    = fps;
    m_calibState        = CalibState::Done;
    m_calibMessage      = (fps == 0)
        ? std::wstring(L"KALIBRASYON TAMAMLANDI: LIMIT GEREKMIYOR")
        : L"KALIBRASYON TAMAMLANDI: " + std::to_wstring(fps) + L" FPS";
    m_calibMessageTimer = GetTickCount64();
    DLSS_Log("[Calib] TAMAMLANDI: %d FPS", fps);
}

void App::UpdateCalibration(const Dlss5Config& cfg)
{
    const bool calibDown    = (GetAsyncKeyState(cfg.vkCalib) & 0x8000) != 0;
    const bool calibPressed = calibDown && !m_prevCalibDown;
    m_prevCalibDown = calibDown;

    const bool running = (m_calibState != CalibState::Idle && m_calibState != CalibState::Done);

    // ---- Baslat / iptal ----
    if (calibPressed)
    {
        if (running)
        {
            // Ayni tusa tekrar basmak iptal eder. Eskiden hicbir sey yapmiyordu;
            // kaciga giren bir kalibrasyonu durdurmanin yolu yoktu.
            CalibAbort(L"KALIBRASYON IPTAL EDILDI");
            return;
        }

        if (m_calibState == CalibState::Idle)
        {
            m_calibExeName = ResolveTargetExeName();
            if (m_calibExeName.empty())
            {
                m_calibMessage      = L"HATA: Hedef uygulama adi okunamadi.";
                m_calibMessageTimer = GetTickCount64();
                m_calibState        = CalibState::Done;
                return;
            }

            // Emniyet 6: kullanicinin mevcut ayarini sakla.
            m_calibOriginalLimit = RTSSManager::Get().GetFramerateLimit(m_calibExeName);
            m_calibSteps         = 0;
            m_calibHiBound       = 0;
            m_calibLoFps         = 0;
            m_calibNotApplied    = 0;
            m_calibTimer         = GetTickCount64();
            m_calibState         = CalibState::InitUncap;
            m_calibMessage       = L"FPS LIMITI SIFIRLANIYOR...";

            DLSS_Log("[Calib] Baslatildi: %ls (onceki limit=%d)",
                     m_calibExeName.c_str(), m_calibOriginalLimit);

            // Sinirsiza al ve oyunun gercek tavanini olc.
            CalibApplyLimit(0);
        }
        return;
    }

    // ---- Mesaj zaman asimi ----
    if (m_calibState == CalibState::Done)
    {
        if (GetTickCount64() - m_calibMessageTimer > 4000)
        {
            m_calibState   = CalibState::Idle;
            m_calibMessage = L"";
        }
        return;
    }

    if (!running) return;

    // ---- Adim zamanlayici ----
    const ULONGLONG now = GetTickCount64();
    if (now - m_calibTimer < kCalibStepMs) return;
    m_calibTimer = now;

    // Emniyet 5: mutlak adim tavani.
    if (++m_calibSteps > kCalibMaxSteps)
    {
        CalibAbort(L"KALIBRASYON ZAMAN ASIMI - ayarlar geri alindi");
        return;
    }

    // Emniyet 4: limit degisiminden sonraki ilk tik olcum icin guvenilir degil.
    if (m_calibSettleTicks > 0)
    {
        --m_calibSettleTicks;
        return;
    }

    // Emniyet 3: bayat olcumle karar verme.
    const int inFps  = m_captureManager ? m_captureManager->GetInputFpsFresh() : 0;
    const int outFps = GetOutputFpsFresh();
    if (inFps <= 0 || outFps <= 0)
    {
        --m_calibSteps; // bu tik sayilmaz
        DLSS_Log("[Calib] Olcum bayat (in=%d out=%d), tik atlandi", inFps, outFps);
        return;
    }

    switch (m_calibState)
    {
    case CalibState::InitUncap:
    {
        // ---------------------------------------------------------------
        // ARAMANIN UST SINIRI
        // ---------------------------------------------------------------
        // DIKKAT: sinirsiz fazda olculen giris FPS'i oyunun gercek tavani DEGILDIR.
        // Sinirsizken oyun GPU'yu doldurur, WGC teslimati bozulur ve olcum gercegin
        // cok altinda cikar. Gercek bir oturumda sinirsizken giris=16 olcuIdu, ama
        // ayni oturumda limit=87 iken giris=40 geldi. Yani sinirsiz olcum, tavani
        // OLDUGUNDAN KUCUK gosteriyor; ust sinir olarak kullanilamaz.
        //
        // Dogru ust sinir MONITOR YENILEME HIZIDIR; DWM ile kompoze edilen bir
        // overlay bunun uzerine zaten cikamaz.
        const int refreshHz = GetMonitorRefreshHz(m_overlayHwnd);
        m_calibHiBound = (std::max)(inFps, refreshHz);
        if (m_calibHiBound < kCalibMinFps) m_calibHiBound = kCalibMinFps;

        DLSS_Log("[Calib] Sinirsiz olcum: giris=%d cikis=%d | monitor=%d Hz -> ust sinir=%d",
                 inFps, outFps, refreshHz, m_calibHiBound);

        if (CalibInSync(inFps, outFps))
        {
            // VLSS5 zaten oyunun tam hizina yetisiyor -> limit gereksiz.
            CalibFinish(0);
            return;
        }

        // Kaba taramaya TABANDAN basla ve 10'ar 10'ar cik.
        //
        // Neden ikili arama degil: bisection ilk sondayi araligin ortasina atiyordu
        // (165 Hz monitorde 87 FPS). Bu hem kullaniciya anlamsiz goruniyor, hem de
        // sistemin ulasamayacagi bir hiz test edildigi icin bilgi tasimiyordu --
        // hedef 87 iken giris 40'ta kaliyor, sonda "desenkron" disinda bir sey
        // soylemiyor. Dogru soru "hangi hizda bozuluyor", cevabi da tabandan
        // yukari yuruyerek bulunur. Cevap tipik olarak dusuk oldugu icin (30-60)
        // bu ayni zamanda DAHA HIZLI yakinsiyor.
        m_calibLoFps = 0;   // henuz hicbir hiz dogrulanmadi
        m_calibState = CalibState::CoarseUp;
        m_calibMessage = L"FPS KALIBRE EDILIYOR (HEDEF: " + std::to_wstring(kCalibMinFps) + L")...";
        CalibApplyLimit(kCalibMinFps);
        return;
    }

    case CalibState::CoarseUp:
    {
        // Emniyet 2: limit yazildi ama oyun hala cok daha hizli kosuyorsa RTSS
        // profili gercekten uygulamiyor demektir.
        if (inFps > m_calibTargetFps * 3 / 2 + 10)
        {
            if (++m_calibNotApplied >= 2)
            {
                DLSS_Log("[Calib] RTSS limiti uygulanmiyor (hedef=%d, olculen giris=%d)",
                         m_calibTargetFps, inFps);
                CalibAbort(L"RTSS LIMITI UYGULANMIYOR - RTSS calisiyor mu?");
                return;
            }
            return;
        }
        m_calibNotApplied = 0;

        if (CalibInSync(inFps, outFps))
        {
            m_calibLoFps = m_calibTargetFps;   // bu hiz DOGRULANDI

            if (inFps < m_calibTargetFps - 5)
            {
                // Oyun kendi ic limitine/tavanina takildi: hedefi yukseltmenin
                // anlami yok, olculen gercek hizda bitir.
                DLSS_Log("[Calib] Oyun kendi tavaninda (hedef=%d, giris=%d)",
                         m_calibTargetFps, inFps);
                CalibFinish(inFps);
                return;
            }
            if (m_calibTargetFps >= m_calibHiBound)
            {
                // Monitor tavanina ulastik ve hala senkronuz.
                CalibFinish(m_calibHiBound);
                return;
            }

            int next = m_calibTargetFps + kCalibCoarseStep;
            if (next > m_calibHiBound) next = m_calibHiBound;

            DLSS_Log("[Calib] %d FPS tasiniyor (giris=%d cikis=%d) -> %d deneniyor",
                     m_calibTargetFps, inFps, outFps, next);

            m_calibMessage = L"FPS KALIBRE EDILIYOR (HEDEF: " + std::to_wstring(next) + L")...";
            CalibApplyLimit(next);
            return;
        }

        // Bozuldu. Cevap (son dogrulanan, mevcut hedef) araligindadir; 1'er inerek ara.
        DLSS_Log("[Calib] %d FPS tasinmiyor (giris=%d cikis=%d) -> ince arama, taban=%d",
                 m_calibTargetFps, inFps, outFps, m_calibLoFps);

        {
            const int floorFps = (m_calibLoFps > 0) ? m_calibLoFps : kCalibMinFps;
            const int next     = m_calibTargetFps - 1;

            if (next <= floorFps)
            {
                // Inecek yer yok: dogrulanmis en yuksek hizda bitir.
                CalibFinish(floorFps);
                return;
            }

            m_calibState   = CalibState::FineDown;
            m_calibMessage = L"SENKRON ARANIYOR (HEDEF: " + std::to_wstring(next) + L")...";
            CalibApplyLimit(next);
        }
        return;
    }

    case CalibState::FineDown:
    {
        if (CalibInSync(inFps, outFps))
        {
            CalibFinish(m_calibTargetFps);
            return;
        }

        // Emniyet 1: ince arama, DOGRULANMIS en yuksek hizin altina inmez.
        // m_calibLoFps hicbir zaman dogrulanmadiysa taban kCalibMinFps'tir.
        // Boylece hedef asla 0'in (RTSS'te SINIRSIZ) altina dusemez ve eskiden
        // gorulen "-30 FPS" kacisi yapisal olarak imkansizdir.
        const int floorFps = (m_calibLoFps > 0) ? m_calibLoFps : kCalibMinFps;
        const int next     = m_calibTargetFps - 1;

        if (next <= floorFps)
        {
            CalibFinish(floorFps);
            return;
        }

        m_calibMessage = L"SENKRON ARANIYOR (HEDEF: " + std::to_wstring(next) + L")...";
        CalibApplyLimit(next);
        return;
    }

    default:
        return;
    }
}

// ==========================================================================
// Update
// ==========================================================================

void App::Update()
{
    // Check stop keybind first (Alt+S)
    CheckStopKey();
    if (!m_running) return;

    // Check target window focus & minimize state (Auto-Hide on Alt+Tab / Minimize)
    CheckFocusAndMinimize();
    if (!m_running) return;

    // If overlay is hidden because target lost focus, skip remaining interactive hotkeys & positioning
    if (m_overlayHidden) return;

    // Check F8 focus toggle key (Overlay vs Target App)
    CheckF8FocusToggle();
    if (!m_running) return;

    // Check F9 to toggle FPS counter display (on/off)
    auto& cfg = ConfigManager::Get().Config();
    const bool fpsDown = (GetAsyncKeyState(cfg.vkFps) & 0x8000) != 0;
    if (fpsDown && !m_prevFpsDown)
    {
        m_fpsEnabled = !m_fpsEnabled;
        m_renderer->SetFpsEnabled(m_fpsEnabled);
        m_renderer->UpdateFpsConstantBuffer(m_context.Get());
    }
    m_prevFpsDown = fpsDown;

    // FPS kalibrasyonu (durum makinesi + kacis emniyetleri ayri fonksiyonda)
    UpdateCalibration(cfg);

    // Check F10 to toggle DLSS 5 on/off
    const bool vlssDown = (GetAsyncKeyState(cfg.vkToggleVlss) & 0x8000) != 0;
    if (vlssDown && !m_prevVlssDown)
    {
        if (m_renderer)
        {
            m_dlssEnabled = !m_dlssEnabled;
            // Eger devre disi kaldiysa mv ve ofa state clear!
            if (!m_dlssEnabled)
            {
                // Removed invalid NvOFManager clear block
            }
            if (m_renderer->GetDLSSNRManager())
            {
                m_renderer->GetDLSSNRManager()->SetEnabled(m_dlssEnabled);
            }
            if (m_renderer->GetDLSSManager())
            {
                m_renderer->GetDLSSManager()->SetEnabled(m_dlssEnabled);
            }
            int inputFps = m_captureManager ? m_captureManager->GetCurrentInputFps() : 0;
            bool showWarning = !m_warningDismissed && (inputFps > m_fpsCurrent + 20);
            const double* history = m_captureManager ? m_captureManager->GetGapHistory() : nullptr;
            int historyIdx = m_captureManager ? m_captureManager->GetGapHistoryIdx() : 0;
            m_renderer->UpdateOSD(m_context.Get(), m_fpsCurrent, inputFps, showWarning, history, historyIdx, true, m_calibMessage);
        }
    }
    m_prevVlssDown = vlssDown;

    // Check FG Indicator (F7)
    const bool fgIndicatorDown = (GetAsyncKeyState(cfg.vkFgIndicator) & 0x8000) != 0;
    if (fgIndicatorDown && !m_prevFgIndicatorDown)
    {
        m_fgMarkerActive = !m_fgMarkerActive;
    }
    m_prevFgIndicatorDown = fgIndicatorDown;

    // Check in-game DLSS 5 settings hotkey (Default: INSERT or user-configured key)
    const auto& scfg = ConfigManager::Get().Config();
    bool modMatch = true;
    if (scfg.settingsMod & MOD_CONTROL) modMatch = modMatch && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    if (scfg.settingsMod & MOD_ALT)     modMatch = modMatch && ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    if (scfg.settingsMod & MOD_SHIFT)   modMatch = modMatch && ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);

    const bool settingsDown = modMatch && ((GetAsyncKeyState(scfg.settingsVk) & 0x8000) != 0);
    static bool s_prevSettingsDown = false;
    if (settingsDown && !s_prevSettingsDown)
    {
        SettingsWindow::Toggle(m_overlayHwnd);
    }
    s_prevSettingsDown = settingsDown;

    // Ayarlar penceresi acikken bazi oyunlar focus kaybina ragmen ClipCursor'u
    // her frame yeniden uyguluyor; imlecin pencerede serbest kalmasi icin
    // clip rect'i surekli temiz tutuyoruz.
    if (SettingsWindow::IsOpen())
        ClipCursor(nullptr);

    // Keep overlay perfectly aligned with the moving/resizing target.
    UpdateOverlayPosition();

    // Handle WGC reporting a window resize.
    if (m_captureManager->ConsumeResizeEvent())
    {
        RecreateCaptureSizedResources();
        return; // skip this frame — textures just recreated
    }
}

// ==========================================================================
// Render
// ==========================================================================

void App::Render(ID3D11ShaderResourceView* srv)
{
    if (!srv) return;

    LARGE_INTEGER frameStart, t0, t1, t2, perfFreq;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&frameStart);

    // --- Watchdog: mark render stage ---
    m_currentStage.store("render", std::memory_order_relaxed);
    QueryPerformanceCounter(&m_stageEnteredTime);

    // Measure our overlay window's actual render FPS
    m_fpsFrames++;
    double elapsed = double(frameStart.QuadPart - m_fpsLastTime.QuadPart) / double(m_fpsFreq.QuadPart);
    if (elapsed >= 0.5) // update every 500 ms for a clean, stable reading
    {
        m_fpsCurrent = static_cast<int>((m_fpsFrames / elapsed) + 0.5);
        m_fpsFrames = 0;
        m_fpsLastTime = frameStart;
        // Tazelik damgasi: kalibrasyon bayat cikis FPS'iyle karar vermesin.
        m_fpsLastUpdateTick = GetTickCount64();
        
        int inputFps = m_captureManager ? m_captureManager->GetCurrentInputFps() : 0;
        
        // Show warning if input > output + 20 and not dismissed
        bool showWarning = false;
        if (!m_warningDismissed && (inputFps > m_fpsCurrent + 20))
        {
            showWarning = true;
        }

        const double* history = m_captureManager ? m_captureManager->GetGapHistory() : nullptr;
        int historyIdx = m_captureManager ? m_captureManager->GetGapHistoryIdx() : 0;
        
        m_renderer->UpdateOSD(m_context.Get(), m_fpsCurrent, inputFps, showWarning, history, historyIdx, true, m_calibMessage);
    }

    // --- RenderFrame stage ---
    m_currentStage.store("RenderFrame", std::memory_order_relaxed);
    QueryPerformanceCounter(&m_stageEnteredTime);
    QueryPerformanceCounter(&t0);
    m_renderer->RenderFrame(m_context.Get(), srv);
    QueryPerformanceCounter(&t1);

    // --- Present stage ---
    m_currentStage.store("Present", std::memory_order_relaxed);
    QueryPerformanceCounter(&m_stageEnteredTime);
    m_renderer->Present();
    QueryPerformanceCounter(&t2);

    m_currentStage.store("idle", std::memory_order_relaxed);

    // Compute timings
    double renderMs  = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(perfFreq.QuadPart);
    double presentMs = double(t2.QuadPart - t1.QuadPart) * 1000.0 / double(perfFreq.QuadPart);
    double totalMs   = double(t2.QuadPart - frameStart.QuadPart) * 1000.0 / double(perfFreq.QuadPart);

    // Accumulate for 5-second summary
    m_sumTotalMs += totalMs;
    if (totalMs > m_maxTotalMs) m_maxTotalMs = totalMs;
    m_sumRenderMs  += renderMs;
    m_sumPresentMs += presentMs;
    if (presentMs > m_maxPresentMs) m_maxPresentMs = presentMs;

    // GPU suresi bir onceki karenin zaman damgalarindan gelir (BeginFrame'de
    // okunur); orneklem basina bir kare gecikmeli olmasi ortalama icin onemsiz.
    double gpuMs = 0.0;
    if (m_renderer && m_renderer->GetD3D12Interop())
        gpuMs = m_renderer->GetD3D12Interop()->GetLastGpuMs();
    m_sumGpuMs += gpuMs;
    if (gpuMs > m_maxGpuMs) m_maxGpuMs = gpuMs;

    m_timingSamples++;

    // Slow-frame threshold: >3× running average
    if (m_timingSamples > 5)
    {
        double avgMs = m_sumTotalMs / static_cast<double>(m_timingSamples);
        if (totalMs > avgMs * 3.0)
            DLSS_Log("[Perf] YAVAS KARE: toplam=%.2f ms (ort=%.2f ms) | render=%.2f ms | present=%.2f ms",
                totalMs, avgMs, renderMs, presentMs);
    }

    // 5-second perf summary
    FlushPerfStats();

    // 5-second WGC capture stats summary
    if (m_captureManager)
        m_captureManager->FlushCaptureStats();
}


// ==========================================================================
// StopOverlay
// ==========================================================================

void App::StopOverlay()
{
    if (m_state != AppState::Capturing) return;

    m_running = false;
    m_state   = AppState::Menu;

    // Yarim kalmis bir kalibrasyon varsa kullanicinin RTSS limitini geri yukle.
    // Aksi halde oyun, arama sirasinda yazilmis gecici bir limitte (ornegin 10 FPS)
    // takili kalirdi.
    if (m_calibState != CalibState::Idle && m_calibState != CalibState::Done)
    {
        CalibRestoreOriginalLimit();
    }
    m_calibState   = CalibState::Idle;
    m_calibMessage = L"";
    m_calibExeName.clear();

    // Stop watchdog before tearing down resources
    StopWatchdog();
    m_currentStage.store("idle", std::memory_order_relaxed);

    SettingsWindow::Hide();

    // Stop WGC before touching D3D resources.
    if (m_captureManager)
    {
        m_captureManager->Stop();
        m_captureManager.reset();
    }

    // Flush D3D, release renderer resources.
    if (m_renderer && m_context)
    {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_context->ClearState();
        m_context->Flush();
        m_renderer->Cleanup();
        m_renderer.reset();
    }

    // Destroy the overlay window.
    if (m_overlayHwnd)
    {
        DestroyWindow(m_overlayHwnd);
        m_overlayHwnd = nullptr;
    }

    // Oturum bitti
    RestoreTargetBorders();
    m_targetHwnd = nullptr;
    m_lastMonitor = nullptr;
    m_overlayFocused = false;
    m_prevFocusDown     = false;
    m_overlayHidden  = false;
}

// ==========================================================================
// OverlayWndProc
// ==========================================================================

LRESULT CALLBACK App::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_appInstance)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_NCHITTEST:
        if (!g_appInstance->m_overlayFocused)
        {
            // Game Mode: pass all mouse moves, aiming, and clicks directly to the game!
            return HTTRANSPARENT;
        }
        return HTCLIENT;

    case WM_MOUSEACTIVATE:
        if (!g_appInstance->m_overlayFocused)
        {
            return MA_NOACTIVATE;
        }
        return MA_ACTIVATE;

    case WM_SETCURSOR:
        if (g_appInstance->m_overlayFocused)
        {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_DESTROY:
        // Window was closed externally (e.g. killed via Task Manager).
        g_appInstance->m_running = false;
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ==========================================================================
// LogSystemInfo — Process priority, power throttling, battery status
// ==========================================================================

void App::LogSystemInfo()
{
    // 1. Process priority class
    DWORD prio = GetPriorityClass(GetCurrentProcess());
    const char* prioStr = "UNKNOWN";
    switch (prio)
    {
    case IDLE_PRIORITY_CLASS:          prioStr = "IDLE"; break;
    case BELOW_NORMAL_PRIORITY_CLASS:  prioStr = "BELOW_NORMAL"; break;
    case NORMAL_PRIORITY_CLASS:        prioStr = "NORMAL"; break;
    case ABOVE_NORMAL_PRIORITY_CLASS:  prioStr = "ABOVE_NORMAL"; break;
    case HIGH_PRIORITY_CLASS:          prioStr = "HIGH"; break;
    case REALTIME_PRIORITY_CLASS:      prioStr = "REALTIME"; break;
    }
    DLSS_Log("[System] Surec onceligi: %s (0x%08X)", prioStr, prio);

    // 2. Power throttling state (Windows 10 1709+)
    PROCESS_POWER_THROTTLING_STATE pts = {};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (GetProcessInformation(GetCurrentProcess(),
            ProcessPowerThrottling, &pts, sizeof(pts)))
    {
        bool throttled = (pts.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
        DLSS_Log("[System] Guc kisitmasi (EcoQoS): %s  (StateMask=0x%08X, ControlMask=0x%08X)",
            throttled ? "AKTIF (YAVAS MOD!)" : "DEVRE DISI", pts.StateMask, pts.ControlMask);
    }
    else
    {
        DLSS_Log("[System] GetProcessInformation(PowerThrottling) basarisiz (eski Windows surumu?)");
    }

    // 3. Battery / power source
    SYSTEM_POWER_STATUS sps = {};
    if (GetSystemPowerStatus(&sps))
    {
        const char* acLine = (sps.ACLineStatus == 1) ? "AC (priz)" :
                             (sps.ACLineStatus == 0) ? "BATARYA"   : "bilinmiyor";
        char batStr[32] = {};
        if (sps.BatteryFlag == 128) // no battery
            strcpy_s(batStr, "yok");
        else
            snprintf(batStr, sizeof(batStr), "%%%u", sps.BatteryLifePercent);

        DLSS_Log("[System] Guc kaynagi: %s | Batarya: %s | BatteryFlag=0x%02X",
            acLine, batStr, sps.BatteryFlag);
    }
}

// ==========================================================================
// StartWatchdog / StopWatchdog / WatchdogThreadProc
//   Low-priority background thread that wakes every 500 ms.
//   If the main render thread has been stuck in the same stage for >2 s,
//   it logs a [Watchdog] warning.
// ==========================================================================

void App::StartWatchdog()
{
    if (m_watchdogRunning.load()) return;

    QueryPerformanceCounter(&m_stageEnteredTime);
    m_currentStage.store("idle", std::memory_order_relaxed);
    m_watchdogRunning.store(true, std::memory_order_relaxed);

    m_watchdogThread = CreateThread(
        nullptr, 0, WatchdogThreadProc, this, 0, nullptr);

    if (m_watchdogThread)
        SetThreadPriority(m_watchdogThread, THREAD_PRIORITY_LOWEST);
}

void App::StopWatchdog()
{
    m_watchdogRunning.store(false, std::memory_order_relaxed);
    if (m_watchdogThread)
    {
        WaitForSingleObject(m_watchdogThread, 2000);
        CloseHandle(m_watchdogThread);
        m_watchdogThread = nullptr;
    }
}

DWORD WINAPI App::WatchdogThreadProc(LPVOID param)
{
    App* app = static_cast<App*>(param);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    while (app->m_watchdogRunning.load(std::memory_order_relaxed))
    {
        Sleep(500);

        const char* stage = app->m_currentStage.load(std::memory_order_relaxed);
        if (!stage || strcmp(stage, "idle") == 0) continue;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double stuckMs = static_cast<double>(now.QuadPart - app->m_stageEnteredTime.QuadPart)
                         * 1000.0 / static_cast<double>(freq.QuadPart);

        if (stuckMs > 2000.0)
            DLSS_Log("[Watchdog] UYARI: render thread '%s' asamasinda %.0f ms dir takildi!",
                stage, stuckMs);
    }
    return 0;
}

// ==========================================================================
// FlushPerfStats — Per-frame pipeline timing 5-second summary
// ==========================================================================

void App::FlushPerfStats()
{
    if (m_timingSamples == 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (m_lastPerfLogTime.QuadPart != 0)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double sinceMs = static_cast<double>(now.QuadPart - m_lastPerfLogTime.QuadPart)
                         * 1000.0 / static_cast<double>(freq.QuadPart);
        if (sinceMs < 5000.0) return;
    }
    m_lastPerfLogTime = now;

    const double n         = static_cast<double>(m_timingSamples);
    const double avgMs     = m_sumTotalMs    / n;
    const double avgRender = m_sumRenderMs   / n;
    const double avgPresent= m_sumPresentMs  / n;
    const double avgGpu    = m_sumGpuMs      / n;

    DLSS_Log("[Perf] ozet: %llu kare | toplam ort/maks: %.2f/%.2f ms | "
             "cpu-submit ort: %.2f ms | GPU ort/maks: %.2f/%.2f ms | present ort/maks: %.2f/%.2f ms",
        m_timingSamples, avgMs, m_maxTotalMs, avgRender,
        avgGpu, m_maxGpuMs, avgPresent, m_maxPresentMs);

    // --- VRAM baskisi ---
    // RTX 4060 = 8 GB. 2560x1440'ta oyun + DLSS-NR modeli + WGC havuzu + paylasilan
    // dokular butceyi asarsa surucu sayfalamaya baslar; bu da once kademeli, sonra
    // ani bir cokuse yol acar. Kullanim butcenin uzerindeyse suclu budur.
    if (m_device)
    {
        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(m_device.As(&dxgiDev)))
        {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)))
            {
                ComPtr<IDXGIAdapter3> adapter3;
                if (SUCCEEDED(adapter.As(&adapter3)))
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
                    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
                    {
                        const double usedMB   = vm.CurrentUsage  / (1024.0 * 1024.0);
                        const double budgetMB = vm.Budget        / (1024.0 * 1024.0);
                        const double resMB    = vm.CurrentReservation / (1024.0 * 1024.0);
                        DLSS_Log("[VRAM] kullanim=%.0f MB / butce=%.0f MB (%.0f%%)%s | rezerve=%.0f MB",
                            usedMB, budgetMB,
                            (budgetMB > 0.0) ? (usedMB * 100.0 / budgetMB) : 0.0,
                            (vm.CurrentUsage > vm.Budget) ? "  <-- BUTCE ASILDI, surucu sayfaliyor" : "",
                            resMB);
                    }
                }
            }
        }
    }

    m_sumTotalMs    = 0.0;
    m_maxTotalMs    = 0.0;
    m_sumRenderMs   = 0.0;
    m_sumPresentMs  = 0.0;
    m_maxPresentMs  = 0.0;
    m_sumGpuMs      = 0.0;
    m_maxGpuMs      = 0.0;
    m_timingSamples = 0;
}

// ==========================================================================
// Window Border Management (Borderless Windowed Mode)
// ==========================================================================
void App::StripTargetBorders()
{
    if (!m_targetHwnd || m_bordersStripped) return;

    m_originalTargetStyle   = GetWindowLongPtrW(m_targetHwnd, GWL_STYLE);
    m_originalTargetExStyle = GetWindowLongPtrW(m_targetHwnd, GWL_EXSTYLE);

    // Eger pencere zaten borderless/fullscreen degilse islem yap
    if ((m_originalTargetStyle & WS_CAPTION) == WS_CAPTION)
    {
        GetWindowRect(m_targetHwnd, &m_originalTargetRect);

        // Mevcut saf oyun alani (client area) boyutunu al
        RECT clientRect = {};
        GetClientRect(m_targetHwnd, &clientRect);
        int clientW = clientRect.right - clientRect.left;
        int clientH = clientRect.bottom - clientRect.top;

        // Pencere kenarliklarini kaldir
        LONG_PTR newStyle = m_originalTargetStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongPtrW(m_targetHwnd, GWL_STYLE, newStyle);

        // Pencerenin dis boyutunu, eski client boyutuna ayarla. 
        // Boylece oyunun ic cozunurlugu kesinlikle degismez, sadece kenarliklar yok olur.
        SetWindowPos(m_targetHwnd, nullptr, 
            m_originalTargetRect.left, m_originalTargetRect.top, 
            clientW, clientH, 
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        m_bordersStripped = true;
        DLSS_Log("[App] Hedef pencere kenarliklari kaldirildi. Orijinal Client: %dx%d", clientW, clientH);
    }
}

void App::RestoreTargetBorders()
{
    if (!m_targetHwnd || !m_bordersStripped) return;

    SetWindowLongPtrW(m_targetHwnd, GWL_STYLE, m_originalTargetStyle);
    SetWindowLongPtrW(m_targetHwnd, GWL_EXSTYLE, m_originalTargetExStyle);
    
    SetWindowPos(m_targetHwnd, nullptr, 
        m_originalTargetRect.left, m_originalTargetRect.top, 
        m_originalTargetRect.right - m_originalTargetRect.left, 
        m_originalTargetRect.bottom - m_originalTargetRect.top, 
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    m_bordersStripped = false;
    DLSS_Log("[App] Hedef pencere kenarliklari geri yuklendi.");
}
