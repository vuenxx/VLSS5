const $ = (id) => document.getElementById(id);

document.getElementById("btnClose").onclick = () => vlss5.send("close");

const sliders = ["intensity", "boost", "structure", "tone", "skin", "resScale", "passCount", "passFalloff", "splitPos"];
const selects = ["style", "preset"];
const toggles = [
  { id: "swAutoMask", field: "useAutoMask" },
  { id: "swOpticalFlow", field: "opticalFlow" },
  { id: "swSplit", field: "splitScreen" },
];

let suppressEvents = false;

function fmtIntensity(v) { return (v / 100).toFixed(2) + "x"; }
function fmtBoost(v) {
  if (v <= 100) return "1.00x (Normal)";
  if (v >= 250) return "2.50x (Maks)";
  return (v / 100).toFixed(2) + "x";
}
function fmtStructure(v) { return (v / 100).toFixed(2) + "x"; }
function fmtTone(v) { return (v / 100).toFixed(2) + "x"; }
function fmtSkin(v) {
  const val = v / 100 - 1.0;
  if (val <= -0.99) return "Otomatik";
  return val.toFixed(2) + "x";
}
function fmtResScale(v) {
  if (v >= 100) return "%100 (Kalite)";
  if (v >= 85) return `%${v} (Dengeli)`;
  if (v >= 70) return `%${v} (Performans)`;
  return `%${v} (Ultra Perf)`;
}
function fmtPassCount(v) {
  if (v <= 1) return "1 (Kapalı)";
  return `${v} geçiş (~${v}x maliyet)`;
}
function fmtPassFalloff(v) {
  if (v >= 100) return "1.00x (Tam)";
  return (v / 100).toFixed(2) + "x";
}
function fmtSplitPos(v) {
  if (v == 50) return "%50 (Tam Orta)";
  return `%${v}`;
}

function updateLiveLabels() {
  $("intensityVal").textContent = fmtIntensity(+$("intensity").value);
  $("boostVal").textContent = fmtBoost(+$("boost").value);
  $("structureVal").textContent = fmtStructure(+$("structure").value);
  $("toneVal").textContent = fmtTone(+$("tone").value);
  $("skinVal").textContent = fmtSkin(+$("skin").value);
  $("resScaleVal").textContent = fmtResScale(+$("resScale").value);
  $("passCountVal").textContent = fmtPassCount(+$("passCount").value);
  $("passFalloffVal").textContent = fmtPassFalloff(+$("passFalloff").value);
  $("splitPosVal").textContent = fmtSplitPos(+$("splitPos").value);
}

function updateLayout() {
  const splitOn = $("swSplit").classList.contains("on");
  $("rowSplitPos").style.display = splitOn ? "" : "none";
}

function readConfigFromForm() {
  return {
    style: +$("style").value,
    preset: +$("preset").value,
    intensity: +$("intensity").value / 100,
    boostFactor: Math.min(2.5, Math.max(1.0, +$("boost").value / 100)),
    localStructure: +$("structure").value / 100,
    localTone: +$("tone").value / 100,
    skinStructure: (() => {
      const v = +$("skin").value / 100 - 1.0;
      return v <= -0.99 ? -1.0 : v;
    })(),
    resolutionScale: +$("resScale").value,
    passCount: Math.min(4, Math.max(1, +$("passCount").value)),
    passFalloff: Math.min(1.0, Math.max(0.25, +$("passFalloff").value / 100)),
    useAutoMask: $("swAutoMask").classList.contains("on"),
    opticalFlow: $("swOpticalFlow").classList.contains("on"),
    splitScreen: $("swSplit").classList.contains("on"),
    splitPos: Math.min(1.0, Math.max(0.0, +$("splitPos").value / 100)),
  };
}

function applyConfigToForm(cfg) {
  suppressEvents = true;
  $("style").value = cfg.style;
  $("preset").value = cfg.preset;
  $("intensity").value = Math.round(cfg.intensity * 100);
  $("boost").value = Math.min(250, Math.max(100, Math.round(cfg.boostFactor * 100)));
  $("structure").value = Math.round(cfg.localStructure * 100);
  $("tone").value = Math.round(cfg.localTone * 100);
  $("skin").value = cfg.skinStructure <= -0.99 ? 0 : Math.round((cfg.skinStructure + 1.0) * 100);
  $("resScale").value = cfg.resolutionScale;
  $("passCount").value = cfg.passCount;
  $("passFalloff").value = Math.round(cfg.passFalloff * 100);
  setToggle("swAutoMask", cfg.useAutoMask);
  setToggle("swOpticalFlow", cfg.opticalFlow);
  setToggle("swSplit", cfg.splitScreen);
  $("splitPos").value = Math.round(cfg.splitPos * 100);
  updateLiveLabels();
  updateLayout();
  suppressEvents = false;
}

function setToggle(id, on) {
  $(id).classList.toggle("on", !!on);
}

function onSettingChanged() {
  if (suppressEvents) return;
  updateLiveLabels();
  vlss5.send("setConfig", { data: readConfigFromForm() });
}

sliders.forEach((id) => {
  $(id).addEventListener("input", () => { updateLiveLabels(); });
  $(id).addEventListener("change", onSettingChanged);
});
selects.forEach((id) => $(id).addEventListener("change", onSettingChanged));

toggles.forEach(({ id }) => {
  $(id).addEventListener("click", () => {
    $(id).classList.toggle("on");
    if (id === "swSplit") updateLayout();
    onSettingChanged();
  });
});

$("presetSourceList").addEventListener("change", () => {
  const sel = $("presetSourceList").value;
  if (sel === "") return;
  vlss5.send("loadPreset", { folder: sel });
});

vlss5.on("config", (msg) => applyConfigToForm(msg.data));

vlss5.on("presetList", (msg) => {
  const sel = $("presetSourceList");
  sel.innerHTML = '<option value="">(Seçilmedi)</option>';
  (msg.data || []).forEach((p) => {
    const opt = document.createElement("option");
    opt.value = p.folder;
    opt.textContent = p.displayName;
    sel.appendChild(opt);
  });
});

vlss5.send("getConfig");
vlss5.send("listPresets");
