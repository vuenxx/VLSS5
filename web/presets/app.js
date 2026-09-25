const $ = (id) => document.getElementById(id);

document.getElementById("btnClose").onclick = () => vlss5.send("close");

let cache = [];
let selectedFolder = null;

$("btnOpenFolder").addEventListener("click", () => vlss5.send("openFolder"));
$("btnDelete").addEventListener("click", () => {
  if (!selectedFolder) return;
  vlss5.send("deletePreset", { folder: selectedFolder });
});

function render() {
  const box = $("listBox");
  box.innerHTML = "";
  $("emptyMsg").style.display = cache.length === 0 ? "" : "none";
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

  $("btnDelete").disabled = !selectedFolder;
}

vlss5.on("presetList", (msg) => {
  cache = msg.data || [];
  if (!cache.some((p) => p.folder === selectedFolder)) {
    selectedFolder = cache.length ? cache[0].folder : null;
  }
  render();
});

vlss5.send("listPresets");
