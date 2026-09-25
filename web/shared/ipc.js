// VLSS5 IPC helper -- native <-> JS JSON mesaj koprusu.
// Native taraf WebViewHost::PostJson ile push eder (window.chrome.webview 'message'
// event'i), JS send() ile native'e postalar (WebViewHost WebMessageReceived'da yakalar).
(function () {
  const handlers = {};

  function send(cmd, payload) {
    const msg = Object.assign({ cmd }, payload || {});
    if (window.chrome && window.chrome.webview) {
      // Nesneyi OLDUGU GIBI posta -- JSON.stringify ETME. WebView2, string
      // olarak postalanan mesajlari native tarafta get_WebMessageAsJson()
      // ile CIFT KODLANMIS (tirnakli JSON string literali) olarak geri verir;
      // bu da native'de json::parse sonrasi bir "object" degil bir "string"
      // degeri uretip ilk msg.value("cmd", ...) cagrisinda type_error firlatip
      // COM sinirini asarak butun uygulamayi sessizce cokertiyordu. Nesneyi
      // dogrudan postalamak WebView2'nin kendi JSON serilestirmesini kullanmasini
      // saglar ve native tarafta gercek bir JSON object olarak parse edilir.
      window.chrome.webview.postMessage(msg);
    } else {
      console.log("[ipc:mock send]", msg);
    }
  }

  function on(type, fn) {
    handlers[type] = fn;
  }

  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener("message", (e) => {
      let data = e.data;
      if (typeof data === "string") {
        try { data = JSON.parse(data); } catch (err) { return; }
      }
      if (data && data.type && handlers[data.type]) {
        handlers[data.type](data);
      }
    });
  }

  window.vlss5 = { send, on };

  // ---------------------------------------------------------------------
  // WebView2'nin (Chromium) VARSAYILAN surukle-birak davranisini TUM
  // sayfalarda kapat: hicbir eleman preventDefault yapmazsa, sayfa uzerine
  // birakilan bir dosyayi TARAYICI GIBI file:// URL'i olarak ACMAYA/
  // INDIRMEYE calisir -- bir .exe icin bu, Windows SmartScreen'in "yaygin
  // olarak indirilen bir dosya degil" uyarisiyla sonuclanan GERCEK bir
  // indirme baslatir (kullanicinin farkli bir dosyayi -- orn. VLSS5.exe'yi --
  // pill DISINDA bir yere birakmasi durumunda basimiza gelen tam olarak bu).
  // Ozel bir surukle-birak hedefi (orn. Ana Sayfa'daki nvngx_dlssnr.dll
  // pill'i) KENDI preventDefault'unu zaten yapiyor; burasi sadece ELE
  // ALINMAYAN tum birakmalari sessizce yutan genel bir guvenlik agi.
  window.addEventListener("dragover", (e) => e.preventDefault());
  window.addEventListener("drop", (e) => e.preventDefault());

  // ---------------------------------------------------------------------
  // Pencere surukleme: WebView2, Electron'un aksine "-webkit-app-region:
  // drag" CSS'ini native pencere surumune otomatik BAGLAMAZ (sadece stil
  // olarak yorumlanir, OS'a hicbir sey bildirmez). Bu yuzden ".titlebar"
  // uzerine gercek bir surukleme yapabilmek icin klasik "sahte baslik
  // cubugu" numarasini kullaniyoruz: mousedown'da native'e "__drag__"
  // komutu gonderiyoruz, WebViewHost bunu ReleaseCapture() + WM_NCLBUTTONDOWN
  // (HTCAPTION) ile pencereyi OS'un kendi surukleme mekanizmasina devrederek
  // isliyor. ".titlebar-btns" (kapat vb.) icindeki tiklamalar haric tutulur.
  // ---------------------------------------------------------------------
  document.querySelectorAll(".titlebar").forEach((bar) => {
    bar.addEventListener("mousedown", (e) => {
      if (e.button !== 0) return;
      if (e.target.closest(".titlebar-btns")) return;
      send("__drag__");
    });
  });
})();
