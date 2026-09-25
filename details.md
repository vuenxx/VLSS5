# VLSS5 ↔ RenoDX Karşılaştırma Raporu

> **Amaç:** RenoDX'in (`renodx-main/`) neyi bizden iyi yaptığını tespit edip kendimize çekmek.
> **Kapsam:** Mimari, özellik seti ve uygulama biçimi. Kod satır referansları verilmiştir.
> **Tarih:** 2026-09-16 · VLSS5 `master` (`a38f3d8` sonrası çalışma ağacı) · RenoDX snapshot (`renodx-main/`, 2026-09-05)

---

## 0. Yönetici Özeti — 60 saniyede

| | VLSS5 | RenoDX |
|---|---|---|
| **Temel yaklaşım** | Oyunu **dışarıdan** WGC ile yakala, DLSS-NR'den geçir, üstüne overlay bas | Oyunun **içine** ReShade addon olarak gir, boru hattını kaynağında değiştir |
| **Kod büyüklüğü** | ~12.5k satır C++ | ~30k satır util + 21k satır mod/addon + 244 oyun profili |
| **Bağımlılık** | Yok (opsiyonel: NGX, NvOF, RTSS) | ReShade **zorunlu** |
| **Ne yapar** | Nöral yeniden sunum (DLSS 5 NR), upscale, FPS kalibrasyon | HDR/tonemap dönüşümü, shader değiştirme, swapchain yükseltme, kaynak yükseltme |
| **Kapsam** | Her oyunda çalışır (jenerik) | Oyun başına elle hazırlanmış mod (bespoke) + jenerik fallback |
| **Renk uzayı** | Sadece `B8G8R8A8_UNORM` SDR | scRGB / HDR10 / `R16G16B16A16_FLOAT`, tam HDR yığını |
| **Görüntü kalitesi tavanı** | DWM kompozisyon tavanına takılı, 8-bit SDR | Oyunun kendi back buffer'ı, kayıpsız |

**Tek cümlelik fark:** Biz *kareyi görüntüledikten sonra* müdahale ediyoruz, onlar *kare oluşurken* müdahale ediyor. Bu, kalitede onların, evrensellikte bizim lehimize.

---

## 1. Mimari Karşılaştırma

### 1.1 Enjeksiyon noktası

**VLSS5** (bkz. `TECHNICAL.md` §7):
```
Oyun → DWM → WGC yakalama → bizim D3D11 → DLSS-NR → overlay swap chain → DWM → ekran
```
İki kere kompozisyondan geçiyoruz. Bunun bedeli belgelenmiş durumda: `WS_EX_LAYERED` zorunluluğu (`src/App.cpp:215`), Direct Flip / MPO kaybı, Present backpressure (`TECHNICAL.md` §7).

**RenoDX**:
```
Oyun → [ReShade addon: shader değiştir / swapchain yükselt / proxy pass] → oyunun kendi swap chain'i → ekran
```
Ekstra kompozisyon yok, ekstra pencere yok, ekstra kopya yok. Oyunun swap chain'ini `R16G16B16A16_FLOAT`'a yükseltip (`renodx-main/src/mods/swapchain_v2.hpp:71`) renk uzayını `extended_srgb_linear` yapıyorlar.

### 1.2 Ölçeklenme modeli

| | VLSS5 | RenoDX |
|---|---|---|
| Yeni oyun desteği | **Sıfır iş** — WGC her pencereyi yakalar | Oyun başına shader dump + decompile + HLSL yazımı |
| Oyuna özel davranış | Yok (tek global INI) | `src/games/` altında **244 ayrı addon** |
| Dağıtım | Tek `VLSS5.exe` | Oyun başına `renodx-{oyun}.addon64` + metadata (`renodx-metadata-schema.json`) |

Bu bir takas, doğrudan "iyi/kötü" değil. Ama RenoDX'in **altyapı/oyun ayrımı** (`src/utils/` + `src/mods/` paylaşılan, `src/games/` ince) bizim monolitik `App.cpp`'mizden çok daha temiz ve bizim de kopyalayabileceğimiz bir desen.

---

## 2. BİZİM DAHA İYİ YAPTIKLARIMIZ

### 2.1 ✅ Bağımsızlık — hiçbir üçüncü parti runtime gerektirmiyoruz
RenoDX'in **tamamı** ReShade'e bağımlı. ReShade yoksa, ReShade sürümü uyuşmuyorsa, anti-cheat ReShade'i engelliyorsa RenoDX hiç çalışmıyor. VLSS5 kendi `wWinMain`'i ile ayakta duruyor; NGX/NvOF/RTSS yoksa ilgili özellik sessizce kapanıyor (`TECHNICAL.md` §2).

**Yan fayda:** Anti-cheat açısından biz oyuna DLL enjekte etmiyoruz — WGC bir OS API'si. RenoDX'in `src/utils/dlss_hook.hpp` dosyası `LoadLibraryA/W`, `GetModuleHandleA/W` IAT hook'ları kuruyor (`renodx-main/src/utils/dlss_hook.hpp:27-60`) ve `src/utils/detour.hpp` Microsoft Detours kullanıyor. Bu, korumalı oyunlarda ölümcül.

### 2.2 ✅ Evrensel kapsam — oyun başına iş yok
244 klasörlük `src/games/` ağacı RenoDX'in gücü olduğu kadar zayıflığı. Desteklenmeyen oyunda RenoDX'in sunduğu tek şey `src/games/generic/addon.cpp` (614 satır, çoğu boş şablon: `// CustomShaderEntry(0x00000000)`). VLSS5 ilk çalıştırmada her oyunda bir şey yapıyor.

### 2.3 ✅ Kendi hotkey sistemi — ReShade'in insafına kalmadık
RenoDX'te **hiç kendi keybind sistemi yok**; `grep VK_ src/utils/*.hpp src/mods/*.hpp` sonucu boş. Her şey ReShade'in overlay tuşuna (`Home`) ve ImGui paneline bağlı. Yani "oyun içinde F9'a bas, FPS göster" gibi bir akış RenoDX'te yok.

Bizde `HotkeysWindow` ile yeniden atanabilir 7 kısayol var ve kritik olan, **hook kurmadan** `GetAsyncKeyState` yoklamasıyla çalışıyor (`TECHNICAL.md` §9). `WH_KEYBOARD_LL` yalnızca atama akışında kurulup ilk tuşta kaldırılıyor. Bu, ReShade ve oyunla tuş çakışmasını yapısal olarak imkânsız kılıyor — RenoDX'in çözemediği bir problem.

### 2.4 ✅ Gerçek nöral yeniden sunum (DLSS 5 Neural Rendering)
RenoDX'in DLSS ile ilişkisi **pasif**: `slDLSSSetOptions`'ı hook'layıp `colorBuffersHDR = eTrue` ve `useAutoExposure = eTrue` zorluyor (`renodx-main/src/utils/dlss/streamline_v2.hpp:791-797`). Yani oyunun **zaten sahip olduğu** DLSS'i düzeltiyor. Oyunda DLSS yoksa RenoDX DLSS ekleyemiyor.

VLSS5 NGX Feature 18'i (Neural Rendering) doğrudan çağırıyor (`src/DLSSNRManager.cpp`), D3D11↔D3D12 köprüsünü kendisi kuruyor (`src/D3D12Interop.cpp`, 784 satır) ve DLSS'i olmayan oyuna DLSS getiriyor. Bu RenoDX'in kapsamında bile değil.

### 2.5 ✅ Kendi optik akış / motion vector üretimi
`src/NvOFManager.cpp` (OFA donanım bloğu) + `src/MotionVectorManager.cpp` (DirectCompute reaktif UI maskesi). RenoDX'te motion vector üretimi yok — oyunun kendi MV'lerine güveniyor (ki addon olarak zaten erişimi var). Biz dışarıdan yakalanan bir karede MV'yi **sıfırdan üretiyoruz**, bu teknik olarak daha zor bir problem ve çözülmüş durumda.

### 2.6 ✅ Luminance-Ratio Transfer shader'ı
`src/Renderer.cpp:77-116` — sub-native modda yüksek frekanslı detayı native kareden, nöral aydınlatmayı *oran* olarak alan blend. Ratio floor (`1/512`), ratio clamp (`±2.0`), ayrı kroma transferi. RenoDX'in `src/shaders/` kütüphanesinde tonemap/gamut matematiği zengin ama bu spesifik "iki farklı çözünürlükteki kareyi faz uyumsuzluğu olmadan birleştirme" problemi yok — çünkü onların böyle bir problemi yok.

### 2.7 ✅ Kalibrasyon durum makinesinin savunma katmanları
`TECHNICAL.md` §8'deki 6 emniyet (min FPS tabanı, RTSS-uygulamıyor tespiti, bayat ölçüm koruması, settle süresi, adım tavanı, geri yükleme) gerçek hata raporlarından doğmuş ve belgelenmiş. RenoDX'in FPS limiter'ında böyle bir "otomatik hedef bulma" katmanı hiç yok — kullanıcı sayıyı elle giriyor. Adaptif bir sistemimiz var ve neden öyle olduğu yazılı.

### 2.8 ✅ Belgelenmiş tasarım gerekçeleri
`TECHNICAL.md` §6'daki "`WS_EX_LAYERED` ZORUNLUDUR — kaldırma" ve alpha 254 açıklaması gibi *neden* notları RenoDX'te dağınık. Onlarda `AGENTS.md` dosyaları (kodlama kuralları) ve `docs/` var ama "bu kararı neden aldık, geri alma" tarzı kurumsal hafıza notu yok. Bu bizim güçlü yanımız, kaybetmeyelim.

---

## 3. ONLARIN DAHA İYİ YAPTIKLARI (çalınacaklar)

### 3.1 🔥 FPS Limiter — dışarıya bağımlı değil, kendi kendini kalibre ediyor
**Dosya:** `renodx-main/src/utils/swapchain.hpp:710-805`

Bu, çalmamız gereken **1 numaralı** şey.

Bizim FPS sınırlama yolumuz: RTSS profil `.cfg` dosyasına yaz → `RTSSHooks64.dll` yükle → `UpdateProfiles()` → umut et. `TECHNICAL.md` §8 kendi kendine itiraf ediyor: *"Bunun RTSS sunucu sürecindeki profilleri gerçekten yeniden yükletip yükletmediği **doğrulanmadı**."* Üstüne kalibrasyon ikili araması ~3 dakika sürüyor (60 adım × 3 sn + settle).

RenoDX'inki **53 satırlık bir addon** (`renodx-main/src/addons/fpslimiter/addon.cpp`) ve altındaki algoritma şu:

```cpp
// Present sonrası, bir sonraki kare zamanına kadar bekle
wait_duration = time_till_next_frame - busy_spin_duration;
std::this_thread::sleep_for(wait_duration);          // veya HR waitable timer

// Uyandıktan sonra GERÇEK gecikmeyi ölç
wait_latency = actual_wait_duration - wait_duration;
wait_latency_history.push_back(wait_latency);        // 1000 örneklik deque

// En kötü %1 gecikmeyi bul, spin süresini ona göre ayarla
auto worst_1_percent = sorted_latencies[current_size * 0.01];
busy_spin_duration = worst_1_percent * 1.5;

// Kalan süreyi YieldProcessor() ile spin'le
while (now < next_time_point) { YieldProcessor(); now = clock::now(); }
```

**Neden bu daha iyi:**
- **Sıfır dış bağımlılık.** RTSS yok, profil dosyası yok, DLL yükleme yok, `NotifyRTSS()` stall'ı yok.
- **Kendi kendini ayarlıyor.** Scheduler'ın uyku gecikmesini ölçüp spin penceresini o kadar açıyor. Sabit "5 ms spin" gibi kaba bir tahmin değil.
- **İki yönlü adaptasyon.** Spin çok uzunsa (`busy_spin_failures > fps_limit`, yani bir saniyelik hata) %10 kısıyor (`:781-793`). Çok kısaysa en kötü %1'e göre uzatıyor.
- **`YieldProcessor()`** = `_mm_pause`, hyperthread kardeşini aç bırakmıyor. `Sleep(0)` / boş döngü değil.
- **Limit değişince geçmişi temizliyor** (`:717-721`) — bizim "settle" fikrimizin aynısı ama bedava.

> **VLSS5'e nasıl uygularız:** `App::Run`'da `WaitForPresentReady()` zaten doğru yerde duruyor. Yanına aynı hibrit sleep+spin bekleyiciyi koyarsak, RTSS'e hiç dokunmadan **çıkış** FPS'imizi hedefe kilitleyebiliriz. RTSS'i yine de **giriş** (oyun) tarafı için kullanmamız gerekiyor ama kalibrasyon araması çok daha hızlı yakınsar, çünkü kendi tarafımızdaki jitter ortadan kalkar.

---

### 3.2 🔥 Deklaratif ayar sistemi — UI'ı elle çizmiyorlar
**Dosya:** `renodx-main/src/utils/settings.hpp` (670 satır) + `renodx-main/src/templates/settings.hpp`

Bizde `SettingsWindow.cpp` **1149 satır** ve her slider elle çiziliyor; `ConfigManager.cpp` **212 satır** ve her alan elle INI'ye yazılıyor; `Dlss5Config` struct'ına bir alan eklemek **3 ayrı dosyada** değişiklik demek (struct + Load + Save + UI).

RenoDX'te bir ayar **tek bir veri yapısı**:

```cpp
new renodx::utils::settings::Setting{
    .key = "FPSLimit",
    .binding = &renodx::utils::swapchain::fps_limit,   // doğrudan float* bağlama
    .default_value = 0.f,
    .label = "FPS Limit",
    .min = 0.f,
    .max = 480.f,
}
```

Ve bu tek tanımdan şunların **hepsi** otomatik türüyor:
- UI widget'ı (`SliderFloat` / `SliderInt` / `Checkbox` / `Button` / `Combo` — `value_type`'a göre)
- INI okuma (`LoadSetting`, `:224`) ve yazma (`SaveSettings`, `:305`)
- Sınır kırpma (min/max, `:231-235`)
- Varsayılana sıfırlama (`ResetSettings`, `:200`)
- Shader constant buffer'ına yazma (`Write()`, `:135` — `binding` işaretçisi üzerinden)

Ekstra yetenekler, hepsi aynı struct'ta:

| Alan | Ne yapar | Bizde karşılığı |
|---|---|---|
| `.tooltip` | Hover açıklaması | ❌ Yok |
| `.section` / `.group` | Otomatik başlık/gruplama | Elle |
| `.is_visible` | Koşullu gizleme (lambda) | ❌ Yok |
| `.is_enabled` | Koşullu gri-out | ❌ Yok |
| `.labels` | int → metin (ör. `{"Off","2.2","BT.1886"}`) | Elle |
| `.parse` | Değeri shader'a yazmadan önce dönüştür | ❌ Yok |
| `.on_change_value` | Değişim callback'i (önceki, yeni) | Elle |
| `.is_logarithmic` | Log skalalı slider | ❌ Yok |
| `.packed_values` | Bit bayraklarını tek float'a paketle | ❌ Yok |
| `.is_global` | Preset'ten bağımsız, global kaydet | ❌ Yok |

> **VLSS5'e nasıl uygularız:** Bu, `SettingsWindow.cpp`'nin ~%70'ini silebilecek bir değişiklik. GDI owner-draw kullandığımız için ImGui'ye geçmek zorunda değiliz — `Setting` struct'ının veri kısmını alıp kendi GDI çizicimizi bir `for (auto* s : settings)` döngüsüne çevirmek yeterli. Tek seferlik iş, sonrasında her yeni ayar **tek satır**.

---

### 3.3 🔥 "Settings Mode" — Basit / Orta / Gelişmiş kademesi
**Dosya:** `renodx-main/src/templates/settings.hpp:52-59`

```cpp
.key = "SettingsMode",
.labels = {"Simple", "Intermediate", "Advanced"},
.is_global = true,
```
ve her ayarda:
```cpp
.is_visible = []() { return current_settings_mode >= 1; },
```

Kullanıcı "Simple" modda 3-4 slider görüyor; "Advanced"te 40 tane. Bizim ayar panelimizde `intensity`, `localStructure`, `localTone`, `skinStructure`, `boostFactor`, `resolutionScale`, `preset`, `style`... hepsi aynı anda ekranda. Yeni kullanıcı için bunaltıcı, ileri kullanıcı için de bir şey kaybettirmiyor.

Uygulama maliyeti neredeyse sıfır (bir global int + her ayarda bir görünürlük koşulu), kazanç büyük.

---

### 3.4 🔥 Canlı shader yeniden yükleme (hot reload)
**Dosya:** `renodx-main/src/utils/shader_compiler_watcher.hpp` (797 satır)

Bir klasörü izliyor, `.hlsl` / `.glsl` / `.slang` dosyası değişince **oyun çalışırken** yeniden derleyip pipeline'a basıyor. Derleme hatası olursa `std::variant<std::exception, std::vector<uint8_t>>` ile hatayı tutup eski shader'ı bozmadan UI'da gösteriyor (`:35-63`). HLSL `#include` bağımlılıklarını da izliyor (`hlsl_dependencies.hpp`).

Bizde `Renderer::CompileShaders` shader'ı **string literal olarak `Renderer.cpp:30-140`'ta gömülü**. Pixel shader'da bir sabiti (`kMaxRatio`, `kRatioFloor`, kLuma ağırlıkları) denemek için: derle → başlat → oyunu aç → bak → kapat. Döngü başına dakikalar.

> **VLSS5'e nasıl uygularız:** Tam bir watcher'a gerek yok. Minimum sürüm: pixel shader'ı `vlss5.hlsl` diye exe yanına çıkar, F5'e basınca `D3DCompileFromFile` ile yeniden derle, başarısızsa eskisini koru. ~80 satır, shader iterasyon hızımızı 10× artırır.

---

### 3.5 🔥 Preset kaydetme yuvaları
**Dosya:** `renodx-main/src/utils/settings.hpp:292-306`

```cpp
static std::string GetCurrentPresetName() {
  switch (preset_index) {
    case 1: return global_name + "-preset1";
    ...
```
Off / Preset #1 / #2 / #3. Ayrılar INI'de ayrı section'lara yazılıyor, preset değişince `LoadSettings` ile hepsi yeniden yükleniyor, `on_preset_changed_callbacks` tetikleniyor.

Bizdeki `Dlss5Config::preset` **bu değil** — o DLSS'in kendi iç preset'i (0-3). Kullanıcının "gündüz ayarım / gece ayarım / oyun A ayarım" diye kaydedeceği bir yuva sistemimiz **yok**. Tek bir `[VLSS5]` section'ımız var, üzerine yazılıyor.

---

### 3.6 🔥 Oyun başına ayar ayrımı
RenoDX ReShade'in config sistemini kullandığı için ayarlar **oyunun kendi klasöründeki `ReShade.ini`'ye** yazılıyor — yani her oyun otomatik olarak kendi ayarına sahip.

Bizde `vlss5_config.ini` **exe'nin yanında, tek dosya** (`ConfigManager::GetIniPath()`). ETS2 için ayarladığın `boostFactor` CS 1.6'ya da uygulanıyor. `WindowEnumerator` zaten hedef process adını biliyor; `[VLSS5:ets2.exe]` gibi section'lara geçmek küçük bir iş, büyük bir kullanılabilirlik kazancı.

---

### 3.7 🔥 HDR / renk uzayı yığını — bizde hiç yok
**Dosyalar:** `renodx-main/src/utils/swapchain.hpp:130-260`, `src/mods/swapchain_v2.hpp`, `src/shaders/color/` (19 dosya), `src/shaders/tonemap/` (12 tone mapper)

RenoDX'in tamamı bunun etrafında kurulu:
- `GetDirectXOutputDesc1()` → monitörün gerçek peak nits'i (`:130-181`)
- `ComputeReferenceWhite()` → BT.2100 HLG OOTF ile referans beyaz hesabı (`:206-229`)
- `GetHDRSupported()` → `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2` ile OS'tan HDR yeteneği (`:238-260`)
- Swap chain'i `R16G16B16A16_FLOAT` + `extended_srgb_linear`'a yükseltme
- 12 farklı tone mapper (ACES, RenoDRT, DICE, Frostbite, Reinhard, Daniele, PsychoV...)
- 19 renk uzayı dönüşüm kütüphanesi (PQ, ICtCp, OKLab, ACEScc/cct, DTUCS, Macleod-Boynton...)

**Bizim durumumuz:** `grep -rn "ColorSpace\|R16G16B16A16\|HDR" src/` → **tek bir isabet**, o da `NVSDK_NGX_DLSS_Feature_Flags_IsHDR` sabitinin tanımı. Swap chain formatımız `B8G8R8A8_UNORM` (`TECHNICAL.md` §7), yani 8-bit SDR.

**Bunun gerçek sonucu:** HDR monitörde HDR oyun oynayan bir kullanıcı VLSS5'i açtığında overlay'imiz **oyunun HDR'ını yok ediyor**. WGC zaten SDR yakalıyor, biz de SDR basıyoruz. Bu bir eksiklik değil, bir **regresyon** — VLSS5 açıkken görüntü kalitesi kapalı olduğundan kötü oluyor.

> Bu muhtemelen en önemli özellik boşluğumuz ve WGC'nin `DirectXPixelFormat::R16G16B16A16Float` desteği ile kısmen çözülebilir.

---

### 3.8 ⚙️ Exclusive fullscreen yönetimi
**Dosya:** `renodx-main/src/mods/swapchain_v2.hpp:121-128, 224-260`

```cpp
static bool prevent_full_screen = true;
static bool force_borderless = true;
static bool force_screen_tearing = true;
static bool prevent_multiple_flip_swapchains_per_window = true;
```
`SetFullscreenState` çağrılarını yakalayıp exclusive fullscreen'i engelliyor, pencereyi borderless'a zorluyor, tearing bayrağını ekliyor, aynı pencerede birden çok flip swapchain oluşmasını önlüyor.

Bizde bu kontrollerin **hiçbiri** yok. Hedef oyun exclusive fullscreen'e girdiğinde overlay'imiz ne olur? Muhtemelen arkada kalır veya minimize olur (`CheckFocusAndMinimize()` devreye girer). RenoDX'in `windowing.hpp:39-43`'teki `RestoreWindowIfMinimized` + `WM_ACTIVATEAPP` işleyicisi de dikkate değer bir savunma.

---

### 3.9 ⚙️ Canlı tanılama altyapısı: DevKit + MCP köprüsü
**Dosyalar:** `renodx-main/src/addons/devkit/addon.cpp` (9451 satır), `src/utils/mcp/server.hpp`, `src/utils/ipc/ipc.hpp`, `src/apps/mcp_bridge`, `docs/DEVKIT_MCP.md`

Oyun içinde çalışan bir addon, named pipe üzerinden bir MCP sunucusu açıyor. Dışarıdan bir AI/araç şu komutları çağırabiliyor:

```
devkit_queue_snapshot      → bir kareyi yakala
devkit_list_draws          → o karedeki tüm draw call'lar
devkit_get_draw            → tek draw'ın RT/SRV/CB/blend state'i
devkit_analyze_resource    → kaynağın format/boyut/içerik analizi
devkit_dump_resource_with_hash
devkit_set_live_shader_path / devkit_load_live_shaders
```

Bu, "oyun neden böyle görünüyor" sorusunu **oyunu kapatmadan** cevaplama makinesi.

Bizim tanılamamız `vlss5_logs.log` — tek yönlü, metin, oturum başına sıfırlanan (`TECHNICAL.md` §2). Watchdog thread'i (`>2000 ms takılma`) ve `[Perf]` özetleri iyi ama **sorgulanabilir** değil.

> **Gerçekçi ölçek:** 9451 satırlık bir devkit'e ihtiyacımız yok. Ama `App`'e küçük bir named pipe / localhost soket komut arayüzü koyup `GetStats`, `SetSetting <key> <val>`, `DumpFrame` çağrılabilir hale getirmek oturum başına saatler kazandırır — özellikle kalibrasyon hata ayıklamasında.

### 3.10 ⚙️ Kare/kaynak dökümü (EXR + PNG)
`renodx-main/src/utils/exr.hpp` (753 satır) ve `png.hpp` (240 satır) — herhangi bir GPU kaynağını diske HDR-doğru olarak yazabiliyorlar. cICP/ICC sinyalleme ile BT.2020 PQ PNG üretimi bile var (`.agents/skills/bt2020-png-generation`).

Bizde kare dökümü yok. "Bu kare neden böyle çıktı" sorusunu ancak ekran görüntüsüyle (yani yine SDR, yine sıkıştırılmış) tartışabiliyoruz. `gModelTex`, `gProxyTex`, `gOriginalTex`'i ayrı ayrı diske yazan bir hotkey, shader ayarlamalarında ölçüm yapmamızı sağlardı.

### 3.11 ⚙️ Kod organizasyonu ve inşa disiplini

| | VLSS5 | RenoDX |
|---|---|---|
| Katmanlama | `App.cpp` 1622 satır, her şey içinde | `utils/` (altyapı) → `mods/` (özellik) → `games/` (yapılandırma) |
| Derleme | MSBuild `.sln` | CMake + preset (clang/ninja/MSVC/VS), `CMakePresets.json` |
| CI | ❌ Yok | 9 GitHub Actions workflow (clang x64/x86, ninja, VS, nightly, snapshot) |
| Test | ❌ Yok | 9 test hedefi (`test/descriptor_cache_race`, `test/swapchain_proxy_barrier_states`, ...) |
| Lint/format | ❌ Yok | `.clang-format`, `.clang-tidy`, `.editorconfig`, `.clangd` |
| AI ajan rehberi | `TECHNICAL.md` (çok iyi) | `AGENTS.md` (kök + 4 iç içe) + `.agents/skills/` |

RenoDX'in test klasörü özellikle dikkat çekici: `descriptor_cache_race`, `swapchain_change_detection`, `swapchain_proxy_barrier_states` — yani **eşzamanlılık ve GPU state geçişlerini** test ediyorlar. `TECHNICAL.md` §11'de bizim "🟡 Thread'ler arası okuma — `m_lastFrameArrivalTime` atomik değil" notumuz var; RenoDX bu tarz şeyleri test hedefiyle sabitlemiş.

### 3.12 ⚙️ Cross-addon paylaşımlı bellek
`renodx-main/src/utils/cross_addon.hpp` — `platform::ProcessAllocator` ile aynı process içindeki birden çok addon arasında state paylaşımı. FPS limiter ve HDR modu aynı `swapchain::DeviceData`'yı kullanabiliyor. Bizim tek process'imiz olduğu için doğrudan ihtiyacımız yok, ama tekil `ConfigManager` singleton'ından daha yapısal bir desen.

---

## 4. ONLARDA OLAN, BİZDE OLMAYAN ÖZELLİKLER

| # | Özellik | RenoDX dosyası | Bizim durum | Önem |
|---|---|---|---|:---:|
| 1 | **HDR çıkış / scRGB / HDR10** | `utils/swapchain.hpp`, `mods/swapchain_v2.hpp` | Yok — sadece 8-bit SDR | 🔴 |
| 2 | **Dahili FPS limiter** (RTSS'siz) | `utils/swapchain.hpp:710` | RTSS'e bağımlı, doğrulanmamış | 🔴 |
| 3 | **Tone mapping** (12 operatör) | `shaders/tonemap/` | Yok | 🔴 |
| 4 | **Renk uzayı dönüşüm kütüphanesi** (19) | `shaders/color/` | Yok | 🔴 |
| 5 | **Canlı shader hot-reload** | `utils/shader_compiler_watcher.hpp` | Shader kodda gömülü | 🟠 |
| 6 | **Preset kaydetme yuvaları** (3 slot) | `utils/settings.hpp:292` | Yok | 🟠 |
| 7 | **Oyun başına ayar** | ReShade config (otomatik) | Tek global INI | 🟠 |
| 8 | **Settings Mode** (Basit/Orta/Gelişmiş) | `templates/settings.hpp:52` | Yok, hepsi açık | 🟠 |
| 9 | **Deklaratif ayar tanımı** | `utils/settings.hpp:56` | Elle 3 yerde | 🟠 |
| 10 | **Ayar tooltip'leri** | `Setting::tooltip` | Yok | 🟠 |
| 11 | **Koşullu ayar görünürlüğü/aktifliği** | `is_visible` / `is_enabled` | Yok | 🟡 |
| 12 | **Exclusive fullscreen engelleme** | `mods/swapchain_v2.hpp:121` | Yok | 🟡 |
| 13 | **Borderless'a zorlama** | `force_borderless` | Yok | 🟡 |
| 14 | **WndProc hook altyapısı** | `utils/windowing.hpp:78` | Sadece kendi pencerelerimiz | 🟡 |
| 15 | **Minimize'dan otomatik geri getirme** | `windowing.hpp:39` | Tersi var (auto-hide) | 🟡 |
| 16 | **Canlı frame/draw inceleme** | `addons/devkit/addon.cpp` | Yok | 🟡 |
| 17 | **MCP / IPC komut arayüzü** | `utils/mcp/`, `utils/ipc/` | Yok | 🟡 |
| 18 | **EXR / PNG kaynak dökümü** | `utils/exr.hpp`, `png.hpp` | Yok | 🟡 |
| 19 | **Kaynak (texture) format yükseltme** | `utils/resource_upgrade.hpp` (4118 satır) | Yok | 🟡 |
| 20 | **Texture değiştirme** | `utils/resource_replace.hpp` | Yok | ⚪ |
| 21 | **Shader dump + decompile** | `utils/shader_dump.hpp`, `decompiler/` | Yok | ⚪ |
| 22 | **Vulkan / OpenGL desteği** | `shader_compiler_vulkan.hpp`, `.gl.glsl` | Yalnız D3D11/12 | ⚪ |
| 23 | **32-bit (x86) hedef** | `clang-x86` preset | Yalnız x64 | ⚪ |
| 24 | **Device proxy** (ayrı D3D cihazı) | `utils/device_proxy.hpp` (1845 satır) | Yok (D3D12Interop benzer rol) | ⚪ |
| 25 | **CI / otomatik test** | 9 workflow, 9 test hedefi | Yok | 🟠 |
| 26 | **Sürüm manifesti / dağıtım** | `scripts/generate-release-manifest.mjs` | Elle | ⚪ |
| 27 | **Web indirme sayfası** | `src/web/` | Yok | ⚪ |
| 28 | **Streamline / DLSS-G hook** | `utils/dlss/streamline_v2.hpp` | Yok | ⚪ |
| 29 | **DLSS DLL sürüm değiştirme** | `addons/dlssfix/addon.cpp` | Yok | ⚪ |
| 30 | **Kod formatı / lint zorlaması** | `.clang-format`, `.clang-tidy` | Yok | 🟡 |

🔴 kritik · 🟠 yüksek · 🟡 orta · ⚪ bizim kapsamımız dışı

---

## 5. BİZDE OLAN, ONLARDA OLMAYAN

| Özellik | Bizim dosya | Not |
|---|---|---|
| **Windows Graphics Capture yakalama** | `src/CaptureManager.cpp` | RenoDX'in hiç ihtiyacı yok (içeriden) — ama bizi evrensel yapan şey |
| **DLSS 5 Neural Rendering (Feature 18)** | `src/DLSSNRManager.cpp` | RenoDX sadece mevcut DLSS'i hook'luyor, eklemiyor |
| **D3D11↔D3D12 paylaşımlı doku + fence köprüsü** | `src/D3D12Interop.cpp` | RenoDX'te eşdeğeri yok |
| **NVIDIA Optical Flow (OFA) sarmalayıcı** | `src/NvOFManager.cpp` | Yok |
| **Motion vector üretimi + reaktif UI maskesi** | `src/MotionVectorManager.cpp` | Yok — oyunun MV'sini kullanıyorlar |
| **Luminance-Ratio Transfer shader'ı** | `src/Renderer.cpp:77-116` | Yok |
| **Otomatik FPS kalibrasyonu (ikili arama)** | `src/App.cpp:~790` | Yok — limiti kullanıcı elle giriyor |
| **Yeniden atanabilir hotkey sistemi** | `src/HotkeysWindow.cpp` | Yok — sadece ReShade overlay tuşu |
| **Hook'suz tuş yoklaması** | `src/InputForwarder.h` | Yok |
| **Bağımsız ana menü + pencere seçici** | `src/Main.cpp`, `WindowEnumerator` | Yok |
| **Tıklama-geçirgen overlay penceresi** | `src/App.cpp:215` | Yok |
| **Bölünmüş ekran karşılaştırma modu** | `g_splitEnabled` shader'da | Yok |
| **RTSS profil entegrasyonu** | `src/RTSSManager.cpp` | Yok |
| **Watchdog thread'i** | `src/App.cpp` | Yok |
| **"Tam Ekran Yap" (monitöre gerdirme)** | `App::ComputeOverlayRect` | Farklı problem çözüyorlar |
| **GPU adaptör seçimi** | `Dlss5Config::selectedGpu` | Yok |

---

## 6. ÖNCELİKLİ ÇALMA LİSTESİ

Etki / maliyet oranına göre sıralı:

### Aşama 1 — Hemen (düşük maliyet, yüksek etki)

1. **Dahili FPS limiter** (§3.1)
   `renodx-main/src/utils/swapchain.hpp:710-805`'teki adaptif sleep+spin algoritmasını `Renderer::WaitForPresentReady()` yanına port et. RTSS bağımlılığını çıkış tarafından tamamen kaldırır. **~120 satır.**

2. **Settings Mode kademesi** (§3.3)
   `Dlss5Config`'e bir `settingsMode` int ekle, `SettingsWindow`'da her kontrole bir görünürlük eşiği ver. **~40 satır.**

3. **Ayar tooltip'leri** (§3.2)
   Her slider'a hover açıklaması. GDI'da `TTM_ADDTOOL` veya elle çizilen kutu. **~60 satır.**

4. **Shader hot-reload (minimal)** (§3.4)
   Pixel shader'ı `.hlsl` dosyasına çıkar, bir hotkey ile `D3DCompileFromFile`, hata varsa eskiyi koru. Kendi shader iterasyonumuzu 10× hızlandırır. **~80 satır.**

### Aşama 2 — Orta vade

5. **Deklaratif ayar sistemi** (§3.2)
   `Setting` struct'ının veri modelini al, `ConfigManager` + `SettingsWindow`'u onun üzerine kur. `SettingsWindow.cpp`'nin büyük kısmını siler, sonraki her ayar tek satır olur.

6. **Preset yuvaları + oyun başına ayar** (§3.5, §3.6)
   INI'de `[VLSS5:preset1]`, `[VLSS5:ets2.exe]` gibi section'lar. (5) yapıldıktan sonra neredeyse bedava gelir.

7. **Kare dökümü hotkey'i** (§3.10)
   `gModelTex` / `gProxyTex` / `gOriginalTex`'i PNG olarak diske yaz. Shader ayarlamalarında ölçüm yapabilmek için.

8. **Exclusive fullscreen davranışı** (§3.8)
   Hedef oyun exclusive fullscreen'e girdiğinde ne olduğunu önce **ölç**, sonra RenoDX'in `prevent_full_screen` / `RestoreWindowIfMinimized` desenlerinden uygun olanı al.

### Aşama 3 — Büyük iş

9. **HDR boru hattı** (§3.7)
   `DirectXPixelFormat::R16G16B16A16Float` ile WGC yakalama → `R16G16B16A16_FLOAT` swap chain → `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`. `GetDirectXOutputDesc1` / `ComputeReferenceWhite` kodları (`swapchain.hpp:130-229`) doğrudan alınabilir. Tone mapping için `shaders/tonemap/` kütüphanesi hazır (MIT lisanslı).
   **En büyük iş, en büyük kazanç.** HDR kullanıcısı için şu an bir regresyon üretiyoruz.

10. **IPC/komut arayüzü** (§3.9)
    Named pipe üzerinden `GetStats` / `SetSetting` / `DumpFrame`. Tanılama ve otomasyon için.

11. **CI + statik analiz** (§3.11)
    `.clang-format` + `.clang-tidy` + bir GitHub Actions workflow (`msbuild /warnaserror`). `TECHNICAL.md`'deki "0 uyarı / 0 hata" hedefini zorlar hale getirir.

### Almayacaklarımız (bilinçli)

- **ReShade bağımlılığı** — bağımsızlığımız en büyük farkımız.
- **Oyun başına addon modeli** — 244 klasörlük bakım yükü bizim değer önerimize ters.
- **Shader dump / decompile / texture replace** — oyunun içine erişimimiz yok, mantıklı değil.
- **Detours / IAT hook** (`dlss_hook.hpp`) — anti-cheat riski; WGC ile çalışmanın tüm anlamını yok eder.
- **Vulkan / OpenGL** — WGC zaten API'den bağımsız yakalıyor, bizim için konu dışı.

---

## 7. Lisans Notu

RenoDX **MIT** lisanslı (`renodx-main/LICENSE`). Kod alıntılamak serbest, telif bildirimini korumak yeterli. Alınan her blok için üst satıra kaynak yorumu düşülmeli:

```cpp
// Uyarlanmıştır: RenoDX (MIT) — clshortfuse
// https://github.com/clshortfuse/renodx  src/utils/swapchain.hpp
```

Not: `external/` altındaki submodule'ler (Streamline, DLSS SDK, Detours, OpenEXR) **ayrı lisanslara** tabi — özellikle NVIDIA Streamline ve DLSS SDK'nın kendi şartları var. Sadece `renodx-main/src/` altındaki kod MIT.
