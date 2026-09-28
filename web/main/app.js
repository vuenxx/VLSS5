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
// nvngx_dlssnr.dll durum isigi -- BASLAT'in USTUNDE, sag altta ayri bir
// bolum (bkz. web/main/index.html .homeBottom). Salt bilgi amacli: dosya
// exe'nin yanindaysa yesil/"hazir", degilse ne yapilmasi gerektigini soyler.
// Surukle-birak yukleme kaldirildi -- dosyayi kullanici exe klasorune KENDI
// elle koyar (bkz. Main.cpp "nvngxStatus" -- IPC ile sadece durum okunur,
// artik dosya icerigi hic native'e gonderilmiyor).
// ---------------------------------------------------------------------------
const NVNGX_DLSSNR_NAME = "nvngx_dlssnr.dll";

function setNvngxStatus(ready) {
  $("dllStatusPill").classList.toggle("ready", ready);
  $("dllStatusPillText").textContent = ready
    ? NVNGX_DLSSNR_NAME + " hazır"
    : NVNGX_DLSSNR_NAME + " bulunamadı — dosyayı VLSS5.exe'nin yanına kopyalayın";
}

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
const VIEW_NAMES = ["home", "settings"];

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
  const stgSliders = ["stgIntensity", "stgBoost", "stgStructure", "stgTone", "stgSkin", "stgColourStrength", "stgResScale", "stgPassCount", "stgPassFalloff", "stgSplitPos"];
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
  function fmtColourStrength(v) { return `%${v}`; }
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
    $("stgColourStrengthVal").textContent = fmtColourStrength(+$("stgColourStrength").value);
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
      colourStrength: +$("stgColourStrength").value / 100,
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
    $("stgColourStrength").value = Math.round(cfg.colourStrength * 100);
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
// TUSLARI DEGISTIR paneli -- "tusa bas ve bekle" YOK: kullanici bir dropdown'dan
// dogrudan bir tus SECIYOR (bkz. konusma). hotkeyId <-> native ile birebir ayni
// (bkz. HotkeysWindow.cpp): 0=Settings, 2=Focus, 3=FPS, 4=VLSS, 5=Calib,
// 6=Start, 7=DismissWarning. Sadece Settings(0) ve Start(6) modifier (Ctrl/
// Alt/Shift) destekliyor -- digerleri native tarafta zaten bare-key.
// ===========================================================================
(function () {
  const MOD_ALT = 0x0001, MOD_CONTROL = 0x0002, MOD_SHIFT = 0x0004;

  function buildKeyOptions() {
    const opts = [];
    for (let i = 1; i <= 12; i++) opts.push({ vk: 0x6F + i, label: "F" + i }); // F1=0x70
    for (let i = 0; i <= 9; i++) opts.push({ vk: 0x30 + i, label: String(i) });
    for (let i = 0; i < 26; i++) opts.push({ vk: 0x41 + i, label: String.fromCharCode(65 + i) });
    [
      [0x2D, "Insert"], [0x2E, "Delete"], [0x24, "Home"], [0x23, "End"],
      [0x21, "Page Up"], [0x22, "Page Down"],
      [0x25, "Sol Ok"], [0x26, "Yukarı Ok"], [0x27, "Sağ Ok"], [0x28, "Aşağı Ok"],
      [0x09, "Tab"], [0x20, "Boşluk"], [0x14, "Caps Lock"],
    ].forEach(([vk, label]) => opts.push({ vk, label }));
    return opts;
  }
  const KEY_OPTIONS = buildKeyOptions();

  function populateSelect(sel) {
    KEY_OPTIONS.forEach(({ vk, label }) => {
      const opt = document.createElement("option");
      opt.value = String(vk);
      opt.textContent = label;
      sel.appendChild(opt);
    });
  }

  const simpleRows = {
    2: $("hkSelFocus"),
    3: $("hkSelFps"),
    4: $("hkSelVlss"),
    5: $("hkSelCalib"),
    7: $("hkSelDismissWarning"),
  };
  const comboRows = {
    0: { select: $("hkSelSettings"), ctrl: $("hkModSettings-CTRL"), alt: $("hkModSettings-ALT"), shift: $("hkModSettings-SHIFT") },
    6: { select: $("hkSelStart"),    ctrl: $("hkModStart-CTRL"),    alt: $("hkModStart-ALT"),    shift: $("hkModStart-SHIFT") },
  };

  Object.values(simpleRows).forEach(populateSelect);
  Object.values(comboRows).forEach((row) => populateSelect(row.select));

  function sendSetKey(hotkeyId, vk, mods) {
    vlss5.send("hotkeysSetKey", { hotkeyId, vk, mods });
  }

  Object.entries(simpleRows).forEach(([id, sel]) => {
    sel.addEventListener("change", () => sendSetKey(+id, +sel.value, 0));
  });

  Object.entries(comboRows).forEach(([id, row]) => {
    const onChange = () => {
      let mods = 0;
      if (row.ctrl.checked)  mods |= MOD_CONTROL;
      if (row.alt.checked)   mods |= MOD_ALT;
      if (row.shift.checked) mods |= MOD_SHIFT;
      sendSetKey(+id, +row.select.value, mods);
    };
    row.select.addEventListener("change", onChange);
    row.ctrl.addEventListener("change", onChange);
    row.alt.addEventListener("change", onChange);
    row.shift.addEventListener("change", onChange);
  });

  vlss5.on("hotkeysList", (msg) => {
    const data = msg.data || {};
    Object.entries(simpleRows).forEach(([id, sel]) => {
      const entry = data[id];
      if (entry) sel.value = String(entry.vk);
    });
    Object.entries(comboRows).forEach(([id, row]) => {
      const entry = data[id];
      if (!entry) return;
      row.select.value = String(entry.vk);
      row.ctrl.checked  = !!(entry.mods & MOD_CONTROL);
      row.alt.checked   = !!(entry.mods & MOD_ALT);
      row.shift.checked = !!(entry.mods & MOD_SHIFT);
    });
  });

  vlss5.send("hotkeysGetHotkeys");
})();
