const $ = (id) => document.getElementById(id);

let lastState = null;

// ---------------------------------------------------------------------------
// Target list
// ---------------------------------------------------------------------------
function renderTargets(state) {
  const box = $("targetList");
  box.innerHTML = "";

  const targets = state.targets || [];
  targets.forEach((t, i) => {
    const item = document.createElement("div");
    item.className = "list-item" + (t.isMonitor ? " monitor" : "");
    if (t.id === state.selectedTargetId) item.classList.add("selected");

    // Son "Tum Ekran" (monitor) girisinden sonra, gercek pencereler baslamadan
    // once bir ayrac cizgisi (eski owner-draw listbox'taki davranisla ayni).
    const next = targets[i + 1];
    if (t.isMonitor && (!next || !next.isMonitor)) {
      item.classList.add("monitor-sep");
    }

    if (t.icon) {
      const img = document.createElement("img");
      img.className = "item-icon";
      img.src = t.icon;
      img.alt = "";
      item.appendChild(img);
    } else {
      const glyph = document.createElement("span");
      glyph.className = "item-icon item-icon-glyph";
      glyph.textContent = t.isMonitor ? "🖥" : "🗔";
      item.appendChild(glyph);
    }
    const label = document.createElement("span");
    label.textContent = t.label;
    item.appendChild(label);

    item.addEventListener("click", () => {
      vlss5.send("selectTarget", { id: t.id });
    });
    box.appendChild(item);
  });

  if (targets.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-msg";
    empty.textContent = "Pencere bulunamadı. Yenile'ye basın.";
    box.appendChild(empty);
  }
}

// ---------------------------------------------------------------------------
// GPU / DLSS-GPU / capture-backend selects
// ---------------------------------------------------------------------------
function renderSelect(selectEl, items, selectedIndex) {
  const prevLen = selectEl.options.length;
  if (prevLen !== (items || []).length) {
    selectEl.innerHTML = "";
    (items || []).forEach((label, i) => {
      const opt = document.createElement("option");
      opt.value = String(i);
      opt.textContent = label;
      selectEl.appendChild(opt);
    });
  } else {
    // Ayni uzunluktaysa metinleri yerinde guncelle (secim odagini bozmamak icin).
    (items || []).forEach((label, i) => {
      if (selectEl.options[i] && selectEl.options[i].textContent !== label) {
        selectEl.options[i].textContent = label;
      }
    });
  }
  selectEl.selectedIndex = selectedIndex;
}

// ---------------------------------------------------------------------------
// Toggle switches
// ---------------------------------------------------------------------------
function setToggle(id, on) {
  $(id).classList.toggle("on", !!on);
}

// ---------------------------------------------------------------------------
// Full state apply
// ---------------------------------------------------------------------------
function applyState(state) {
  lastState = state;

  renderTargets(state);
  renderSelect($("gpuSelect"), state.gpuList, state.selectedGpu);
  renderSelect($("dlssGpuSelect"), state.dlssGpuList, state.selectedDlssGpu);
  renderSelect($("backendSelect"), state.captureBackendList, state.selectedCaptureBackend);
  $("backendSelect").disabled = !!state.captureBackendLocked;

  setToggle("swVsync", state.vsync);
  setToggle("swFps", state.fps);
  setToggle("swDlss", state.dlss);
  setToggle("swFullscreen", state.fullscreenStretch);

  $("fpsModeSelect").value = String(state.fpsDisplayMode || 0);
  $("rowFpsMode").classList.toggle("disabled", !state.fps);

  const btnStart = $("btnStart");
  if (state.isCapturing) {
    btnStart.textContent = "DURDUR  ■";
    btnStart.classList.add("running");
    btnStart.disabled = false;
  } else {
    const start = (state.hotkeyLabels && state.hotkeyLabels.start) || "";
    btnStart.textContent = "BAŞLAT  ➔" + (start ? "   [" + start + "]" : "");
    btnStart.classList.remove("running");
    // nvngx_dlssnr.dll yoksa BASLAT'a basilamaz (bkz. dllStatusBar / setNvngxStatus) --
    // native tarafta da AYNI kontrol var (Main.cpp "startStop"), burasi sadece UX.
    btnStart.disabled = !state.nvngxDlssnrReady;
  }

  setNvngxStatus(!!state.nvngxDlssnrReady);

  if (state.hotkeyLabels) {
    const h = state.hotkeyLabels;
    $("hotkeyPill").textContent =
      "[" + h.focus + "] Odak | [" + h.fps + "] FPS | [" + h.toggleVlss + "] VLSS5 | [" + h.start + "] Başlat";
  }

  $("statusText").textContent = state.statusText || "";

  // Yakalama surerken "Gorüntü Ayarları (Varsayılan)" sekmesi kilitlenir --
  // buradaki degisiklikler native tarafta zaten reddediliyor (bkz. Main.cpp
  // settingsSetConfig/settingsLoadPreset), ama kullanicinin "neden hicbir sey
  // olmuyor" diye sasirmamasi icin gorsel olarak da devre disi gosteriyoruz.
  $("tabPanel-settings").classList.toggle("capturing-locked", !!state.isCapturing);
}

vlss5.on("state", (msg) => applyState(msg.data));

// ---------------------------------------------------------------------------
// nvngx_dlssnr.dll durum isigi + surukle-birak yukleme -- BASLAT'in USTUNDE,
// sag altta ayri bir bolum (bkz. web/main/index.html .homeBottom).
//
// WebView2 (standart web File API gibi) suruklenen dosyanin GERCEK dosya
// sistemi YOLUNU vermiyor -- sadece icerigini (File nesnesi) veriyor. Bu
// yuzden yol native'e gonderilmiyor; dosya PARCALARA bolunup base64 ile
// IPC uzerinden native'e akitiliyor (bkz. Main.cpp "nvngxDropBegin/Chunk/End"),
// native de bunlari nvngx_dlssnr.dll olarak diske yaziyor. Buyuk (>100MB)
// dosyalarda tek seferde tum icerigi belleğe/JSON'a almamak icin 4MB'lik
// parcalar kullanilir.
// ---------------------------------------------------------------------------
const NVNGX_DLSSNR_NAME = "nvngx_dlssnr.dll";
const NVNGX_CHUNK_SIZE = 4 * 1024 * 1024;

let nvngxReady = false;
let nvngxUploading = false;

function setNvngxStatus(ready) {
  nvngxReady = ready;
  if (nvngxUploading) return; // yukleme surerken ilerleme metnini ezme

  $("dllStatusPill").classList.toggle("ready", ready);
  // Hazirsa dosya adi yeterli; degilse ne yapilmasi gerektigi HOVER OLMADAN
  // dogrudan yazida gorunsun (tooltip'e gomulmez).
  $("dllStatusPillText").textContent = ready
    ? NVNGX_DLSSNR_NAME + " hazır"
    : NVNGX_DLSSNR_NAME + " — buraya sürükleyip bırakın";
}

function arrayBufferToBase64(buf) {
  let binary = "";
  const bytes = new Uint8Array(buf);
  const step = 0x8000;
  for (let i = 0; i < bytes.length; i += step) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + step));
  }
  return btoa(binary);
}

async function uploadNvngxDlssnr(file) {
  nvngxUploading = true;
  const text = $("dllStatusPillText");

  vlss5.send("nvngxDropBegin", { name: file.name, size: file.size });

  let offset = 0;
  let seq = 0;
  while (offset < file.size) {
    const slice = file.slice(offset, offset + NVNGX_CHUNK_SIZE);
    const buf = await slice.arrayBuffer();
    vlss5.send("nvngxDropChunk", { seq, dataB64: arrayBufferToBase64(buf) });
    offset += buf.byteLength;
    seq++;
    text.textContent = "Yükleniyor... %" + Math.round((offset / file.size) * 100);
  }
  vlss5.send("nvngxDropEnd", {});
}

(function () {
  const pill = $("dllStatusPill");

  ["dragenter", "dragover"].forEach((evt) => {
    pill.addEventListener(evt, (e) => {
      e.preventDefault();
      if (!nvngxUploading) pill.classList.add("dragover");
    });
  });
  ["dragleave"].forEach((evt) => {
    pill.addEventListener(evt, (e) => {
      e.preventDefault();
      pill.classList.remove("dragover");
    });
  });

  pill.addEventListener("drop", (e) => {
    e.preventDefault();
    pill.classList.remove("dragover");
    if (nvngxUploading) return;

    const file = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    if (!file) return;

    if (file.name.toLowerCase() !== NVNGX_DLSSNR_NAME) {
      const text = $("dllStatusPillText");
      text.textContent = `Yalnızca "${NVNGX_DLSSNR_NAME}" kabul edilir`;
      setTimeout(() => setNvngxStatus(nvngxReady), 3000);
      return;
    }

    uploadNvngxDlssnr(file);
  });

  vlss5.on("nvngxDropResult", (msg) => {
    nvngxUploading = false;
    const data = msg.data || {};
    if (data.ok) {
      setNvngxStatus(true);
    } else {
      setNvngxStatus(nvngxReady);
      $("dllStatusPillText").textContent = "Yükleme başarısız";
      setTimeout(() => setNvngxStatus(nvngxReady), 3000);
    }
  });
})();

// ---------------------------------------------------------------------------
// User interactions -> native
// ---------------------------------------------------------------------------
$("btnRefresh").addEventListener("click", () => vlss5.send("refresh"));

$("swVsync").addEventListener("click", () => {
  const next = !$("swVsync").classList.contains("on");
  setToggle("swVsync", next);
  vlss5.send("setVsync", { value: next });
});
$("swFps").addEventListener("click", () => {
  const next = !$("swFps").classList.contains("on");
  setToggle("swFps", next);
  $("rowFpsMode").classList.toggle("disabled", !next);
  vlss5.send("setFps", { value: next });
});
$("fpsModeSelect").addEventListener("change", () => {
  vlss5.send("setFpsDisplayMode", { value: parseInt($("fpsModeSelect").value, 10) });
});
$("swDlss").addEventListener("click", () => {
  const next = !$("swDlss").classList.contains("on");
  setToggle("swDlss", next);
  vlss5.send("setDlss", { value: next });
});
$("swFullscreen").addEventListener("click", () => {
  const next = !$("swFullscreen").classList.contains("on");
  setToggle("swFullscreen", next);
  vlss5.send("setFullscreen", { value: next });
});

$("gpuSelect").addEventListener("change", () => {
  vlss5.send("setGpu", { index: $("gpuSelect").selectedIndex });
});
$("dlssGpuSelect").addEventListener("change", () => {
  vlss5.send("setDlssGpu", { index: $("dlssGpuSelect").selectedIndex });
});

$("btnGpuHelp").addEventListener("click", () => vlss5.send("help", { topic: "gpu" }));
$("btnDlssGpuHelp").addEventListener("click", () => vlss5.send("help", { topic: "dlssGpu" }));
$("btnBackendHelp").addEventListener("click", () => vlss5.send("help", { topic: "captureBackend" }));

$("btnStart").addEventListener("click", () => {
  const targetId = lastState ? lastState.selectedTargetId : -1;
  vlss5.send("startStop", { targetId });
});

vlss5.send("getState");

// ---------------------------------------------------------------------------
// Faz 3: ust seviye gorunum gecisi -- Ana Sayfa / Ayarlar (salt JS)
// ---------------------------------------------------------------------------
const VIEW_NAMES = ["home", "updates", "settings"];

function switchView(name) {
  VIEW_NAMES.forEach((n) => {
    $("navBtn-" + n).classList.toggle("active", n === name);
    $("view-" + n).classList.toggle("hidden", n !== name);
  });
}

VIEW_NAMES.forEach((n) => {
  $("navBtn-" + n).addEventListener("click", () => switchView(n));
});

// ---------------------------------------------------------------------------
// Faz 3: alt sekme gecisi (Ayarlar gorunumu icinde, salt JS)
// ---------------------------------------------------------------------------
const TAB_NAMES = ["general", "settings"];

function switchTab(name) {
  TAB_NAMES.forEach((n) => {
    $("tabBtn-" + n).classList.toggle("active", n === name);
    $("tabPanel-" + n).classList.toggle("hidden", n !== name);
  });
}

TAB_NAMES.forEach((n) => {
  $("tabBtn-" + n).addEventListener("click", () => switchTab(n));
});

// ---------------------------------------------------------------------------
// Genel Ayarlar sekmesi: RTSS / On Ayarlar / Tuslar akordiyon bolumleri
// ---------------------------------------------------------------------------
document.querySelectorAll(".accordionHeader").forEach((header) => {
  header.addEventListener("click", () => {
    const acc = header.parentElement;
    const body = acc.querySelector(".accordionBody");
    const open = !body.classList.contains("collapsed");
    body.classList.toggle("collapsed", open);
    acc.classList.toggle("open", !open);
  });
});

// ===========================================================================
// AYARLAR sekmesi (bkz. web/settings/app.js) -- cmd/type "settings" onekiyle
// ===========================================================================
(function () {
  const stgSliders = ["stgIntensity", "stgBoost", "stgStructure", "stgTone", "stgSkin", "stgResScale", "stgPassCount", "stgPassFalloff", "stgSplitPos"];
  const stgSelects = ["stgStyle", "stgPreset"];
  const stgToggles = ["stgSwAutoMask", "stgSwOpticalFlow", "stgSwSplit"];

  let suppressEvents = false;

  // Kompakt rozetlere sigmasi icin kisa deger metinleri (aciklayici ekler
  // eski genis-yayilmis duzendeydi, dar valBadge'de tasiyordu).
  function fmtIntensity(v) { return (v / 100).toFixed(2) + "x"; }
  function fmtBoost(v) { return (v / 100).toFixed(2) + "x"; }
  function fmtStructure(v) { return (v / 100).toFixed(2) + "x"; }
  function fmtTone(v) { return (v / 100).toFixed(2) + "x"; }
  function fmtSkin(v) {
    const val = v / 100 - 1.0;
    if (val <= -0.99) return "Oto";
    return val.toFixed(2) + "x";
  }
  function fmtResScale(v) { return `%${v}`; }
  function fmtPassCount(v) { return String(v); }
  function fmtPassFalloff(v) { return (v / 100).toFixed(2) + "x"; }
  function fmtSplitPos(v) { return `%${v}`; }

  function updateLiveLabels() {
    $("stgIntensityVal").textContent = fmtIntensity(+$("stgIntensity").value);
    $("stgBoostVal").textContent = fmtBoost(+$("stgBoost").value);
    $("stgStructureVal").textContent = fmtStructure(+$("stgStructure").value);
    $("stgToneVal").textContent = fmtTone(+$("stgTone").value);
    $("stgSkinVal").textContent = fmtSkin(+$("stgSkin").value);
    $("stgResScaleVal").textContent = fmtResScale(+$("stgResScale").value);
    $("stgPassCountVal").textContent = fmtPassCount(+$("stgPassCount").value);
    $("stgPassFalloffVal").textContent = fmtPassFalloff(+$("stgPassFalloff").value);
    $("stgSplitPosVal").textContent = fmtSplitPos(+$("stgSplitPos").value);
  }

  function updateLayout() {
    const splitOn = $("stgSwSplit").classList.contains("on");
    $("stgRowSplitPos").style.display = splitOn ? "" : "none";
  }

  function readConfigFromForm() {
    return {
      style: +$("stgStyle").value,
      preset: +$("stgPreset").value,
      intensity: +$("stgIntensity").value / 100,
      boostFactor: Math.min(2.5, Math.max(1.0, +$("stgBoost").value / 100)),
      localStructure: +$("stgStructure").value / 100,
      localTone: +$("stgTone").value / 100,
      skinStructure: (() => {
        const v = +$("stgSkin").value / 100 - 1.0;
        return v <= -0.99 ? -1.0 : v;
      })(),
      resolutionScale: +$("stgResScale").value,
      passCount: Math.min(4, Math.max(1, +$("stgPassCount").value)),
      passFalloff: Math.min(1.0, Math.max(0.25, +$("stgPassFalloff").value / 100)),
      useAutoMask: $("stgSwAutoMask").classList.contains("on"),
      opticalFlow: $("stgSwOpticalFlow").classList.contains("on"),
      splitScreen: $("stgSwSplit").classList.contains("on"),
      splitPos: Math.min(1.0, Math.max(0.0, +$("stgSplitPos").value / 100)),
    };
  }

  function applyConfigToForm(cfg) {
    suppressEvents = true;
    $("stgStyle").value = cfg.style;
    $("stgPreset").value = cfg.preset;
    $("stgIntensity").value = Math.round(cfg.intensity * 100);
    $("stgBoost").value = Math.min(250, Math.max(100, Math.round(cfg.boostFactor * 100)));
    $("stgStructure").value = Math.round(cfg.localStructure * 100);
    $("stgTone").value = Math.round(cfg.localTone * 100);
    $("stgSkin").value = cfg.skinStructure <= -0.99 ? 0 : Math.round((cfg.skinStructure + 1.0) * 100);
    $("stgResScale").value = cfg.resolutionScale;
    $("stgPassCount").value = cfg.passCount;
    $("stgPassFalloff").value = Math.round(cfg.passFalloff * 100);
    setToggle("stgSwAutoMask", cfg.useAutoMask);
    setToggle("stgSwOpticalFlow", cfg.opticalFlow);
    setToggle("stgSwSplit", cfg.splitScreen);
    $("stgSplitPos").value = Math.round(cfg.splitPos * 100);
    updateLiveLabels();
    updateLayout();
    suppressEvents = false;
  }

  function onSettingChanged() {
    if (suppressEvents) return;
    updateLiveLabels();
    vlss5.send("settingsSetConfig", { data: readConfigFromForm() });
  }

  stgSliders.forEach((id) => {
    $(id).addEventListener("input", () => { updateLiveLabels(); });
    $(id).addEventListener("change", onSettingChanged);
  });
  stgSelects.forEach((id) => $(id).addEventListener("change", onSettingChanged));

  stgToggles.forEach((id) => {
    $(id).addEventListener("click", () => {
      $(id).classList.toggle("on");
      if (id === "stgSwSplit") updateLayout();
      onSettingChanged();
    });
  });

  $("stgPresetSourceList").addEventListener("change", () => {
    const sel = $("stgPresetSourceList").value;
    if (sel === "") return;
    vlss5.send("settingsLoadPreset", { folder: sel });
  });

  vlss5.on("settingsConfig", (msg) => applyConfigToForm(msg.data));

  vlss5.on("settingsPresetList", (msg) => {
    const sel = $("stgPresetSourceList");
    sel.innerHTML = '<option value="">(Seçilmedi)</option>';
    (msg.data || []).forEach((p) => {
      const opt = document.createElement("option");
      opt.value = p.folder;
      opt.textContent = p.displayName;
      sel.appendChild(opt);
    });
  });

  vlss5.send("settingsGetConfig");
  vlss5.send("settingsListPresets");
})();

// ===========================================================================
// RTSS AYARLARI sekmesi (bkz. web/rtss/app.js) -- cmd/type "rtss" onekiyle
// ===========================================================================
(function () {
  $("rtssBtnBrowse").addEventListener("click", () => vlss5.send("rtssBrowseFolder"));
  $("rtssBtnRefresh").addEventListener("click", () => vlss5.send("rtssListProfiles"));
  $("rtssBtnDelete").addEventListener("click", () => {
    const sel = $("rtssProfileList").value;
    if (!sel) return;
    vlss5.send("rtssDeleteProfile", { name: sel });
  });

  vlss5.on("rtssDir", (msg) => { $("rtssDirPath").textContent = msg.data || ""; });

  vlss5.on("rtssProfileList", (msg) => {
    const sel = $("rtssProfileList");
    const prev = sel.value;
    sel.innerHTML = "";
    (msg.data || []).forEach((name) => {
      const opt = document.createElement("option");
      opt.value = name;
      opt.textContent = name;
      sel.appendChild(opt);
    });
    if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
  });

  vlss5.send("rtssGetDir");
  vlss5.send("rtssListProfiles");
})();

// ===========================================================================
// ON AYARLAR sekmesi (bkz. web/presets/app.js) -- cmd/type "presets" onekiyle
// ===========================================================================
(function () {
  let cache = [];
  let selectedFolder = null;

  $("presetsBtnOpenFolder").addEventListener("click", () => vlss5.send("presetsOpenFolder"));
  $("presetsBtnDelete").addEventListener("click", () => {
    if (!selectedFolder) return;
    vlss5.send("presetsDeletePreset", { folder: selectedFolder });
  });

  function render() {
    const box = $("presetsListBox");
    box.innerHTML = "";
    $("presetsEmptyMsg").style.display = cache.length === 0 ? "" : "none";
    box.style.display = cache.length === 0 ? "none" : "";

    cache.forEach((p) => {
      const div = document.createElement("div");
      div.className = "list-item" + (p.folder === selectedFolder ? " selected" : "");
      div.textContent = p.displayName;
      div.addEventListener("click", () => {
        selectedFolder = p.folder;
        render();
      });
      box.appendChild(div);
    });

    $("presetsBtnDelete").disabled = !selectedFolder;
  }

  vlss5.on("presetsPresetList", (msg) => {
    cache = msg.data || [];
    if (!cache.some((p) => p.folder === selectedFolder)) {
      selectedFolder = cache.length ? cache[0].folder : null;
    }
    render();
  });

  vlss5.send("presetsList");
})();

// ===========================================================================
// TUSLARI DEGISTIR sekmesi (bkz. web/hotkeys/app.js) -- cmd/type "hotkeys" onekiyle
// ===========================================================================
(function () {
  // hotkeyId <-> native ile birebir ayni (bkz. HotkeysWindow.cpp StartKeybindCapture):
  // 0=Settings, 2=Focus, 3=FPS, 4=VLSS, 5=Calib, 6=Start, 7=DismissWarning
  const buttons = {
    0: $("hkBtnSettings"),
    2: $("hkBtnFocus"),
    3: $("hkBtnFps"),
    4: $("hkBtnVlss"),
    5: $("hkBtnCalib"),
    6: $("hkBtnStart"),
    7: $("hkBtnDismissWarning"),
  };

  let capturingId = -1;

  Object.entries(buttons).forEach(([id, btn]) => {
    btn.addEventListener("click", () => {
      if (capturingId !== -1) return;
      capturingId = +id;
      btn.textContent = "Tuşa Basın...";
      btn.classList.add("active");
      vlss5.send("hotkeysStartCapture", { hotkeyId: capturingId });
    });
  });

  vlss5.on("hotkeysList", (msg) => {
    const labels = msg.data || {};
    for (const [id, btn] of Object.entries(buttons)) {
      if (labels[id] !== undefined) btn.textContent = labels[id];
    }
  });

  vlss5.on("hotkeysKeyCaptured", () => {
    if (capturingId !== -1 && buttons[capturingId]) {
      buttons[capturingId].classList.remove("active");
    }
    capturingId = -1;
    vlss5.send("hotkeysGetHotkeys");
  });

  vlss5.send("hotkeysGetHotkeys");
})();

// ===========================================================================
// GUNCELLEMELER sekmesi (bkz. src/UpdateChecker.h/.cpp, Main.cpp PushUpdatesStateToJs)
// -- cmd/type "updates" onekiyle
// ===========================================================================
(function () {
  let lastUpdatesState = null;

  function fmtDate(iso) {
    if (!iso) return "";
    const d = new Date(iso);
    if (isNaN(d.getTime())) return "";
    return d.toLocaleDateString("tr-TR", { year: "numeric", month: "2-digit", day: "2-digit" });
  }

  function fmtBytes(n) {
    if (!n) return "";
    const units = ["B", "KB", "MB", "GB"];
    let v = n, i = 0;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return v.toFixed(i === 0 || v >= 10 ? 0 : 1) + " " + units[i];
  }

  // Native UpdateChecker::IsAutoInstallableAsset ile AYNI kaba desen: adı
  // ".exe" ile bitiyor ve içinde "setup" geçiyorsa bu bir Inno Setup
  // installer'ıdır -- indirilince sessizce kurulup uygulama otomatik
  // yeniden başlar (bkz. Main.cpp LaunchSilentInstallAndExit). Aksi halde
  // (eski sürümlerin .rar asset'i) sadece varsayılan programla açılır.
  function isAutoInstallable(name) {
    const n = (name || "").toLowerCase();
    return n.endsWith(".exe") && n.includes("setup");
  }

  function pickBestAsset(release) {
    const assets = release.assets || [];
    return assets.find((a) => isAutoInstallable(a.name)) || assets[0] || null;
  }

  function renderReleases(releases) {
    const box = $("updReleaseList");
    box.innerHTML = "";

    if (!releases || releases.length === 0) {
      const empty = document.createElement("div");
      empty.className = "empty-msg";
      empty.textContent = "Sürüm geçmişi yüklenemedi.";
      box.appendChild(empty);
      return;
    }

    // Sadece EN YENI (ilk) sürüm güncellenebilir/indirilebilir -- geçmiş
    // sürümler yalnızca değişiklik notu (changelog) olarak okunur, ayrı
    // ayrı indirme/güncelleme butonu YOK (bkz. üstteki "Şimdi Güncelle").
    releases.forEach((r) => {
      const item = document.createElement("div");
      item.className = "updReleaseItem";

      const header = document.createElement("div");
      header.className = "updReleaseHeader";

      const tag = document.createElement("span");
      tag.className = "updReleaseTag";
      tag.textContent = r.tag || r.name || "";
      header.appendChild(tag);

      if (r.prerelease) {
        const badge = document.createElement("span");
        badge.className = "updReleaseBadge";
        badge.textContent = "ÖN SÜRÜM";
        header.appendChild(badge);
      }

      const date = document.createElement("span");
      date.className = "updReleaseDate";
      date.textContent = fmtDate(r.publishedAt);
      header.appendChild(date);

      item.appendChild(header);

      const body = document.createElement("div");
      body.className = "updReleaseBody";
      // GitHub API'nin ONCEDEN kendi tarafinda render ettigi HTML (bkz.
      // UpdateChecker::FetchLatestReleases "Accept: application/vnd.github.html+json") --
      // burada uretici/attacker kontrolunde degil, kendi GitHub reposumuzun
      // resmi API yaniti.
      body.innerHTML = r.bodyHtml || "";
      item.appendChild(body);

      box.appendChild(item);
    });
  }

  function applyUpdatesState(data) {
    lastUpdatesState = data;
    $("updCurrentVersion").textContent = "v" + (data.currentVersion || "?");
    setToggle("updSwAutoCheck", data.autoCheck);

    const btn = $("updBtnCheck");
    btn.disabled = !!data.checking;
    btn.textContent = data.checking ? "Kontrol ediliyor..." : "Güncelleme Kontrol Et";

    if (!data.downloading) {
      const banner = $("updStatusBanner");
      banner.classList.remove("newer", "error");
      if (data.error) {
        banner.textContent = "Kontrol edilemedi: " + data.error;
        banner.classList.add("error");
      } else if (data.checking) {
        banner.textContent = "Güncellemeler kontrol ediliyor...";
      } else if (data.hasUpdate) {
        banner.textContent = "Yeni güncelleme mevcut: " + data.latestTag;
        banner.classList.add("newer");
      } else if (data.latestTag) {
        banner.textContent = "En güncel sürümü kullanıyorsunuz.";
      } else {
        banner.textContent = "";
      }
    }

    renderReleases(data.releases);

    const updateBtn = $("updBtnUpdateNow");
    const latest = (data.releases || [])[0];
    const latestAsset = latest ? pickBestAsset(latest) : null;
    updateBtn.classList.toggle("hidden", !(data.hasUpdate && latestAsset));
    if (latestAsset) {
      updateBtn.disabled = !!data.downloading;
      updateBtn.textContent = data.downloading
        ? "Güncelleniyor..."
        : (isAutoInstallable(latestAsset.name) ? "Şimdi Güncelle" : "Şimdi İndir");
    }
  }

  $("updBtnCheck").addEventListener("click", () => vlss5.send("updatesCheckNow"));

  $("updBtnUpdateNow").addEventListener("click", () => {
    const latest = lastUpdatesState ? (lastUpdatesState.releases || [])[0] : null;
    const asset = latest ? pickBestAsset(latest) : null;
    if (asset) vlss5.send("updatesDownload", { url: asset.url, name: asset.name });
  });

  $("updSwAutoCheck").addEventListener("click", () => {
    const next = !$("updSwAutoCheck").classList.contains("on");
    setToggle("updSwAutoCheck", next);
    vlss5.send("updatesSetAutoCheck", { value: next });
  });

  vlss5.on("updatesState", (msg) => applyUpdatesState(msg.data));

  vlss5.on("updatesDownloadProgress", (msg) => {
    const d = msg.data || {};
    const pct = d.total ? Math.round((d.received / d.total) * 100) : null;
    const banner = $("updStatusBanner");
    banner.classList.remove("newer", "error");
    banner.textContent = pct !== null ? ("İndiriliyor... %" + pct) : ("İndiriliyor... " + fmtBytes(d.received));
  });

  vlss5.on("updatesDownloadDone", (msg) => {
    const data = msg.data || {};
    const banner = $("updStatusBanner");
    banner.classList.remove("newer", "error");
    if (data.ok && data.autoInstalling) {
      banner.textContent = "İndirme tamamlandı. Güncelleme kuruluyor, VLSS5 birazdan otomatik olarak yeniden başlayacak...";
    } else if (data.ok) {
      banner.textContent = "İndirme tamamlandı, dosya açılıyor... (bu sürüm için otomatik kurulum yok, elle kurmanız gerekiyor)";
    } else {
      banner.textContent = "İndirme başarısız: " + (data.error || "");
      banner.classList.add("error");
    }
  });

  vlss5.send("updatesGetState");
})();
