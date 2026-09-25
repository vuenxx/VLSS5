#include "MouseMapper.h"
#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// OCR_* sabitleri yalnizca <windows.h>'tan ONCE OEMRESOURCE tanimlanirsa gelir.
// Common.h tum projede ortak oldugundan orayi kirletmek yerine degerleri burada
// tanimliyoruz (Windows SDK winuser.h ile birebir ayni).
// ---------------------------------------------------------------------------
namespace
{
    constexpr DWORD kOcrNormal      = 32512;
    constexpr DWORD kOcrIBeam       = 32513;
    constexpr DWORD kOcrWait        = 32514;
    constexpr DWORD kOcrCross       = 32515;
    constexpr DWORD kOcrUp          = 32516;
    constexpr DWORD kOcrSizeNWSE    = 32642;
    constexpr DWORD kOcrSizeNESW    = 32643;
    constexpr DWORD kOcrSizeWE      = 32644;
    constexpr DWORD kOcrSizeNS      = 32645;
    constexpr DWORD kOcrSizeAll     = 32646;
    constexpr DWORD kOcrNo          = 32648;
    constexpr DWORD kOcrHand        = 32649;
    constexpr DWORD kOcrAppStarting = 32650;
    constexpr DWORD kOcrHelp        = 32651;

    struct SysCursorEntry { DWORD ocr; LPCWSTR idc; };

    const SysCursorEntry kSysCursors[] = {
        { kOcrNormal,      IDC_ARROW       },
        { kOcrIBeam,       IDC_IBEAM       },
        { kOcrWait,        IDC_WAIT        },
        { kOcrCross,       IDC_CROSS       },
        { kOcrUp,          IDC_UPARROW     },
        { kOcrSizeNWSE,    IDC_SIZENWSE    },
        { kOcrSizeNESW,    IDC_SIZENESW    },
        { kOcrSizeWE,      IDC_SIZEWE      },
        { kOcrSizeNS,      IDC_SIZENS      },
        { kOcrSizeAll,     IDC_SIZEALL     },
        { kOcrNo,          IDC_NO          },
        { kOcrHand,        IDC_HAND        },
        { kOcrAppStarting, IDC_APPSTARTING },
        { kOcrHelp,        IDC_HELP        },
    };

    // Tamamen saydam 32x32 imlec. AND maskesi 1, XOR maskesi 0 => "ekrani oldugu
    // gibi birak": hicbir piksel cizilmez.
    HCURSOR CreateBlankCursor()
    {
        BYTE andMask[32 * 4];
        BYTE xorMask[32 * 4];
        memset(andMask, 0xFF, sizeof(andMask));
        memset(xorMask, 0x00, sizeof(xorMask));
        return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, andMask, xorMask);
    }

    // Surec normal yoldan kapanirsa (StopOverlay cagrilmadan) sistem imleclerini
    // bosaltilmis birakmamak icin son emniyet. Cokme durumunda calismaz; asil
    // guvence App::StopOverlay -> MouseMapper::Stop yolundadir.
    bool s_atexitRegistered = false;
    std::atomic<bool> s_cursorsBlankedGlobal{ false };

    void RestoreCursorsAtExit()
    {
        if (s_cursorsBlankedGlobal.load())
            SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    }
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------
bool MouseMapper::Start(HWND overlayHwnd, HWND targetHwnd, bool stretch,
                        bool mapping, bool lock, bool hideSystem)
{
    Stop();

    if (!overlayHwnd || !targetHwnd) return false;

    m_overlay = overlayHwnd;
    m_target  = targetHwnd;
    // Gerdirme kapaliyken koordinat sapmasi YOKTUR; modulun tamami devre disi.
    m_stretch = stretch;
    m_mapping = mapping && stretch;
    m_lock    = lock    && stretch;
    m_hide    = hideSystem && m_mapping;   // esleme yoksa gizlemenin anlami yok
    m_started = true;

    m_cursorPixels.assign(static_cast<size_t>(kCursorTexSize) * kCursorTexSize, 0u);

    // Ham girdi ham "mickey" birimidir; Windows isaretci hizini (1..20, 10 = normal)
    // uygulayarak imlecin masaustundekiyle benzer hizda hareket etmesini sagliyoruz.
    // Isaretci ivmesi (Enhance pointer precision) kasitli olarak uygulanmiyor:
    // nisan alinan bir goruntude 1:1 hareket daha ongorulebilir.
    int speed = 10;
    if (SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0) && speed > 0)
        m_speedScale = speed / 10.0;
    else
        m_speedScale = 1.0;

    // Ham girdi kaydi burada DEGIL, esleme fiilen etkinlesince yapilir
    // (EnsureRawInput). Klasik pencere modunda hic acilmaz.

    if (!s_atexitRegistered)
    {
        atexit(RestoreCursorsAtExit);
        s_atexitRegistered = true;
    }

    DLSS_Log("[Mouse] Baslatildi: gerdirme=%d esleme=%d kilit=%d imlec-gizle=%d hiz=%.2f",
             m_stretch ? 1 : 0, m_mapping ? 1 : 0, m_lock ? 1 : 0, m_hide ? 1 : 0, m_speedScale);
    return true;
}

void MouseMapper::Stop()
{
    if (!m_started)
    {
        // Yine de sarkta kalmis bir kilit/bosaltma varsa temizle.
        if (m_clipApplied) { ClipCursor(nullptr); m_clipApplied = false; }
        if (m_cursorsBlanked) RestoreSystemCursors();
        return;
    }

    EnsureRawInput(false);

    if (m_clipApplied) { ClipCursor(nullptr); m_clipApplied = false; }
    if (m_cursorsBlanked) RestoreSystemCursors();

    m_started       = false;
    m_mappingActive = false;
    m_haveVirtual   = false;
    m_haveImage     = false;
    m_imageDirty    = false;
    m_lastCursor    = nullptr;
    m_overlay       = nullptr;
    m_target        = nullptr;
    m_suspendUntil  = 0;
    m_backoffMs     = 3000;
    m_foreignMoves  = 0;

    DLSS_Log("[Mouse] Durduruldu; imlec kilidi ve sistem imlecleri geri yuklendi.");
}

// ---------------------------------------------------------------------------
// EnsureRawInput
// ---------------------------------------------------------------------------
bool MouseMapper::EnsureRawInput(bool on)
{
    if (on == m_rawRegistered) return m_rawRegistered;

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;   // Generic Desktop
    rid.usUsage     = 0x02;   // Mouse

    if (on)
    {
        if (!m_overlay) return false;
        // INPUTSINK: overlay odakta OLMADAN da ham girdi gelir. Bu bir hook
        // degildir; oyunun kendi girdisini ne tuketir ne geciktirir.
        rid.dwFlags    = RIDEV_INPUTSINK;
        rid.hwndTarget = m_overlay;

        m_rawRegistered = (RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE);
        if (!m_rawRegistered)
            DLSS_Log("[Mouse] Ham girdi kaydi basarisiz (hata=%lu); esleme yapilamiyor.",
                     GetLastError());
    }
    else
    {
        rid.dwFlags    = RIDEV_REMOVE;
        rid.hwndTarget = nullptr;   // REMOVE icin NULL olmak ZORUNDA
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        m_rawRegistered = false;
    }

    return m_rawRegistered;
}

// ---------------------------------------------------------------------------
// Tick — her kare
// ---------------------------------------------------------------------------
void MouseMapper::Tick(const RECT& overlayRect, const RECT& targetRect, bool enabled)
{
    if (!m_started) return;

    m_overlayRect = overlayRect;
    m_targetRect  = targetRect;
    m_enabled     = enabled;

    const bool geometryOk =
        (overlayRect.right > overlayRect.left) && (overlayRect.bottom > overlayRect.top) &&
        (targetRect.right  > targetRect.left)  && (targetRect.bottom  > targetRect.top);

    const ULONGLONG now = GetTickCount64();

    // "Oyun modundayiz": ham girdi guvenlik agi yuzunden esleme GECICI olarak
    // askiya alinsa bile DEGISMEYEN, kararli durum. Kilit (ClipCursor) ve sistem
    // imlecini gizleme SADECE buna baglidir -- m_mappingActive'e (asagida) DEGIL.
    //
    // ONEMLI GECMIS: Eskiden kilit/gizleme m_mappingActive'e baglanmisti. Guvenlik
    // agi ("oyun imleci kendisi konumluyor") beklenenden COK sik yanlis pozitif
    // veriyor (RIDEV_NOLEGACY kullanilmadigindan Windows'un kendi native imlec
    // ballistigi raw girdiyle PARALEL calisiyor ve GetCursorPos'u bizim yazdigimiz
    // yerden sik sik saptiriyor); her tetiklendiginde:
    //   (a) kilit ANLIK olarak birakiliyordu -> tam hizli bir donusun ortasinda
    //       gercek imlec kirpma disina cikip 2. monitore kacabiliyor, oradaki bir
    //       tiklama da oyunu arka plana atiyordu.
    //   (b) sistem imlecleri geri yukleniyordu -> gercek imlec TEKRAR GORUNUR
    //       oluyordu ama hala kucuk hedef dikdortgenine kirpiliydi: kullanici
    //       gerilmis ekranda kucuk bir kutuya hapsolmus, yanlis yerde bir imlec
    //       goruyordu.
    // Simdi kilit ve gizleme oyun modu boyunca KESINTISIZ acik kaliyor; yalnizca
    // bizim SetCursorPos ile takip/cizim yapmamiz (m_mappingActive) askiya
    // aliniyor. Boylece askiya alinma sirasinda gercek imlec ne kacabilir ne de
    // yanlis yerde gorunur olur -- sadece "donmus" ve gizli kalir.
    const bool sessionActive = enabled && geometryOk;

    bool want = m_mapping && sessionActive;
    if (want && now < m_suspendUntil) want = false;

    if (want && !EnsureRawInput(true))
        want = false;       // ham girdi yoksa esleme de yok
    if (!want && !m_mapping)
        EnsureRawInput(false);

    // ---- Gizleme: oyun modu boyunca surekli acik/kapali; askiya almadan bagimsiz.
    const bool wantHide = m_hide && sessionActive;
    if (wantHide && !m_cursorsBlanked)
    {
        BlankSystemCursors();
        m_lastCursor = nullptr;
    }
    else if (!wantHide && m_cursorsBlanked)
    {
        RestoreSystemCursors();
    }

    // ---- Kilit: oyun modu boyunca surekli acik/kapali; askiya almadan bagimsiz.
    // m_lock zaten Start'ta gerdirmeyle ANDlenmistir; klasik modda buraya girilmez.
    if (m_lock && sessionActive)
        UpdateClip();
    else if (m_clipApplied)
    {
        ClipCursor(nullptr);
        m_clipApplied = false;
    }

    if (want && !m_mappingActive)
    {
        m_foreignMoves = 0;
        m_healthySince = now;
        m_mappingActive = true;
        ResyncFromPhysical();
        if (m_hide) RefreshCursorImage();

        DLSS_Log("[Mouse] Esleme ETKIN: overlay %ldx%ld @(%ld,%ld) <- hedef %ldx%ld @(%ld,%ld) "
                 "olcek %.3fx/%.3fx | imlec-bosaltildi=%d goruntu=%d",
                 overlayRect.right - overlayRect.left, overlayRect.bottom - overlayRect.top,
                 overlayRect.left, overlayRect.top,
                 targetRect.right - targetRect.left, targetRect.bottom - targetRect.top,
                 targetRect.left, targetRect.top,
                 (double)(targetRect.right - targetRect.left) / (overlayRect.right - overlayRect.left),
                 (double)(targetRect.bottom - targetRect.top) / (overlayRect.bottom - overlayRect.top),
                 m_cursorsBlanked ? 1 : 0, m_haveImage ? 1 : 0);
    }
    else if (!want && m_mappingActive)
    {
        m_mappingActive = false;
        m_haveImage     = false;
        EnsureRawInput(false);
        DLSS_Log("[Mouse] Esleme pasif (oyun modu disi ya da askida; kilit/gizleme kalici).");
    }

    // Esleme bir sure sorunsuz calistiysa geri cekilme suresini sifirla:
    // oyun menuden oyuna gecip geri dondugunde uzun bekleme miras kalmasin.
    if (m_mappingActive && m_backoffMs > 3000 && (now - m_healthySince) > 5000)
        m_backoffMs = 3000;

    if (m_mappingActive && m_hide)
        RefreshCursorImage();
}

// ---------------------------------------------------------------------------
// UpdateClip — imleci HEDEF pencereye kilitle
//
// Kilit her zaman hedef dikdortgenine uygulanir: tiklamalarin fiilen gittigi
// yer orasidir. Tam Ekran modunda gorsel imlec overlay icinde serbest gezmeye
// devam eder, cunku sanal konum zaten overlay dikdortgenine kirpilir.
// ---------------------------------------------------------------------------
void MouseMapper::UpdateClip()
{
    RECT desired = m_targetRect;
    if (desired.right <= desired.left || desired.bottom <= desired.top) return;

    // Oyunlarin cogu kendi ClipCursor'unu her kare yeniden uyguluyor. Bu yuzden
    // mevcut kirpma dikdortgenini okuyup yalnizca farkliysa yaziyoruz; her kare
    // kosulsuz ClipCursor cagirmak gereksiz cekisme yaratirdi.
    static ULONGLONG s_lastCheck = 0;
    const ULONGLONG now = GetTickCount64();
    const bool rectChanged =
        (desired.left != m_appliedClip.left || desired.top != m_appliedClip.top ||
         desired.right != m_appliedClip.right || desired.bottom != m_appliedClip.bottom);

    if (!rectChanged && (now - s_lastCheck) < 250) return;
    s_lastCheck = now;

    RECT current = {};
    if (!rectChanged && GetClipCursor(&current) &&
        current.left == desired.left && current.top == desired.top &&
        current.right == desired.right && current.bottom == desired.bottom)
    {
        return;
    }

    if (ClipCursor(&desired))
    {
        m_appliedClip = desired;
        m_clipApplied = true;
    }
}

// ---------------------------------------------------------------------------
// OnRawInput
// ---------------------------------------------------------------------------
void MouseMapper::OnRawInput(LPARAM lParam)
{
    if (!m_started || !m_mappingActive) return;

    BYTE  buffer[sizeof(RAWINPUT) + 64];
    UINT  size = sizeof(buffer);

    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                        buffer, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;

    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buffer);
    if (ri->header.dwType != RIM_TYPEMOUSE) return;

    const RAWMOUSE& mouse = ri->data.mouse;

    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
    {
        // Tablet / uzak masaustu: cihaz zaten mutlak bir nokta bildiriyor.
        // Kullanici nereyi isaret ediyorsa GORSEL konum odur.
        const bool virt = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int  w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int  h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        const int  l = virt ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        const int  t = virt ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;

        m_vx = l + (mouse.lLastX / 65535.0) * w;
        m_vy = t + (mouse.lLastY / 65535.0) * h;
        m_haveVirtual = true;
        ApplyMapping();
        return;
    }

    if (mouse.lLastX == 0 && mouse.lLastY == 0) return;
    if (!m_haveVirtual) ResyncFromPhysical();

    // --- Guvenlik agi: imleci oyun mu konumluyor? ---
    // Son yazdigimiz yerde bulamiyorsak baska biri (tipik olarak FPS oyununun
    // her kare yaptigi "merkeze cek") imleci tasiyor demektir. Ust uste yeterince
    // tekrarlarsa eslemeyi askiya aliyoruz; yoksa iki taraf birbiriyle cekisir.
    //
    // Tolerans SABIT DEGIL, hedef pencere boyutuyla orantili. Neden: biz
    // RIDEV_NOLEGACY kullanmiyoruz, yani Windows'un kendi native imlec
    // ballistigi ham girdiyle PARALEL calisiyor ve gercek imleci her olayda
    // TAM (olceklenmemis) ekran-pikseli kadar kaydirmaya calisiyor; biz ise
    // ayni olay icin hedef/overlay oranina gore KUCULTULMUS bir konum
    // komutluyoruz. Kucuk/dusuk cozunurluklu hedef pencerelerde bu iki hareket
    // arasindaki fark, olceklenmemis ballistigin neredeyse tamamina yakin
    // oluyor ve sabit 8px esigi HER ham olayda asiliyordu -> esleme surekli
    // askiya aliniyor, imlec dusuk cozunurluklu pencerelerde hic gorunmuyordu.
    // ClipCursor artik askiya alinsa da acik kaldigindan (Tick'e bak) gercek
    // imlec zaten hedef dikdortgenin disina cikamiyor; bu yuzden toleransi
    // pencere boyutuyla oranti kadar genis tutmak guvenli.
    const double tw = static_cast<double>(m_targetRect.right  - m_targetRect.left);
    const double th = static_cast<double>(m_targetRect.bottom - m_targetRect.top);
    const double tolX = (tw * 0.20 > 8.0) ? tw * 0.20 : 8.0;
    const double tolY = (th * 0.20 > 8.0) ? th * 0.20 : 8.0;

    POINT phys = {};
    if (GetCursorPos(&phys))
    {
        if (labs(phys.x - m_lastSet.x) > tolX || labs(phys.y - m_lastSet.y) > tolY)
        {
            if (++m_foreignMoves > 12)
            {
                Suspend("oyun imleci kendisi konumluyor");
                return;
            }
        }
        else
        {
            m_foreignMoves = 0;
        }
    }

    m_vx += mouse.lLastX * m_speedScale;
    m_vy += mouse.lLastY * m_speedScale;
    ApplyMapping();
}

// ---------------------------------------------------------------------------
// ApplyMapping — gorsel konum -> gercek imlec konumu
// ---------------------------------------------------------------------------
void MouseMapper::ApplyMapping()
{
    const double ow = static_cast<double>(m_overlayRect.right  - m_overlayRect.left);
    const double oh = static_cast<double>(m_overlayRect.bottom - m_overlayRect.top);
    const double tw = static_cast<double>(m_targetRect.right   - m_targetRect.left);
    const double th = static_cast<double>(m_targetRect.bottom  - m_targetRect.top);
    if (ow <= 0.0 || oh <= 0.0 || tw <= 0.0 || th <= 0.0) return;

    // Gorsel konum her zaman overlay dikdortgeninde kalir.
    if (m_vx < m_overlayRect.left)          m_vx = m_overlayRect.left;
    if (m_vx > m_overlayRect.right  - 1.0)  m_vx = m_overlayRect.right  - 1.0;
    if (m_vy < m_overlayRect.top)           m_vy = m_overlayRect.top;
    if (m_vy > m_overlayRect.bottom - 1.0)  m_vy = m_overlayRect.bottom - 1.0;

    long px = static_cast<long>(std::lround(m_targetRect.left + (m_vx - m_overlayRect.left) * (tw / ow)));
    long py = static_cast<long>(std::lround(m_targetRect.top  + (m_vy - m_overlayRect.top)  * (th / oh)));

    if (px < m_targetRect.left)       px = m_targetRect.left;
    if (px > m_targetRect.right  - 1) px = m_targetRect.right  - 1;
    if (py < m_targetRect.top)        py = m_targetRect.top;
    if (py > m_targetRect.bottom - 1) py = m_targetRect.bottom - 1;

    SetCursorPos(static_cast<int>(px), static_cast<int>(py));
    m_lastSet.x = px;
    m_lastSet.y = py;
}

// ---------------------------------------------------------------------------
// ResyncFromPhysical — gercek imlecten gorsel konumu turet
// ---------------------------------------------------------------------------
void MouseMapper::ResyncFromPhysical()
{
    POINT p = {};
    if (!GetCursorPos(&p))
    {
        m_vx = (m_overlayRect.left + m_overlayRect.right)  * 0.5;
        m_vy = (m_overlayRect.top  + m_overlayRect.bottom) * 0.5;
    }
    else
    {
        const double tw = static_cast<double>(m_targetRect.right  - m_targetRect.left);
        const double th = static_cast<double>(m_targetRect.bottom - m_targetRect.top);
        const double ow = static_cast<double>(m_overlayRect.right  - m_overlayRect.left);
        const double oh = static_cast<double>(m_overlayRect.bottom - m_overlayRect.top);

        if (tw > 0.0 && th > 0.0 && ow > 0.0 && oh > 0.0 &&
            p.x >= m_targetRect.left && p.x < m_targetRect.right &&
            p.y >= m_targetRect.top  && p.y < m_targetRect.bottom)
        {
            m_vx = m_overlayRect.left + (p.x - m_targetRect.left) * (ow / tw);
            m_vy = m_overlayRect.top  + (p.y - m_targetRect.top)  * (oh / th);
        }
        else
        {
            m_vx = (m_overlayRect.left + m_overlayRect.right)  * 0.5;
            m_vy = (m_overlayRect.top  + m_overlayRect.bottom) * 0.5;
        }
    }

    m_haveVirtual = true;
    ApplyMapping();
}

// ---------------------------------------------------------------------------
// Suspend
// ---------------------------------------------------------------------------
void MouseMapper::Suspend(const char* reason)
{
    m_suspendUntil  = GetTickCount64() + m_backoffMs;
    m_mappingActive = false;
    m_foreignMoves  = 0;
    m_haveImage     = false;
    m_haveVirtual   = false;
    EnsureRawInput(false);
    // Kilit ve sistem imleci gizleme BURADA GERI ALINMAZ: ikisi de artik
    // Tick()'teki sessionActive durumuna bagli ve askiya alma sirasinda da
    // acik kalmalari gerekiyor (bkz. Tick yorumu) -- aksi halde gercek imlec
    // hem gorunur hem de hala kucuk hedef dikdortgenine kirpiliyken kullaniciya
    // gosterilmis olurdu, ya da kilit gecici olarak kalkip imlec baska monitore
    // kacabilirdi.

    DLSS_Log("[Mouse] Esleme askiya alindi (%s); %d ms sonra tekrar denenecek.",
             reason, m_backoffMs);

    m_backoffMs *= 2;
    if (m_backoffMs > 60000) m_backoffMs = 60000;
}

// ---------------------------------------------------------------------------
// Sistem imleclerini bosaltma / geri yukleme
// ---------------------------------------------------------------------------
void MouseMapper::BlankSystemCursors()
{
    if (m_cursorsBlanked) return;

    m_systemCursors.clear();
    m_systemCursors.reserve(_countof(kSysCursors));

    // 1. Once orijinal goruntulerin KOPYASINI al. SetSystemCursor kendisine
    //    verilen handle'i yok ettigi icin sonradan alamayiz.
    for (const auto& e : kSysCursors)
    {
        SystemCursorSlot slot;
        slot.ocrId     = e.ocr;
        HCURSOR sys    = LoadCursorW(nullptr, e.idc);
        slot.sysHandle = sys;
        slot.original  = sys ? CopyCursor(sys) : nullptr;
        m_systemCursors.push_back(slot);
    }

    // 2. Hepsini saydam bir imlecle degistir.
    bool any = false;
    for (size_t i = 0; i < m_systemCursors.size(); ++i)
    {
        HCURSOR blank = CreateBlankCursor();
        if (!blank) continue;
        if (SetSystemCursor(blank, m_systemCursors[i].ocrId))
            any = true;
        else
            DestroyCursor(blank);   // basarisizsa sahiplik bizde kalir
    }

    // 3. Bosaltma SONRASI handle'lari oku. GetCursorInfo bunlari dondurdugunde
    //    hangi mantiksal seklin istendigini boylece biliyoruz (hepsi ayni saydam
    //    goruntuye sahip olsa da handle'lari farklidir).
    for (size_t i = 0; i < m_systemCursors.size(); ++i)
        m_systemCursors[i].blankHandle = LoadCursorW(nullptr, kSysCursors[i].idc);

    m_cursorsBlanked = any;
    s_cursorsBlankedGlobal.store(any);

    if (!any)
        DLSS_Log("[Mouse] Sistem imlecleri bosaltilamadi (hata=%lu).", GetLastError());
}

void MouseMapper::RestoreSystemCursors()
{
    if (m_cursorsBlanked)
    {
        // Kayittaki imlec semasini yeniden yukler: tek cagri, tum OCR_* icin.
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
        m_cursorsBlanked = false;
        s_cursorsBlankedGlobal.store(false);
    }

    for (auto& s : m_systemCursors)
        if (s.original) DestroyCursor(s.original);
    m_systemCursors.clear();

    m_lastCursor = nullptr;
    m_haveImage  = false;
}

// ---------------------------------------------------------------------------
// RefreshCursorImage — o an gecerli imlec seklini dokuya hazirla
// ---------------------------------------------------------------------------
void MouseMapper::RefreshCursorImage()
{
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci)) return;

    // Oyun imleci kendisi gizlediyse (FPS modu) biz de cizmiyoruz.
    if (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
    {
        m_haveImage  = false;
        m_lastCursor = nullptr;
        return;
    }

    if (ci.hCursor == m_lastCursor && m_haveImage) return;
    m_lastCursor = ci.hCursor;

    // Bosalttigimiz bir sistem imleci mi? Oyleyse ORIJINAL goruntusunu ciz.
    // Degilse oyunun kendi ozel imlecidir; oldugu gibi kullanilir.
    HCURSOR draw = ci.hCursor;
    for (const auto& s : m_systemCursors)
    {
        if (s.original && (s.blankHandle == ci.hCursor || s.sysHandle == ci.hCursor))
        {
            draw = s.original;
            break;
        }
    }

    int hx = 0, hy = 0;
    if (RenderCursorToPixels(draw, hx, hy))
    {
        m_hotX       = hx;
        m_hotY       = hy;
        m_haveImage  = true;
        m_imageDirty = true;
        DLSS_Log("[Mouse] Imlec goruntusu hazir: handle=%p (sistem=%d) sicak nokta=(%d,%d)",
                 (void*)ci.hCursor, (draw != ci.hCursor) ? 1 : 0, hx, hy);
    }
    else
    {
        m_haveImage = false;
        DLSS_Log("[Mouse] UYARI: imlec goruntusu cizilemedi (handle=%p).", (void*)ci.hCursor);
    }
}

// ---------------------------------------------------------------------------
// RenderCursorToPixels
//
// Imleci IKI kez ciziyoruz: bir kez siyah, bir kez beyaz zemine. GDI 32-bit DIB'e
// cizerken alfa kanalini guvenilir doldurmaz (eski AND/XOR maskeli imleclerde
// tamamen 0 kalir). Iki ornekten alfa cikarilabilir:
//     alfa = 255 - (beyazZemin - siyahZemin)
// ve siyah zemindeki sonuc zaten PREMULTIPLIED renktir. Shader de bu formu
// bekler: out = imlec.rgb + arka * (1 - imlec.a)
// ---------------------------------------------------------------------------
bool MouseMapper::RenderCursorToPixels(HCURSOR cur, int& hotX, int& hotY)
{
    if (!cur) return false;

    ICONINFO ii = {};
    if (!GetIconInfo(cur, &ii)) return false;

    int w = 0, h = 0;
    BITMAP bm = {};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm))
    {
        w = bm.bmWidth;
        h = bm.bmHeight;
    }
    else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm))
    {
        w = bm.bmWidth;
        h = bm.bmHeight / 2;    // monokrom imlecte AND ve XOR maskeleri alt alta
    }

    hotX = static_cast<int>(ii.xHotspot);
    hotY = static_cast<int>(ii.yHotspot);

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);

    if (w <= 0 || h <= 0) return false;
    if (w > kCursorTexSize) { hotX = hotX * kCursorTexSize / w; w = kCursorTexSize; }
    if (h > kCursorTexSize) { hotY = hotY * kCursorTexSize / h; h = kCursorTexSize; }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = kCursorTexSize;
    bi.bmiHeader.biHeight      = -kCursorTexSize;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;

    void*   bits = nullptr;
    HBITMAP dib  = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits)
    {
        if (dib) DeleteObject(dib);
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ oldBmp   = SelectObject(dc, dib);
    const size_t cnt = static_cast<size_t>(kCursorTexSize) * kCursorTexSize;

    std::vector<uint32_t> onBlack(cnt, 0u);

    // 1. gecis: siyah zemin
    memset(bits, 0x00, cnt * 4);
    DrawIconEx(dc, 0, 0, cur, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();
    memcpy(onBlack.data(), bits, cnt * 4);

    // 2. gecis: beyaz zemin
    memset(bits, 0xFF, cnt * 4);
    DrawIconEx(dc, 0, 0, cur, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();
    const uint32_t* onWhite = static_cast<const uint32_t*>(bits);

    for (size_t i = 0; i < cnt; ++i)
    {
        const uint32_t b = onBlack[i];
        const uint32_t wv = onWhite[i];

        const int gb = static_cast<int>((b  >> 8) & 0xFF);
        const int gw = static_cast<int>((wv >> 8) & 0xFF);

        int alpha = 255 - (gw - gb);
        if (alpha < 0)   alpha = 0;
        if (alpha > 255) alpha = 255;

        m_cursorPixels[i] = (b & 0x00FFFFFFu) | (static_cast<uint32_t>(alpha) << 24);
    }

    SelectObject(dc, oldBmp);
    DeleteObject(dib);
    DeleteDC(dc);
    return true;
}

// ---------------------------------------------------------------------------
// Overlay'in cizecegi imlec
// ---------------------------------------------------------------------------
bool MouseMapper::GetCursorDrawPos(float& outX, float& outY) const
{
    if (!m_mappingActive || !m_hide || !m_haveImage) return false;

    outX = static_cast<float>(m_vx - m_overlayRect.left - m_hotX);
    outY = static_cast<float>(m_vy - m_overlayRect.top  - m_hotY);
    return true;
}

bool MouseMapper::ConsumeCursorImage(const uint32_t** outPixels)
{
    if (!m_imageDirty || m_cursorPixels.empty()) return false;
    m_imageDirty = false;
    if (outPixels) *outPixels = m_cursorPixels.data();
    return true;
}
