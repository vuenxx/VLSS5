# VLSS5 — Teknik Referans

> Bu belge projenin **mimari haritasıdır**. Amaç: yeni bir oturumda kod tabanını baştan analiz etmeden "ne nerede, neden öyle" sorularını cevaplayabilmek.
>
> **Son güncelleme:** 2026-09-15 · `master` · commit `a38f3d8` sonrası çalışma ağacı

---

## 1. Proje Nedir?

VLSS5, çalışan bir oyunu **Windows Graphics Capture (WGC)** ile yakalar, kareyi **NVIDIA DLSS 5 Neural Rendering** (ve/veya klasik DLSS + optik akış) yığınından geçirir, sonucu oyunun üzerine bindirilen **şeffaf, tıklama-geçirgen bir overlay penceresinde** sunar.

Yani bir HUD/overlay uygulaması **değil** — bir *yeniden sunum* (re-projection) katmanıdır. Oyunun kendi çıktısı overlay tarafından tamamen kapatılır; kullanıcının gördüğü kare bizim swap chain'imizden gelir.

Bu ayrım, belgedeki çoğu tasarım kararının sebebidir.

**Platform:** Windows 10/11 x64 · **Toolset:** MSVC v143 · **Standart:** C++17 · **Subsystem:** Windows (GUI)

---

## 2. Derleme

```bash
msbuild VLSS5.sln /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
```

MSBuild yolunu `vswhere` ile bulmak gerekirse:

```bash
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
```

**Çıktı:** `x64\Release\VLSS5.exe`

**Referans hedef:** 0 uyarı / 0 hata. Bir uyarı belirirse gerçekten yenidir; görmezden gelinmemeli.

### Çalışma zamanı dosyaları — EXE'nin YANINDA

| Dosya | Ne |
|---|---|
| `vlss5_config.ini` | Tüm ayarlar (`ConfigManager::GetIniPath()`) |
| `vlss5_logs.log` | Tanılama günlüğü (`DLSS_Log`, `DLSSManager.cpp:11`) |

> ⚠️ Bunlar `%APPDATA%` altında **değil**, `GetModuleFileNameW` ile bulunan exe dizinindedir. Log her açılışta `"w"` moduyla **sıfırlanır** — bir çökme sonrası eski logu okumak istiyorsan programı yeniden başlatmadan al.

### Harici bağımlılıklar (repoda yok, çalışma zamanında aranır)

| DLL | Nereden | Kim yükler |
|---|---|---|
| `_nvngx.dll` | NVIDIA sürücü dizini veya exe yanı | `DLSSNRManager`, `DLSSManager` |
| `nvngx_dlssnr.dll` | exe yanı (snippet) | `DLSSNRManager` |
| `nvngx.dll_dlssnr.dll` | exe yanı (forwarder) | `DLSSNRManager` |
| `nvofapi64.dll` | NVIDIA Optical Flow SDK 5.0 | `NvOFManager` |
| `RTSSHooks64.dll` | RivaTuner kurulumu | `RTSSManager::NotifyRTSS()` |

Hepsi opsiyoneldir; yoksa ilgili özellik sessizce devre dışı kalır ve log'a düşer.

---

## 3. Dosya Haritası

| Dosya | Satır | Sorumluluk |
|---|---:|---|
| `src/App.cpp/h` | 1550 | **Merkez.** Overlay penceresi, render döngüsü, kısayollar, FPS kalibrasyonu, watchdog |
| `src/Main.cpp` | 1548 | `wWinMain`, ana menü penceresi (owner-draw GDI), pencere listesi, global hotkey |
| `src/SettingsWindow.cpp/h` | 1149 | VLSS5 ayar paneli (slider'lar, preset, split screen) |
| `src/Renderer.cpp/h` | 1099 | D3D11 swap chain, HLSL kaynağı, full-screen üçgen, OSD |
| `src/D3D12Interop.cpp/h` | 784 | D3D11↔D3D12 paylaşımlı doku + fence köprüsü |
| `src/DLSSNRManager.cpp/h` | 637 | DLSS 5 Neural Rendering (NGX Feature 18, D3D12) |
| `src/DLSSManager.cpp/h` | 635 | Klasik DLSS (NGX Feature 1, D3D11) + `DLSS_Log` tanımı |
| `src/MotionVectorManager.cpp/h` | 627 | Optik akış + dinamik UI reaktif maske (DirectCompute) |
| `src/CaptureManager.cpp/h` | 505 | WGC yakalama, zero-copy SRV, giriş FPS & jitter ölçümü |
| `src/NvOFManager.cpp/h` | 364 | Donanımsal NVIDIA Optical Flow (OFA) sarmalayıcı |
| `src/HotkeysWindow.cpp/h` | 348 | Kısayol atama penceresi |
| `src/RtssWindow.cpp/h` | 286 | RTSS dizini/profil yönetim penceresi |
| `src/ConfigManager.cpp/h` | 212 | `Dlss5Config` + INI okuma/yazma (singleton) |
| `src/InputForwarder.cpp/h` | 138 | Kısayol yakalama (hook **yalnızca** atama sırasında) |
| `src/RTSSManager.cpp/h` | 112 | RTSS profil `.cfg` okuma/yazma |
| `src/WindowEnumerator.cpp/h` | — | Görünür üst-seviye pencere listesi |
| `src/Common.h` | — | Ortak include'lar, `#pragma comment(lib,...)`, `WindowInfo`, `DLSS_Log` bildirimi |

---

## 4. Yaşam Döngüsü

```
wWinMain (Main.cpp)
  └─ Ana menü penceresi (owner-draw GDI, koyu tema)
       ├─ WindowEnumerator::GetWindows()  → hedef listesi
       ├─ [BAŞLAT] veya Alt+S
       │     ShowWindow(menu, SW_HIDE)
       │     App::StartOverlay(menu, target, vsync, dlss, fps, fullscreenStretch)
       │       ├─ InitD3D()                    → paylaşılan ID3D11Device
       │       ├─ CreateOverlayWindow(target)  → topmost katmanlı pencere
       │       ├─ CaptureManager::Start()      → WGC oturumu
       │       └─ Renderer::Init(cap, out)     → swap chain + DLSS yığını
       │     App::Run()                        ← BLOKLAR (iç içe modal döngü)
       │       └─ … render döngüsü …
       │     App::StopOverlay()                ← Run() dönerken
       │     ShowWindow(menu, SW_SHOW)
       └─ …
```

`Run()` **çağıran thread'i bloklar** — ayrı bir thread değildir. Menü penceresinin mesaj pompası bu süre boyunca `App::Run` içindeki `PeekMessageW` döngüsüne devredilir.

### Render döngüsü (`App::Run`, App.cpp:~410)

```
timeBeginPeriod(1)
AvSetMmThreadCharacteristicsW("Games" → "Capture")   // MMCSS
SetThreadPriority(THREAD_PRIORITY_HIGHEST)

while (m_running):
  1. PeekMessage döngüsü (WM_QUIT / WM_HOTKEY → çık)
  2. IsWindow(target)? değilse çık
  3. Update()                       → kısayollar, odak, konum, kalibrasyon
  4. m_overlayHidden ise: kareyi boşalt, Sleep(25), continue
  5. Yeni kare var mı?
       evet → AcquireCurrentFrameSRV() → Render(srv) → ReleaseCurrentFrame()
       hayır → MsgWaitForMultipleObjectsEx(frameEvent, 2ms, QS_ALLINPUT)
```

`Render()` içinde sırasıyla: FPS sayacı (500 ms) → `Renderer::RenderFrame()` → `Renderer::Present()` → zamanlama istatistikleri.

### Threading modeli

| Thread | Ne yapar |
|---|---|
| **Ana thread** | Menü, overlay penceresi, render döngüsü, D3D11 immediate context — **her şey** |
| **WGC pool thread** | `FrameArrived` geri çağrısı (free-threaded). Yalnızca sayaç/atomik yazar |
| **Watchdog thread** | 500 ms'de bir uyanır; render thread'i bir aşamada 2 sn'den uzun takıldıysa log'a yazar |

> D3D11 immediate context yalnızca ana thread'den kullanılır. `FrameArrived` içinde D3D çağrısı yapılmaz — yalnızca `m_newFrame` atomik bayrağı set edilir ve `m_frameEvent` sinyallenir.

---

## 5. Çözünürlük Mimarisi — İKİ ayrı çözünürlük var

Bu, kodun en kolay yanlış anlaşılan kısmı. **Ayrımı bozma.**

| | `m_width` / `m_height` | `m_outWidth` / `m_outHeight` |
|---|---|---|
| Adı | **Pipeline** çözünürlüğü | **Çıkış** çözünürlüğü |
| Nedir | Yakalanan karenin boyutu | Swap chain / viewport / HUD boyutu |
| Kim kullanır | DLSS, DLSS-NR, MotionVector, NvOF, D3D12Interop | `ResizeBuffers`, `D3D11_VIEWPORT`, `g_screenSize` |

`Renderer::ApplyOutputSize(outW, outH)` — `outW/outH <= 0` verilirse çıkış pipeline'a **eşitlenir** (klasik 1:1 mod).

### "Tam Ekran Yap" bu ayrım üzerine kurulu

`Dlss5Config::fullscreenStretch` açıkken:

- `App::ComputeOverlayRect()` hedefin DWM çerçevesi yerine **monitörün `rcMonitor`'ünü** döndürür
- `App::StartOverlay` → `Renderer::Init(capW, capH, monW, monH)`
- Gerdirme, full-screen üçgenin son geçişinde **GPU filtrelemeyle bedelsiz** olur
- DLSS yığını yakalama çözünürlüğünde çalışmaya devam eder — dokunulmadı

**Kritik kural:** klasik modda `outW/outH` olarak **0 geçilir**, pencere dikdörtgeni değil. Sebep: DWM çerçeve sınırı ile WGC yakalama boyutu DPI/kenarlık yüzünden bir-iki piksel sapabilir; bunu "gerdirme" sanıp lineer filtreye düşmek 1:1 modda gereksiz bulanıklık yaratır.

```cpp
// App.cpp:289 ve App.cpp:582 — ikisi de aynı kalıp
const int outW = m_fullscreenStretch ? (m_lastTargetRect.right - m_lastTargetRect.left) : 0;
```

`g_stretchActive` sabit-arabellek bayrağı (`m_outWidth > m_width`) piksel gölgelendiricide örnekleyiciyi seçer: 1:1'de **nokta** (HUD/yazı keskin kalsın), gerdirmede **lineer** (blok blok görünmesin).

---

## 6. Overlay Penceresi — Dokunmadan Önce Oku

```cpp
// App.cpp:215
CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
    L"VLSS5Overlay", L"VLSS5 Overlay", WS_POPUP, x, y, w, h, ...);

SetLayeredWindowAttributes(m_overlayHwnd, 0, 254, LWA_ALPHA);   // App.cpp:227
```

### `WS_EX_LAYERED` ZORUNLUDUR — kaldırma

Bu bir kez denendi ve geri alındı. Gerekçeyi kaybetme:

`WS_EX_TRANSPARENT` **tek başına bir çizim bayrağıdır**. İşletim sistemi seviyesinde hit-test muafiyetini **yalnızca `WS_EX_LAYERED` ile birlikte** verir.

| | İmlecin altındaki pencere | Sonuç |
|---|---|---|
| **LAYERED var** | Oyun (overlay hit-test'ten OS seviyesinde muaf) | Oyunun `ShowCursor(FALSE)` çağrısı geçerli → imleç yok ✅ |
| **LAYERED yok** | Overlay | `WM_SETCURSOR` bize gelir, `DefWindowProc` ok çizer → ekranın ortasında çakılı imleç ❌ |

`WM_NCHITTEST → HTTRANSPARENT` (App.cpp:1329) tıklamaları oyuna geçirir ama **imleç sahipliğini değiştirmez**. GoldSrc (CS 1.6) gibi her karede `SetCursorPos(merkez)` ile imleci ışınlayan motorlarda ok tam ortada sabit kalır.

Layered'ın bedeli: Direct Flip / Independent Flip / MPO devre dışı; `DXGI_PRESENT_ALLOW_TEARING` yok sayılır. **Bu bilinçli bir takas — doğruluk hızdan önce gelir.**

### Alpha neden 254?

255 tam opak demektir ve DWM alttaki oyun penceresini **tamamen kapalı (occluded)** olarak sınıflandırır; oyun kendi Direct Flip / Reflex / Frame Generation zamanlamasını kısar. 254 göze 255 ile aynıdır ama bu sınıflandırmayı engeller.

### Diğer pencere davranışları

| Mesaj / durum | Konum | Davranış |
|---|---|---|
| `WM_NCHITTEST` | App.cpp:1329 | Oyun modunda `HTTRANSPARENT`, overlay odaklıyken `HTCLIENT` |
| `WM_MOUSEACTIVATE` | App.cpp | `MA_NOACTIVATE` (oyun modunda) |
| `WM_SETCURSOR` | App.cpp:1340 | Overlay odaklıysa ok, değilse dokunma |
| Konum takibi | `UpdateOverlayPosition()` | Klasik: `GetWindowRect` hızlı kontrol + 100 ms'de bir DWM sorgusu. Tam ekran: yalnızca **monitör değişince** |
| Otomatik gizleme | `CheckFocusAndMinimize()` | Hedef odağı kaybederse/minimize olursa `SW_HIDE` + DLSS geçmişi sıfırlanır |

> `UpdateOverlayPosition()` içinde `DwmGetWindowAttribute` IPC'dir ve pahalıdır. Bu yüzden önce `GetWindowRect` ile ucuz değişiklik kontrolü yapılır, DWM sorgusu 100 ms'de bire kısılır. Bu optimizasyonu kaldırma.

---

## 7. Görüntü Boru Hattı

```
Oyun penceresi
   │  WGC FrameArrived (pool thread)
   ▼
CaptureManager ── zero-copy ──► ID3D11ShaderResourceView
   │                             (SRV önbelleği, 8 girişli, texture pointer'a göre)
   ▼
Renderer::RenderFrame
   │
   ├─ YOL A: DLSS 5 Neural Rendering (tercih edilen)
   │    D3D12Interop::BeginFrame(inputTex, srv, mvMgr)
   │      ├─ D3D11 → paylaşımlı D3D12 dokusuna kopya (veya ölçek < %100 ise bilinear downscale)
   │      └─ MotionVectorManager / NvOF → paylaşımlı MV dokusu
   │    DLSSNRManager::Evaluate(cmdList, color, output, motion)   ← NGX Feature 18
   │    D3D12Interop::EndFrame()  → fence ile D3D11'e geri senkron
   │
   └─ YOL B: Geri düşüş (DLSS-NR yoksa/kapalıysa)
        MotionVectorManager::ProcessFrame(ctx, srv)
        DLSSManager::Evaluate(...)                                ← NGX Feature 1
   │
   ▼
Full-screen üçgen (piksel gölgelendirici)
   t0 gModelTex   = DLSS-NR çıktısı      (veya renderSRV)
   t1 gProxyTex   = ham D3D12 girişi     (veya renderSRV)
   t2 gOriginalTex= yakalanan kare       (veya renderSRV)
   t3 gFpsTex     = FPS rozeti (GDI ile CPU'da çizilir)
   t4 gWarningTex = uyarı kutusu
   s0 nokta örnekleyici · s1 lineer örnekleyici
   ▼
Renderer::Present()  →  overlay swap chain
```

### Piksel gölgelendiricinin harmanlama mantığı

- `g_dlssnrActive == 0` → doğrudan `gOriginalTex` (geçiş modu)
- `g_isSubNative == 0` → `lerp(original, boostedModel, intensity)`
- `g_isSubNative == 1` → **OptiScaler / RenoDX Luminance-Ratio Transfer**: yüksek frekanslı geometri, kenar, HUD ve yazı %100 native kareden gelir; DLSS 5'in nöral aydınlatma/gürültü giderme etkisi bir *oran* olarak aktarılır. Bu, model çözünürlüğü %50-99 iken bulanıklığı önler.
- `boostedModel = saturate(raw + (model - raw) * g_boostFactor)` — lineer ekstrapolasyon

### Swap chain (Renderer::CreateSwapChain)

```
Format      B8G8R8A8_UNORM     (WGC çıktısıyla eşleşir)
BufferCount 3                  (2 idi — bkz. aşağıdaki backpressure notu)
Scaling     DXGI_SCALING_STRETCH
SwapEffect  FLIP_DISCARD
Flags       ALLOW_TEARING | FRAME_LATENCY_WAITABLE_OBJECT
IDXGISwapChain2::SetMaximumFrameLatency(1)
MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)
```

Geri düşüş zinciri: waitable → waitable'sız → blt-model (`DISCARD`, BufferCount 1).

`Present(vsync ? 1 : 0, tearing ? ALLOW_TEARING : 0)`. 50 ms üzeri Present log'a uyarı düşürür.

### ⚠️ Present backpressure — bu mimarinin ana darboğazı

Layered pencere DWM kompozisyonundan geçer; Direct Flip / MPO yoktur (§6). `syncInterval=0` ile sınırsız kare basarsak DWM kuyruğu kompozisyon hızında boşaltır, kuyruk dolunca **`Present()` bir arabellek serbest kalana kadar bloke olur**.

Ölçülen (2026-09-15 logu, ETS2, 165 Hz):

| | Süre |
|---|---|
| `render` (yakalama + DLSS + shader) | **0.91 – 1.14 ms** |
| `present` | **31 – 143 ms** |

Yani boru hattının tamamı ~1 ms; geri kalan **her şey** `Present()` içinde bloke geçiyordu.

**Uygulanan çözüm — `WaitForPresentReady()`:**

```cpp
// App::Run, kare üretiminden ÖNCE
m_renderer->WaitForPresentReady();   // frame-latency waitable object üzerinde bekler
Render(srv);
```

Bloke etme noktasını döngünün sonundan başına taşır:

| | Önce | Sonra |
|---|---|---|
| `Present()` | 31–143 ms bloke | anında döner |
| Atılacak kareler | yakalanır, DLSS'ten geçirilir, atılır | hiç üretilmez |
| Tempo | kuyruk taşması → düzensiz | kompozisyon hızına kilitli |
| Gecikme | kuyrukta biriken kareler | `SetMaximumFrameLatency(1)` |

**Kritik:** `ResizeBuffers` oluşturma bayraklarının **aynısını** almak zorunda — bu yüzden `m_swapChainFlags` saklanıyor. Waitable bayrağını düşürmek nesneyi sessizce geçersiz kılar.

Waitable, `WS_EX_LAYERED` ile tam uyumludur; fare geçirgenliğinden ödün vermez.

---

## 8. FPS Kalibrasyonu (`App::UpdateCalibration`, App.cpp:~790)

### Amaç

Oyunun kare üretim hızını, VLSS5'in işleyebildiği hıza **RTSS FPS limiti** ile kilitlemek. Giriş çıkıştan hızlıysa WGC kareleri kuyrukta birikip atılır → girdi gecikmesi + stutter.

### Durum makinesi — ikili arama

```
Idle
 └─(kısayol)→ InitUncap    limit=0 (SINIRSIZ), ölç
       │       üst sınır = max(ölçülen giriş, MONİTÖR YENİLEME HIZI)
       │
       ├─ senkron? → Finish(0)                    "limit gerekmiyor"
       └─ değil    → Bisect   lo=kCalibMinFps, hi=üst sınır
              │
              │  her adımda hedef = (lo + hi) / 2
              ├─ senkron + giriş < hedef-5 → Finish(giriş)   "oyunun kendi tavanı"
              ├─ senkron                   → lo = hedef
              └─ desenkron                 → hi = hedef
              │
              └─ hi - lo <= kCalibResolution → Finish(lo)
```

Her adım `kCalibStepMs = 3000` ms bekler (+ bir oturma tiki = pratikte 6 sn/sonda). Sabitler `App.h`'de.

### ⚠️ Üst sınır neden monitör yenileme hızı?

İlk sürüm üst sınırı **sınırsız fazda ölçülen giriş FPS'ine** eşitliyordu. Bu yanlıştı ve şöyle tezahür etti: arama 10'dan sonra 20'ye çıkamayıp **16'ya kırpıldı** ve orada "tamamlandı" dedi.

Sebep döngüseldi: sınırsız fazda hiçbir limit olmadığı için `Present()` DWM backpressure'ına girer. Gerçek bir oturumun logu:

| Faz | Kare maliyeti | Present | Ölçülen giriş |
|---|---|---|---|
| Sınırsız (ölçüm anı) | **84 ms** | 60–100 ms | **16 FPS** |
| Limit uygulandıktan sonra | **1.65 ms** | normal | — |

Yani *kendi tıkanıklığımızı* "oyunun tavanı" sanıp aramayı oraya hapsediyorduk. Boru hattı gerçekte 1.65 ms/kare (600+ FPS) kapasitesindeydi.

DWM ile kompoze edilen bir overlay zaten monitör yenileme hızının üzerine çıkamaz — gerçek üst sınır budur. Oyun kendi iç limiti yüzünden daha düşükte kalıyorsa `giriş < hedef - 5` dalı onu ayrıca yakalar.

### Altı emniyet — hepsi gerçek bir hata raporundan doğdu

| # | Emniyet | Neden var |
|---|---|---|
| 1 | **`kCalibMinFps = 10` tabanı** | RTSS'te `Limit=0` **SINIRSIZ** demektir. İlk sürüm ince aramada sınırsız aşağı sayıyordu; sayaç 0'ı geçince oyun tam hıza salınıyor, desenkron kalıcılaşıyor ve hedef `-1, -2, … -30` diye kaçıyordu. Bildirilen **"-30 FPS"** hatası tam olarak buydu. İkili arama bunu ayrıca yapısal olarak imkânsız kılar: hedef daima `[lo, hi]` aralığının içinde kalır |
| 2 | **"RTSS uygulamıyor" tespiti** | Limit yazıldığı halde giriş `hedef*1.5+10` üzerinde kalıyorsa RTSS profili gerçekten uygulamıyordur. Üst üste 2 tikte iptal. Eskiden bu durum sonsuz desenkron olarak okunup (1)'deki kaçışı tetikliyordu |
| 3 | **Bayat ölçüm koruması** | `m_currentInputFps` yalnızca yeni WGC karesi gelince, `m_fpsCurrent` yalnızca `Render()` çalışınca güncellenir. İkisi de donabilir. `GetInputFpsFresh()` / `GetOutputFpsFresh()` bayatsa 0 döner, tik **atlanır ve sayılmaz** |
| 4 | **Oturma (settle) süresi** | Limit değişince `ResetInputFpsWindow()` çağrılır ve bir tik atlanır; eski rejimin kareleri karara karışmaz |
| 5 | **`kCalibMaxSteps = 60` tavanı** | Her ne olursa olsun ~3 dk sonra durur |
| 6 | **Geri yükleme** | Başlarken `GetFramerateLimit()` saklanır; iptal, hata ve `StopOverlay()` yollarının **hepsinde** geri yazılır. Yoksa oyun 10 FPS'te takılı kalırdı |

### Senkron toleransı

```cpp
const int tol = max(2, (int)(inFps * 0.05 + 0.5));
return abs(inFps - outFps) <= tol;
```

Sabit `<= 1` toleransı fazla dardı: 0.5 sn'lik pencerede tamsayıya yuvarlanan FPS doğal olarak ±2 oynar ve sahte desenkron üretir.

### İptal

Kalibrasyon tuşuna **tekrar basmak** iptal eder ve kullanıcının limitini geri yükler.

### `RTSSManager` sözleşmesi

| Metot | Not |
|---|---|
| `GetFramerateLimit(exe)` | `[Framerate] Limit`. Profil yoksa 0 |
| `SetFramerateLimit(exe, n)` | `n < 0` **reddedilir** ve log'a düşer. `LimitDenominator=1` de yazılır |
| `NotifyRTSS()` | `RTSSHooks64.dll` → `UpdateProfiles()` |

> ⚠️ `NotifyRTSS()` her çağrıda DLL'i **kendi sürecimize** `LoadLibrary`/`FreeLibrary` yapıyor. Bunun RTSS sunucu sürecindeki profilleri gerçekten yeniden yükletip yükletmediği **doğrulanmadı**. Emniyet #2 tam da bu belirsizliği görünür kılmak için var — log'da `RTSS limiti uygulanmiyor` görürsen önce buraya bak.

---

## 9. Yapılandırma (`Dlss5Config`, ConfigManager.h)

INI bölümü `[VLSS5]`, kısayollar `[Hotkeys]`.

### DLSS / görüntü

| Alan | Varsayılan | Aralık / anlam |
|---|---|---|
| `preset` | 0 | 0=Varsayılan, 1-3 |
| `style` | 0 | 0=Standart, 1=Doğal, 2=Film |
| `intensity` | 1.0 | 0.0 – 2.0 |
| `localStructure` | 1.0 | 0.0 – 2.0 · yüzey detayı |
| `localTone` | 1.0 | 0.0 – 2.0 · mikro kontrast |
| `skinStructure` | -1.0 | -1.0 = otomatik takip |
| `useAutoMask` | true | otomatik ten maskesi |
| `resolutionScale` | 100 | 50 – 100 (%) · `< 100` → `isSubNative` yolu |
| `temporalStabilizer` | false | zorunlu reset bayrağı |
| `opticalFlow` | true | optik akış MV |
| `boostFactor` | 1.0 | 1.0 – 2.5 |
| `splitScreen` / `splitPos` | false / 0.5 | karşılaştırma modu |

### Davranış

| Alan | Varsayılan | Not |
|---|---|---|
| `fullscreenStretch` | false | **Tam Ekran Yap** (bkz. §5) |
| `directFlip` | false | ⚠️ **ÖLÜ KOD** — bkz. §11 |
| `selectedGpu` | `"Auto"` | adaptör açıklaması |
| `rtssDirectory` | `""` | boşsa `C:\Program Files (x86)\RivaTuner Statistics Server\Profiles` |

### Kısayollar (hepsi yeniden atanabilir — `HotkeysWindow`)

| Alan | Varsayılan | İşlev |
|---|---|---|
| `settingsVk` / `settingsMod` | `INSERT` | Ayar paneli |
| `vkFgIndicator` | `F7` | FG göstergesi |
| `vkFocus` | `F8` | Overlay ↔ oyun odağı |
| `vkFps` | `F9` | FPS göstergesi |
| `vkToggleVlss` | `F10` | VLSS5 aç/kapa |
| `vkCalib` | `F2` | FPS kalibrasyonu (tekrar bas = iptal) |
| `vkStart` / `modStart` | `Alt+S` | Başlat / durdur |

Kısayol algılama **hook'suz**: render döngüsünde `GetAsyncKeyState` ile yoklanır (`InputForwarder.h`'deki gerekçeye bak). `WH_KEYBOARD_LL` **yalnızca** menüdeki kısayol atama akışında kurulur ve ilk tuşta kaldırılır. Bu, ReShade ve oyunların tuşları eksiksiz almasını garanti eder — **bozma**.

---

## 10. Tanılama

### Log etiketleri

| Etiket | Kaynak | Ne zaman |
|---|---|---|
| `[App]` | App.cpp | Oturum olayları, odak değişimi |
| `[Calib]` | App.cpp | Her kalibrasyon adımı, iptal, tamamlanma |
| `[Capture]` | CaptureManager.cpp | WGC kare boşluğu > 200 ms, 5 sn özetler |
| `[Renderer]` | Renderer.cpp | Swap chain kurulumu |
| `[Present]` | Renderer.cpp | Present > 50 ms, `DXGI_STATUS_OCCLUDED` |
| `[Perf]` | App.cpp | `YAVAS KARE` (ort × 3), 5 sn özet (**render/present kırılımlı**) |
| `[Watchdog]` | App.cpp | Bir aşamada > 2000 ms takılma |
| `[DLSS]` / `[DLSS-NR]` | DLSSManager / DLSSNRManager | NGX yükleme, feature oluşturma, evaluate sonucu |
| `[RTSS]` | RTSSManager.cpp | Reddedilen negatif limit |

```bash
Get-Content ".\x64\Release\vlss5_logs.log" -Tail 80
```

### Ölçüm noktaları

| Ne | Nerede | Pencere |
|---|---|---|
| Giriş FPS (WGC) | `CaptureManager::m_currentInputFps` | 0.5 sn |
| Çıkış FPS (overlay) | `App::m_fpsCurrent` | 0.5 sn |
| Kare boşluğu jitter | `m_gapHistory[120]` | 120 örnek kayan pencere |
| Kare süresi | `App::Render` (`renderMs`, `presentMs`, `totalMs`) | kare başına |

---

## 11. Bilinen Riskler ve Ölü Kod

### 🔴 `directFlip` ölü yapılandırma

`Dlss5Config::directFlip` hâlâ INI'ye yazılıp okunuyor (`ConfigManager.cpp:137, 195`) ve `App::m_directFlip` üyesi duruyor — **ama `App.cpp` artık hiçbir yerde okumuyor.** Overlay her zaman layered oluşturuluyor.

Bu bilinçli: Direct Flip denendi, fare/imleç davranışını bozdu, geri alındı (§6). Ya temizlenmeli ya da tekrar bağlanmalı; şu anki hâli yanıltıcı.

### 🟢 Present backpressure — waitable swap chain ile ele alındı

Bkz. §7. Layered'ın kompozisyon tavanı hâlâ geçerli (overlay monitör yenileme hızının üzerine çıkamaz), ama kuyruk taşması ve ondan doğan jitter giderildi.

### 🟢 `NotifyRTSS()` — çözüldü

Eskiden **her çağrıda** `RTSSHooks64.dll` `LoadLibraryW` + `FreeLibrary` ediliyordu, üstelik **render thread'inden**. Bir hooking kütüphanesini sürece alıp çıkarmak `DLL_PROCESS_ATTACH/DETACH` yollarını işletir ve loader kilidini tutar.

Ölçülen sonuç: her limit yazma işleminden **~140 ms sonra** tek bir dev Present stall'ı —

```
[19:10:39.695] [Calib] Limit uygulandi: 33  →  [19:10:39.837] present=141.24 ms
[19:10:45.701] [Calib] Limit uygulandi: 35  →  [19:10:45.846] present=128.75 ms
[19:10:51.718] [Calib] Limit uygulandi: 36  →  [19:10:51.863] present=143.79 ms
```

Bu stall'lar kalibrasyon ölçümlerini de zehirliyordu. Modül artık bir kez yüklenip süreç ömrü boyunca tutuluyor; `FreeLibrary` yok.

### 🟡 Thread'ler arası okuma

`m_lastFrameArrivalTime` (LARGE_INTEGER) WGC pool thread'inde yazılıp render thread'inde okunuyor; atomik değil. x64'te 8 baytlık hizalı okuma pratikte bölünmez, ama biçimsel olarak veri yarışı. `m_currentInputFps` (int) için de aynı durum. Şimdiye kadar sorun çıkarmadı.

### 🟡 `FrameTimings` yapısı kullanılmıyor

`App.h`'deki `struct FrameTimings` tanımlı ama hiçbir yerde örneklenmiyor. Zamanlama yerel değişkenlerle yapılıyor.

---

## 12. Bir Şeye Dokunmadan Önce

| Değiştireceğin şey | Önce oku | Kırılma riski |
|---|---|---|
| Overlay pencere stilleri | §6 | Fare/imleç her oyunda bozulur |
| `Renderer::Resize` / `Init` imzası | §5 | 1:1 modda bulanıklık, DLSS yeniden boyutlanma çökmesi |
| Kalibrasyon durum makinesi | §8 | Negatif FPS kaçışı geri gelir |
| Kalibrasyon üst sınırı | §8 | Arama kendi tıkanıklığımıza hapsolur (16 FPS hatası) |
| `GetAsyncKeyState` yoklaması | `InputForwarder.h` | Oyunlar tuş kaybeder |
| Alpha 254 | §6 | Oyunun FG/Reflex zamanlaması kısılır |
| `UpdateOverlayPosition` kısma mantığı | §6 | Kare başına DWM IPC → stutter |
| Piksel gölgelendirici cbuffer | `Renderer.cpp:39` | 16 bayt hizalama; `desc.ByteWidth` ile eşleşmeli |

---

## 13. Terimler

| Kısaltma | Açılım |
|---|---|
| **WGC** | Windows Graphics Capture |
| **NGX** | NVIDIA NGX — DLSS'in altındaki SDK. Feature 1 = DLSS, Feature 18 = Neural Rendering |
| **MV** | Motion Vector (hareket vektörü) |
| **OFA / NvOF** | NVIDIA Optical Flow Accelerator — SM/CUDA kullanmayan ayrı donanım bloğu |
| **MPO** | Multi-Plane Overlay — DWM'i atlayan donanımsal katman |
| **iFlip** | Independent Flip |
| **FG** | Frame Generation |
| **RTSS** | RivaTuner Statistics Server |
| **MMCSS** | Multimedia Class Scheduler Service |
