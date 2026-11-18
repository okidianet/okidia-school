; =====================================================================
; OKIDIA Windows Master Installer Script (Inno Setup 6+)
; =====================================================================

#define MyAppName "OKIDIA Master"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "OKIDIA"
#define MyAppURL "https://okidia.net"
#define MyAppExeName "okidia-master.exe"

[Setup]
; Temel Kurulum Bilgileri
AppId={{C7E8D3A1-7B2F-4F3E-8D2A-9B3C4D5E6F70}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany=OKIDIA
VersionInfoDescription=OKIDIA Master Setup
VersionInfoTextVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\Okidia\Master
UsePreviousAppDir=no
DirExistsWarning=no
DefaultGroupName=OKIDIA
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE.txt
OutputDir=..\..\build\dist\windows
OutputBaseFilename=okidia-master-setup-v{#MyAppVersion}
SetupIconFile=..\..\resources\icons\master\okidia-master.ico
UninstallDisplayIcon={app}\resources\icons\master\okidia-master.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=force
RestartApplications=no
UpdateUninstallLogAppName=yes

[Languages]
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
turkish.PurgeDataPrompt=OKIDIA Master kaldırılırken yerel kullanıcı profilleri, sertifikalar ve yerel veritabanı (okidia.db) kayıtları tamamen silinsin mi?%n%n[Evet]: Tüm verileri ve profilleri tamamen sil.%n[Hayır]: Veritabanı ve sertifikaları sonraki kurulumlar için koru.
english.PurgeDataPrompt=Do you want to completely remove local user profiles, certificates, and local database (okidia.db) records?%n%n[Yes]: Completely remove all data and profiles.%n[No]: Keep databases and certificates for future installations.

[Dirs]
Name: "{app}\bin"
Name: "{app}\resources"
Name: "{app}\resources\icons"
Name: "{app}\resources\icons\master"

[Files]
; Ana İkili Dosyalar ve Tüm Bağımlılıklar (Qt6, MinGW, OpenSSL, libssh2, Pluginler)
Source: "..\..\build\windows\staging\master\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs

; İkonlar
Source: "..\..\resources\icons\master\okidia-master.ico"; DestDir: "{app}\resources\icons\master"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\OKIDIA\OKIDIA Master"; Filename: "{app}\bin\{#MyAppExeName}"; IconFilename: "{app}\resources\icons\master\okidia-master.ico"
Name: "{autodesktop}\OKIDIA Master"; Filename: "{app}\bin\{#MyAppExeName}"; IconFilename: "{app}\resources\icons\master\okidia-master.ico"

[Registry]
; Kullanıcıya ait Master ayarlarını (kullanıcı adı, oturum, tema vb.) kaldırma sırasında temizle
Root: HKCU; Subkey: "Software\okidia\master"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\okidia"; Flags: uninsdeletekeyifempty

[Run]
; 1. Windows Güvenlik Duvarı Kurallarını Ekle (mDNS ve Ağ İletişimi)
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Master App"" dir=in action=allow program=""{app}\bin\{#MyAppExeName}"" enable=yes profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Master App Out"" dir=out action=allow program=""{app}\bin\{#MyAppExeName}"" enable=yes profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Master mDNS In"" dir=in action=allow protocol=UDP localport=5353 profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Master mDNS Out"" dir=out action=allow protocol=UDP remoteport=5353 profile=any"; Flags: runhidden

; 2. Kurulum Sonunda Başlatma Seçeneği (Etkileşimli Sihirbaz Kurulumu)
Filename: "{app}\bin\{#MyAppExeName}"; Description: "{cm:LaunchProgram,OKIDIA Master}"; Flags: nowait postinstall skipifsilent runasoriginaluser
; 3. Sessiz (OTA) Güncelleme Sonunda Otomatik Yeniden Başlatma
Filename: "{app}\bin\{#MyAppExeName}"; Flags: nowait skipifnotsilent

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\Temp\Okidia\Master"
Type: files; Name: "{localappdata}\Temp\okidia-master.pid"
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\resources"
Type: filesandordirs; Name: "{app}"

[Code]
var
  ShouldPurgeData: Boolean;

function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  // Çalışan master süreçlerini sonlandır
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := True;
end;

function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  // Kaldırma başlar başlamaz master sürecini kapat
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1000);

  // Kullanıcıya profilleri, veritabanlarını ve sertifikaları silmek isteyip istemediğini sor
  if MsgBox(CustomMessage('PurgeDataPrompt'), mbConfirmation, MB_YESNO) = IDYES then
    ShouldPurgeData := True
  else
    ShouldPurgeData := False;

  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppPath, MasterLocalDataPath, MasterCommonDataPath, ParentLocalDataPath, ParentCommonDataPath, ParentAppPath: String;
  TempPidFile, TempRunPath: String;
  ResultCode: Integer;
  i: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    // 1. Master sürecini zorla kapat
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1500);

    // 2. Güvenlik Duvarı kurallarını sil
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Master App"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Master App Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Master mDNS In"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Master mDNS Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    AppPath := ExpandConstant('{app}');
    MasterLocalDataPath := ExpandConstant('{localappdata}\Okidia\Master');
    ParentLocalDataPath := ExpandConstant('{localappdata}\Okidia');
    MasterCommonDataPath := ExpandConstant('{commonappdata}\Okidia\Master');
    ParentCommonDataPath := ExpandConstant('{commonappdata}\Okidia');
    ParentAppPath := ExpandConstant('{autopf}\Okidia');
    TempPidFile := ExpandConstant('{localappdata}\Temp\okidia-master.pid');
    TempRunPath := ExpandConstant('{localappdata}\Temp\Okidia\Master');
    
    // Sürecin kapalı olduğundan emin ol
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);

    // 1. Kendi uygulama dizinini temizle ({autopf}\Okidia\Master)
    if DirExists(AppPath) then
    begin
      Exec(ExpandConstant('{cmd}'), '/C rmdir /s /q "' + AppPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      for i := 1 to 5 do
      begin
        DelTree(AppPath, True, True, True);
        if not DirExists(AppPath) then Break;
        Sleep(500);
      end;
    end;

    // 2. Kullanıcı veri temizliğini onayladıysa profilleri, sertifikaları ve veritabanlarını sil
    if ShouldPurgeData then
    begin
      // Aktif kullanıcının yerel veri dizinini tamamen temizle ({localappdata}\Okidia\Master: certs, db, logs)
      if DirExists(MasterLocalDataPath) then
      begin
        Exec(ExpandConstant('{cmd}'), '/C rmdir /s /q "' + MasterLocalDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
        for i := 1 to 5 do
        begin
          DelTree(MasterLocalDataPath, True, True, True);
          if not DirExists(MasterLocalDataPath) then Break;
          Sleep(500);
        end;
      end;

      // Çok kullanıcılı sistemlerde diğer kullanıcıların AppData altındaki Master dizinlerini de temizle
      Exec(ExpandConstant('{cmd}'), '/C for /d %u in ("C:\Users\*") do if exist "%u\AppData\Local\Okidia\Master" rmdir /s /q "%u\AppData\Local\Okidia\Master"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

      // Eski sürümlerden kalma commonappdata varsa temizle ({commonappdata}\Okidia\Master)
      if DirExists(MasterCommonDataPath) then
      begin
        Exec(ExpandConstant('{cmd}'), '/C rmdir /s /q "' + MasterCommonDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
        DelTree(MasterCommonDataPath, True, True, True);
      end;

      // Registry kayıtlarını temizle
      RegDeleteKeyIncludingSubkeys(HKCU, 'Software\okidia\master');
      RegDeleteKeyIfEmpty(HKCU, 'Software\okidia');
    end;

    // 6. Registry kayıtlarını garanti olarak temizle
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\okidia\master');
    RegDeleteKeyIfEmpty(HKCU, 'Software\okidia');

    // 7. Eğer üst dizinler boşsa ({localappdata}\Okidia, {commonappdata}\Okidia, {autopf}\Okidia) kaldır
    Exec(ExpandConstant('{cmd}'), '/C rmdir "' + ParentLocalDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{cmd}'), '/C rmdir "' + ParentCommonDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{cmd}'), '/C rmdir "' + ParentAppPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
