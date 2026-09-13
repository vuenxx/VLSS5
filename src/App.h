#pragma once
#include "Common.h"
#include "CaptureManager.h"
#include "Renderer.h"
#include "InputForwarder.h"

// -----------------------------------------------------------------------
// App — state machine that owns the overlay window and the render loop.
// -----------------------------------------------------------------------
enum class AppState { Menu, Capturing };

class App
{
public:
    explicit App(HINSTANCE hInstance);
    ~App();

    // Start WGC capture + overlay for the given target HWND.
    // 'menuHwnd' is the menu window (used to exclude from window list; hidden before this call).
    // Returns false on failure (shows menu again, caller logs the error).
    bool StartOverlay(HWND menuHwnd, HWND targetHwnd, bool vsync = false, bool dlss = true, bool fps = true);

    // Run the overlay render loop until Alt+S is pressed or the target window closes.
    // Blocks on the calling thread (nested Win32 modal loop).
    void Run();

    AppState GetState() const { return m_state; }
    void RequestStop() { m_running = false; }

    // Update preferred GPU (resets D3D device if in menu so next capture uses new GPU)
    void SetPreferredGpu(const std::wstring& gpuName);

private:
    // D3D11 device shared for the lifetime of the app.
    bool InitD3D();

    // Create the borderless topmost overlay HWND.
    bool CreateOverlayWindow(HWND targetHwnd);

    // Keep the overlay rect in sync with the target window every frame.
    void UpdateOverlayPosition();

    // Recreate textures and swap chain after a WGC size change.
    void RecreateCaptureSizedResources();

    // Per-frame update + render.
    void Update();
    void Render(ID3D11ShaderResourceView* srv);

    // Poll the stop keybind each frame via GetAsyncKeyState (no hooks).
    void CheckStopKey();

    // Toggle between Overlay focus and Target App focus via F8 key.
    void CheckF8FocusToggle();

    // Auto-Hide overlay when target window loses focus or minimizes (Alt+Tab handling).
    void CheckFocusAndMinimize();

    // Tear down the overlay session and restore state.
    void StopOverlay();

    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ---- members ----
    HINSTANCE m_hInstance   = nullptr;
    HWND      m_overlayHwnd = nullptr;
    HWND      m_targetHwnd  = nullptr;
    HWND      m_menuHwnd    = nullptr;

    AppState  m_state            = AppState::Menu;
    bool      m_running          = false;
    bool      m_prevStopKeyDown  = false;
    bool      m_overlayFocused   = false; // F8 toggle: false = focus on target app, true = focus on overlay
    bool      m_prevF8Down       = false;
    bool      m_overlayHidden    = false; // Auto-Hide: true when target is minimized or not focused
    ULONGLONG m_sessionStartTime = 0;

    // FPS tracking (measures overlay window's actual render FPS)
    LARGE_INTEGER m_fpsFreq       = {};
    LARGE_INTEGER m_fpsLastTime   = {};
    int           m_fpsFrameCount = 0;
    int           m_currentFps    = 0;
    bool          m_fpsEnabled    = true;
    bool          m_prevF9Down    = false;
    bool          m_dlssEnabled   = true;
    bool          m_prevF10Down   = false;
    bool          m_fgMarkerActive = false;
    // Oyun modunda OS imlecinin gizli tutulmasi icin thread imlec sayaci durumu.
    bool          m_cursorVisible  = true;
    bool          m_prevF7Down    = false;

    RECT m_lastTargetRect = {};

    ComPtr<ID3D11Device>        m_device;
    ComPtr<ID3D11DeviceContext> m_context;

    std::unique_ptr<CaptureManager> m_captureManager;
    std::unique_ptr<Renderer>       m_renderer;

    // ---- Per-frame pipeline timing ----
    struct FrameTimings
    {
        double captureWaitMs  = 0.0;
        double downscaleMs    = 0.0;
        double mvMs           = 0.0;
        double evalMs         = 0.0;
        double presentMs      = 0.0;
        double totalMs        = 0.0;
    };

    // Running stats for the 5-second summary log (MV, Eval, Total)
    static constexpr int kJitterWindow = 120;
    double   m_mvHistory[kJitterWindow]    = {};
    double   m_evalHistory[kJitterWindow]  = {};
    double   m_totalHistory[kJitterWindow] = {};
    int      m_perfHistoryIdx              = 0;

    double   m_sumMvMs        = 0.0;
    double   m_maxMvMs        = 0.0;
    double   m_sumEvalMs      = 0.0;
    double   m_maxEvalMs      = 0.0;
    double   m_sumTotalMs     = 0.0;
    double   m_maxTotalMs     = 0.0;
    uint64_t m_timingSamples  = 0;
    LARGE_INTEGER m_lastPerfLogTime = {};

    // Rolling median of last 60 frames for dynamic stutter event detection
    static constexpr int kMedianWindow = 60;
    double   m_medianHistory[kMedianWindow] = {};
    int      m_medianHistoryIdx             = 0;

    // Watchdog: tracks which pipeline stage is currently executing
    std::atomic<const char*> m_currentStage{ "idle" };
    std::atomic<bool>        m_watchdogRunning{ false };
    HANDLE                   m_watchdogThread = nullptr;
    LARGE_INTEGER            m_stageEnteredTime = {};

    static DWORD WINAPI WatchdogThreadProc(LPVOID param);
    void StartWatchdog();
    void StopWatchdog();
    void FlushPerfStats();
    void LogSystemInfo();
    void LogDwmStatus();
    void SetOverlayCursorVisible(bool visible);
};
