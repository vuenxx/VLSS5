#pragma once
#include "Common.h"

// ---------------------------------------------------------------------------
// MouseMapper — "Tam Ekran Yap" modunda fare koordinat duzeltmesi
//
//  SORUN
//  -----
//  Overlay penceresi WS_EX_TRANSPARENT ile hit-test gecirgendir; fare olaylari
//  dogrudan ALTTAKI oyun penceresine gider. Tam Ekran modunda overlay MONITORUN
//  tamamini kaplar, oyun penceresi ise hala kendi kucuk dikdortgeninde durur.
//  Goruntu gerilmistir, koordinat gerilmemistir: kullanicinin tikladigi ekran
//  noktasi ile oyunun okudugu nokta arasinda (hedefRect / overlayRect) oraninda
//  sapma olusur.
//
//  COZUM
//  -----
//  1. Overlay uzerinden WM_INPUT (ham fare) dinlenir; RIDEV_INPUTSINK sayesinde
//     overlay odakta olmadan da gelir ve oyunun kendi girdisini TUKETMEZ --
//     ham girdi bir hook degildir, yalnizca bir kopyadir.
//  2. Kullanicinin GORSEL imlec konumu (m_vx/m_vy) overlay dikdortgeninde
//     tutulur ve ham deltalarla guncellenir.
//  3. Her harekette gercek OS imleci, bu gorsel konumun hedef pencere
//     dikdortgenindeki KARSILIGINA tasinir (SetCursorPos). Oyun artik
//     kullanicinin gordugu noktaya karsilik gelen koordinati okur; tiklama
//     dogru yere gider.
//  4. Gercek imlec kucuk hedef dikdortgeninde oldugundan yanlis yerde bir
//     "hayalet" ok olarak gorunurdu. Bu yuzden sistem imlecleri SetSystemCursor
//     ile bosaltilir ve overlay KENDI imlecini dogru konumda cizer
//     (Renderer::SetCursorOverlay). Cikista SPI_SETCURSORS ile geri yuklenir.
//
//  GUVENLIK AGI
//  ------------
//  FPS oyunlari imleci her kare ekran merkezine geri ceker (SetCursorPos).
//  Boyle bir oyunda bizim yazdigimiz konumla oyunun yazdigi konum catisirdi.
//  Bunu tespit ediyoruz: yazdigimiz konumda bulamadigimiz imlec ust uste
//  belli sayida olursa esleme gecici olarak ASKIYA alinir ve artan bekleme
//  (3s, 6s, 12s ... en fazla 60s) ile tekrar denenir. Oyun menuye dondugunde
//  esleme kendiliginden geri gelir.
// ---------------------------------------------------------------------------
class MouseMapper
{
public:
    // Overlay'in cizecegi imlec goruntusu (premultiplied BGRA, 128x128 sabit doku).
    static constexpr int kCursorTexSize = 128;

    ~MouseMapper() { Stop(); }

    // overlayHwnd  : ham girdinin gonderilecegi pencere (App'in overlay'i)
    // targetHwnd   : oyun penceresi
    // stretch      : "Tam Ekran Yap" acik mi. TEK yetkili anahtar budur.
    //                Kapaliyken modulun tamami olu: ne esleme, ne kilit, ne imlec
    //                bosaltma. Eskiden bunun yerine "overlay dikdortgeni hedefinkine
    //                esit mi" bakiliyordu; pencere tasinirken/boyutlanirken iki
    //                dikdortgen gecici olarak bir kac piksel ayrisip klasik modda
    //                imleci oyun penceresine HAPSEDIYORDU (gorunmez duvar).
    // mapping      : koordinat eslemesi acik mi
    // lock         : imleci hedef pencereye kilitle (ClipCursor)
    // hideSystem   : sistem imleclerini bosalt + kendi imlecimizi ciz
    bool Start(HWND overlayHwnd, HWND targetHwnd, bool stretch,
               bool mapping, bool lock, bool hideSystem);
    void Stop();

    // Overlay WndProc'undan WM_INPUT ile cagrilir.
    void OnRawInput(LPARAM lParam);

    // Her kare cagrilir.
    //  overlayRect/targetRect : EKRAN koordinatlarinda
    //  enabled : oyun modunda miyiz (ayarlar penceresi kapali, overlay gizli degil)
    void Tick(const RECT& overlayRect, const RECT& targetRect, bool enabled);

    // Esleme su an fiilen calisiyor mu (askiya alinmadi, olcek anlamli).
    bool IsMappingActive() const { return m_mappingActive; }

    // ---- Overlay imleci ----
    // Cizilecek imlec var mi; varsa sol-ust kosesi overlay-yerel piksel olarak.
    bool GetCursorDrawPos(float& outX, float& outY) const;
    // Yeni bir imlec goruntusu hazirlandiysa true doner ve bayragi temizler.
    bool ConsumeCursorImage(const uint32_t** outPixels);

private:
    // Ham girdi kaydi YALNIZCA esleme fiilen calisirken acik tutulur. Aksi halde
    // 1000 Hz'lik WM_INPUT akisi, hicbir ise yaramadan render dongusunu her fare
    // hareketinde uyandirirdi (MsgWaitForMultipleObjectsEx / QS_ALLINPUT).
    bool  EnsureRawInput(bool on);
    void  ApplyMapping();                       // m_vx/m_vy -> SetCursorPos
    void  ResyncFromPhysical();                 // gercek imlecten sanal konumu turet
    void  Suspend(const char* reason);
    void  UpdateClip();
    void  RefreshCursorImage();                 // GetCursorInfo -> doku pikselleri

    // Sistem imleclerini bosaltma / geri yukleme
    void  BlankSystemCursors();
    void  RestoreSystemCursors();
    bool  RenderCursorToPixels(HCURSOR cur, int& hotX, int& hotY);

    HWND   m_overlay   = nullptr;
    HWND   m_target    = nullptr;
    bool   m_stretch   = false;
    bool   m_mapping   = false;
    bool   m_lock      = false;
    bool   m_hide      = false;
    bool   m_started   = false;
    bool   m_rawRegistered = false;

    RECT   m_overlayRect = {};
    RECT   m_targetRect  = {};
    bool   m_enabled       = false;   // Tick'ten gelen "oyun modundayiz" bayragi
    bool   m_mappingActive = false;

    double m_vx = 0.0, m_vy = 0.0;    // gorsel imlec konumu (ekran koordinati)
    double m_speedScale = 1.0;        // Windows isaretci hizi (SPI_GETMOUSESPEED)
    POINT  m_lastSet = { 0, 0 };
    bool   m_haveVirtual = false;

    int       m_foreignMoves  = 0;
    ULONGLONG m_suspendUntil  = 0;
    int       m_backoffMs     = 3000;
    ULONGLONG m_healthySince  = 0;

    RECT      m_appliedClip   = {};
    bool      m_clipApplied   = false;

    // ---- imlec goruntusu ----
    HCURSOR   m_lastCursor    = nullptr;
    int       m_hotX = 0, m_hotY = 0;
    bool      m_imageDirty    = false;
    bool      m_haveImage     = false;
    std::vector<uint32_t> m_cursorPixels;   // kCursorTexSize^2, premultiplied BGRA

    // Bosaltilmis sistem imlecleri -> orijinal goruntuleri.
    // OLCULEN: LoadCursorW(NULL, IDC_*) bosaltmadan ONCE ve SONRA AYNI handle'i
    // dondurur (0x10003 -> 0x10003); SetSystemCursor yalnizca handle'in ardindaki
    // goruntuyu degistirir. Yine de garanti edilmis bir davranis olmadigi icin her
    // iki handle da saklanip ikisiyle de karsilastiriliyor.
    struct SystemCursorSlot
    {
        DWORD   ocrId       = 0;
        HCURSOR sysHandle   = nullptr;   // bosaltmadan onceki sistem handle'i
        HCURSOR blankHandle = nullptr;   // bosaltmadan sonraki sistem handle'i
        HCURSOR original    = nullptr;   // bosaltmadan once alinmis goruntu kopyasi
    };
    std::vector<SystemCursorSlot> m_systemCursors;
    bool m_cursorsBlanked = false;
};
