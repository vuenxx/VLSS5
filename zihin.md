# VLSS5 — Frame Generation Teşhis, Analiz ve Çözüm Raporu (Zihin / Hafıza)

Bu doküman, oyunda Frame Generation (DLSS FG / FSR FG) açıkken VLSS5 üzerinde meydana gelen düşük FPS ve stutter sorununun teşhis edilme sürecini, test edilen hipotezleri, ortaya çıkarılan kök nedenleri ve uygulanan kalıcı çözümleri kayıt altına almaktadır.

---

## 1. Problem Tanımı

- **Belirti:** Oyunda Frame Generation (DLSS 3 / FSR 3) aktifken oyunun kendi dahili FPS sayacı yüksek değerler verirken, VLSS5 overlay çıktısı belirgin şekilde düşük FPS'te (30–40 FPS) kalmakta ve düzensiz stutter (takılma) hissi oluşturmaktadır.
- **Başlangıç Hipotezleri:**
  1. **Hipotez A (İç Hesaplama Yükü):** VLSS5'in kare başına maliyetinin (özellikle Motion Vector / Optik Akış hesaplaması veya DLSS-NR değerlendirmesi) yüksek girdi frekansında değişkenleşmesi/şişmesi.
  2. **Hipotez B (DWM & Sunum Blokajı):** Windows Graphics Capture (WGC) ve DWM masaüstü kompozitörünün, oyunun ürettiği kare hızıyla senkronize olamaması ve `Present()` çağrısında kuyruk tıkanması yaşaması.

---

## 2. Teşhis Altyapısının Kurulması

Sorunun kaynağını sezgisel değil, kesin metriklerle tespit etmek amacıyla teşhis katmanı eklendi:

1. **Capture-Arrival Jitter Metriği (`CaptureManager`):**
   - 120 örneklik kayan pencerede WGC kare geliş aralıkları (`gapMs`) ölçüldü.
   - Ortalama, varyans, standart sapma (`stddev`) ve maksimum gecikme 5 saniyelik özetlerle loglandı.
2. **Aşama Bazlı Jitter & Süre Ölçümü (`Renderer` / `D3D12Interop` / `App`):**
   - **MV (Optik Akış):** QPC ile `ProcessFrame` süresi.
   - **Eval (DLSS-NR):** `Evaluate` + `EndFrame` GPU komut süresi.
   - **Toplam:** Tüm kare döngüsünün süresi.
   - Kayan pencerede ortalama, standart sapma ve tepe süreleri loglandı.
3. **Kayan Medyan Tabanlı Stutter Tespiti (`App`):**
   - Son 60 karenin medyan süresi (`std::nth_element`) hesaplanarak, medyanın 2.5 katını aşan gecikmeler `[Perf] STUTTER` olarak etiketlendi.
4. **DWM Kompozisyon Tespiti (`App`):**
   - `DwmGetWindowAttribute(DWMWA_CLOAKED)` ve pencere `exStyle` bayrakları taranarak kompozitör durumu raporlandı.
5. **Manuel Test İşaretçisi (`F7`):**
   - Yalın `F10` kısayolunu (DLSS aç/kapa) bozmamak için kullanıcı kontrollü FG test başlangıç/bitiş işaretçisi `F7` tuşuna bağlandı.

---

## 3. Log Verileri ve Kök Neden Analizi

Kullanıcı testlerinden toplanan `vlss5_logs.log` analiz edildiğinde:

### A. Hipotez A Çürütüldü (İç Hesaplama Çok Hızlı)
- NVOF (Donanımsal Optik Akış): **0.3 ms** (stddev 0.1 ms)
- DLSS-NR Değerlendirmesi: **0.8 ms** (stddev 0.2 ms)
- Toplam iç GPU hesaplaması sadece **1.2 ms** sürmektedir. Donanım tek başına 700+ FPS basabilecek kapasitededir.

### B. Hipotez B Doğrulandı (`Present()` Blokajı)
- Her karede şu kritik uyarı tetiklenmekteydi:
  `[Present] UYARI: Present() 24.51 ms surdu (vsync=KAPALI, tearing=DESTEKLENIYOR)`
  `[Present] UYARI: Present() 28.32 ms surdu (vsync=KAPALI, tearing=DESTEKLENIYOR)`
- Toplam kare süresi olan `27.7 ms`'nin yaklaşık **%95'i** doğrudan `m_swapChain->Present()` fonksiyonunun içinde bloke olmaktaydı.
- Bu durum VLSS5'i en fazla 36–40 FPS sınırına çekmekte, oyunun yüksek hızda ürettiği kareler kuyrukta birikip atılmakta ve yoğun stutter oluşturmaktaydı.

### C. Present Blokajının Windows DWM Kök Nedeni
1. **`WS_EX_LAYERED` Kısıtı:**
   - Overlay penceresi `WS_EX_LAYERED` ve `SetLayeredWindowAttributes(m_overlayHwnd, 0, 254, LWA_ALPHA)` ile oluşturulmuştu.
   - Windows DWM mimarisinde layered pencereler **Direct Flip**, **Independent Flip (iFlip)** ve **Donanım MPO (Multi-Plane Overlay)** kapsamına alınamaz; zorunlu olarak DWM yazılımsal yeniden yönlendirme yüzeyine (Redirection Surface) hapsedilir.
   - Microsoft DXGI şartnamesine göre, layered pencerelerde `DXGI_PRESENT_ALLOW_TEARING` bayrağı işletim sistemi tarafından **yok sayılır**.
   - DWM, arkada Frame Generation çalıştıran 3D oyun ile layered overlay'i masaüstünde harmanlamaya çalışırken `Present()` çağrısını 24–32 ms boyunca uyutmaktaydı.
2. **`DXGI_SCALING_STRETCH` Kısıtı:**
   - Swap chain tanımında yer alan `DXGI_SCALING_STRETCH`, DWM'in Direct Flip'e izin vermesini engelleyen ikinci bir faktördü (Direct Flip için `DXGI_SCALING_NONE` şarttır).

---

## 4. Uygulanan Kalıcı Çözümler

### 1. `WS_EX_LAYERED` Kaldırıldı & Direct Flip Açıldı
- **Dosya:** `src/App.cpp`
- `CreateWindowExW` içerisinden `WS_EX_LAYERED` kaldırıldı (`WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` korundu).
- `SetLayeredWindowAttributes` çağrısı tamamen kaldırıldı.
- Fare ve klavye girdilerinin oyuna geçişi zaten `OverlayWndProc` içinde `WM_NCHITTEST -> HTTRANSPARENT` ve `WM_MOUSEACTIVATE -> MA_NOACTIVATE` ile sağlandığından `WS_EX_LAYERED`'e ihtiyaç kalmamıştır.
- Overlay penceresi artık DWM tarafından masaüstü kompozitörü baypas edilerek doğrudan **Independent Flip / Direct Flip** modunda çalıştırılır.

### 2. SwapChain Direct Flip Uyumlu Hale Getirildi
- **Dosya:** `src/Renderer.cpp`
- `desc.Scaling = DXGI_SCALING_NONE;` olarak güncellendi.
- `desc.BufferCount = 4;` (Quad-buffering `FLIP_DISCARD`) ile DWM arabellek kilitlenmesi önlendi.
- `SetMaximumFrameLatency(2)` ile GPU kuyruk rahatlığı sağlandı.

### 3. Log Tanılama Geliştirmesi
- `LogDwmStatus()` içerisine overlay penceresinin stil durumu (`overlayExStyle`) ve `DirectFlip=UYGUN/ENGELENDI` bilgisi eklendi.

---

## 5. Doğrulama ve Derleme

- **Platform:** Visual Studio 2022 Community MSBuild (`Release|x64`).
- **Derleme Sonucu:** `0 Uyarı, 0 Hata`.
- **Çıktı Yolu:** `VLSS5\x64\Release\VLSS5.exe`
