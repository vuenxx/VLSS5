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

Object.entries(buttons).forEach(([id, btn]) => {
  btn.addEventListener("click", () => {
    if (capturingId !== -1) return;
    capturingId = +id;
    btn.textContent = "Tuşa Basın...";
    btn.classList.add("active");
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
  if (capturingId !== -1 && buttons[capturingId]) {
    buttons[capturingId].classList.remove("active");
  }
  capturingId = -1;
  vlss5.send("getHotkeys");
});

vlss5.send("getHotkeys");
