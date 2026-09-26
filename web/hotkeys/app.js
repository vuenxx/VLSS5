const $ = (id) => document.getElementById(id);

document.getElementById("btnClose").onclick = () => vlss5.send("close");

// hotkeyId <-> native ile birebir ayni (bkz. HotkeysWindow.cpp StartKeybindCapture):
// 0=Settings, 2=Focus, 3=FPS, 4=VLSS, 5=Calib, 6=Start, 7=DismissWarning
const buttons = {
  0: $("btnSettings"),
  2: $("btnFocus"),
  3: $("btnFps"),
  4: $("btnVlss"),
  5: $("btnCalib"),
  6: $("btnStart"),
  7: $("btnDismissWarning"),
};

let capturingId = -1;

// Tus yakalama artik native bir WH_KEYBOARD_LL hook'u YERINE, bu WebView2
// sayfasinin kendi "keydown" olayiyla yapiliyor (bkz. HotkeysWindow.cpp
// OnWebMessage "keyPicked"). Pencere zaten odaktayken (kullanici butona
// tikladi) tarayici tarafi keydown'u guvenilir sekilde aliyor; sistem geneli
// bir hook'a hic gerek yok -- ve boylece antivirus'un "global keylogger"
// olarak okudugu davranis tamamen ortadan kalkiyor.
const MOD_ALT = 0x0001, MOD_CONTROL = 0x0002, MOD_SHIFT = 0x0004, MOD_WIN = 0x0008;
const BARE_MODIFIER_KEYCODES = new Set([16, 17, 18, 91, 92]); // Shift/Ctrl/Alt/LWin/RWin

function onCaptureKeydown(e) {
  if (capturingId === -1) return;
  e.preventDefault();
  e.stopPropagation();

  if (e.key === "Escape") {
    cancelCapture();
    return;
  }
  if (BARE_MODIFIER_KEYCODES.has(e.keyCode)) return; // salt modifier tek basina gecerli deger degil

  let mods = 0;
  if (e.ctrlKey)  mods |= MOD_CONTROL;
  if (e.altKey)   mods |= MOD_ALT;
  if (e.shiftKey) mods |= MOD_SHIFT;
  if (e.metaKey)  mods |= MOD_WIN;

  // Chromium/Windows'ta keyCode, WM_KEYDOWN'daki ham VK koduyla ayni --
  // ayri bir JS->VK cevrim tablosuna gerek yok.
  vlss5.send("keyPicked", { hotkeyId: capturingId, vk: e.keyCode, mods });
  endCaptureUi();
}

function cancelCapture() {
  vlss5.send("cancelCapture", { hotkeyId: capturingId });
  endCaptureUi();
}

function endCaptureUi() {
  if (capturingId !== -1 && buttons[capturingId]) {
    buttons[capturingId].classList.remove("active");
  }
  capturingId = -1;
}

window.addEventListener("keydown", onCaptureKeydown, true);

Object.entries(buttons).forEach(([id, btn]) => {
  btn.addEventListener("click", () => {
    if (capturingId !== -1) return;
    capturingId = +id;
    btn.textContent = "Tuşa Basın...";
    btn.classList.add("active");
    // Native tarafa yakalamanin basladigini bildirmezsek s_rebindingKey hic
    // true olmuyor ve asagidaki "keyPicked" native'de sessizce yok sayiliyor
    // (bkz. HotkeysWindow::EndKeybindCapture erken "if (!s_rebindingKey) return").
    vlss5.send("startCapture", { hotkeyId: capturingId });
  });
});

vlss5.on("hotkeys", (msg) => {
  const labels = msg.data || {};
  for (const [id, btn] of Object.entries(buttons)) {
    if (labels[id] !== undefined) btn.textContent = labels[id];
  }
});

vlss5.on("keyCaptured", () => {
  vlss5.send("getHotkeys");
});

vlss5.send("getHotkeys");
