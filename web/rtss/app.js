const $ = (id) => document.getElementById(id);

document.getElementById("btnClose").onclick = () => vlss5.send("close");

$("btnBrowse").addEventListener("click", () => vlss5.send("browseFolder"));
$("btnRefresh").addEventListener("click", () => vlss5.send("listProfiles"));
$("btnDelete").addEventListener("click", () => {
  const sel = $("profileList").value;
  if (!sel) return;
  vlss5.send("deleteProfile", { name: sel });
});

vlss5.on("rtssDir", (msg) => { $("dirPath").textContent = msg.data || ""; });

vlss5.on("profileList", (msg) => {
  const sel = $("profileList");
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

vlss5.send("getRtssDir");
vlss5.send("listProfiles");
