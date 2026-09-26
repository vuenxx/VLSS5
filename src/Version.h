#pragma once

// VLSS5 surum numarasi -- su an derlenmis binary'de sadece bir sabit olarak
// tasinir (orn. ileride bir Hakkinda/crash-report ekranina koymak icin);
// uygulama kendi icinde bunu hicbir yerde okuyup GitHub'la karsilastirmiyor
// (eski otomatik guncelleme ozelligi kaldirildi).
//
// ELLE DOKUNMANIZA GEREK YOK: .github/workflows/release.yml, "git tag vX.Y.Z"
// ile push ettiginiz tag'in surumunu derlemeden ONCE src/VersionGenerated.h
// dosyasina (gitignore'da, repoya commit'lenmez) yazar. O dosya varsa
// asagidaki fallback yerine ONU kullanilir -- yani dagitilan .exe HER ZAMAN
// tag'le birebir ayni surumu tasir, elle senkronize etmeye gerek kalmaz.
//
// VersionGenerated.h yoksa (Visual Studio'dan yerel/elle derleme) asagidaki
// sabit kullanilir -- bu sadece bir GELISTIRME fallback'i, dagitilan surumu
// ETKILEMEZ.
#if __has_include("VersionGenerated.h")
#include "VersionGenerated.h"
#else
#define VLSS5_VERSION_STRING L"0.5.1"
#endif
