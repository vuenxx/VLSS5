; ---------------------------------------------------------------------------
; VLSS5 Inno Setup script
;
; Derleme (yerelde, Inno Setup kurulu ise):
;   ISCC.exe installer\VLSS5.iss /DMyAppVersion=0.5.1
;
; CI bunu otomatik yapar (bkz. .github/workflows/release.yml) -- bir "v*"
; git tag'i push edildiginde MSBuild ile Release|x64 derlenir, sonra bu
; script tag'ten cikarilan surumle derlenip Setup.exe GitHub Release'e
; asset olarak eklenir. src/UpdateChecker.cpp bu asset'i otomatik bulup
; sessizce calistirir (bkz. Main.cpp LaunchSilentInstallAndExit) -- yani
; buradaki [Setup] CloseApplications/RestartApplications ayarlari VLSS5'in
; kendi kendini guncelleme akisinin TEMELI.
;
; NOT: Bu script ONCEDEN derlenmis bir cikti klasorunu (varsayilan
; ..\x64\Release) paketler, kendisi derleme yapmaz.
; ---------------------------------------------------------------------------

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\x64\Release"
#endif

#define MyAppName "VLSS5"
#define MyAppPublisher "Berkay Kucukbekar"
#define MyAppURL "https://github.com/vuenxx/VLSS5"
#define MyAppExeName "VLSS5.exe"

[Setup]
; sabit AppId: guncellemelerin AYNI kurulumu tanimasi icin ASLA degistirilmemeli.
AppId={{B4C1F7D0-3A6E-4F2C-9C0E-6E1E3F1B9C7A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=VLSS5-Setup-{#MyAppVersion}
SetupIconFile=..\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; VLSS5.exe zaten kendi manifestinde RequireAdministrator istiyor (bkz.
; VLSS5.vcxproj) -- installer da ayni yetkiyle calismali.
PrivilegesRequired=admin

; ---------------------------------------------------------------------------
; Sessiz OTOMATIK GUNCELLEME akisinin can damari: VLSS5.exe calisirken bu
; installer /VERYSILENT ile baslatilirsa, Windows Restart Manager calisan
; VLSS5.exe'yi KENDISI kapatir (dosyalarin uzerine yazilabilmesi icin),
; kurulum bitince OTOMATIK olarak yeniden baslatir -- ayri bir batch/
; zamanlayici script YAZMAMIZA gerek birakmiyor. AppMutex, RM'den once ek
; bir klasik kontrol (Main.cpp'deki TEK-ORNEK mutex adiyla birebir ayni).
; ---------------------------------------------------------------------------
AppMutex=Local\VLSS5_SingleInstance_Mutex
CloseApplications=yes
CloseApplicationsFilter={#MyAppExeName}
RestartApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs
; Kucuk (114KB), dagitilabilir yardimci DLL (bkz. redist/README.md) --
; asil (>100MB, telifli) NGX model agirliklari BURAYA DAHIL DEGIL, kullanici
; tarafindan ayrica temin edilmesi gerekiyor (bkz. Main.cpp CheckRequiredFiles).
Source: "..\redist\nvngx.dll_dlssnr.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\beni oku.txt"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; skipifsilent: sadece INTERAKTIF/ilk kurulumda "Programi Baslat" secenegi
; gosterir. Sessiz GUNCELLEMELERDE bunun yerine yukaridaki RestartApplications
; (Restart Manager) devreye girer -- CloseApplications'in kapattigi VLSS5.exe'yi
; otomatik yeniden baslatir, boylece burada IKINCI bir baslatma OLMAZ.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
