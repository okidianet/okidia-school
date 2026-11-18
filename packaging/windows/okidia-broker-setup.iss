; =====================================================================
; OKIDIA Windows Broker Installer Script (Inno Setup 6+)
; =====================================================================

#define MyAppName "OKIDIA Broker"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "OKIDIA"
#define MyAppURL "https://okidia.net"
#define MyAppExeName "okidia-broker.exe"
#define MyTrayExeName "okidia-broker-tray.exe"

[Setup]
; Temel Kurulum Bilgileri
AppId={{E1F2A3B4-5C6D-7E8F-9A0B-1C2D3E4F5A60}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany=OKIDIA
VersionInfoDescription=OKIDIA Broker Server Setup
VersionInfoTextVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\Okidia\Broker
DirExistsWarning=no
DefaultGroupName=OKIDIA
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE.txt
OutputDir=..\..\build\dist\windows
OutputBaseFilename=okidia-broker-setup-v{#MyAppVersion}
SetupIconFile=..\..\resources\icons\broker\okidia-broker.ico
UninstallDisplayIcon={app}\resources\icons\broker\okidia-broker.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=no
RestartApplications=no
UpdateUninstallLogAppName=yes

[Languages]
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Dirs]
Name: "{commonappdata}\Okidia"
Name: "{commonappdata}\Okidia\Broker"
Name: "{commonappdata}\Okidia\Broker\certs"
Name: "{commonappdata}\Okidia\Broker\logs"
Name: "{commonappdata}\Okidia\Broker\run"
Name: "{commonappdata}\Okidia\Broker\db"
Name: "{commonappdata}\Okidia\Broker\updates"
Name: "{app}\bin"
Name: "{app}\resources"
Name: "{app}\resources\icons"
Name: "{app}\resources\icons\broker"

[Files]
; Ana İkili Dosyalar ve Tüm Bağımlılıklar (Qt6, MinGW, OpenSSL, Pluginler)
Source: "..\..\build\windows\staging\broker\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace

; İkonlar
Source: "..\..\resources\icons\broker\okidia-broker.ico"; DestDir: "{app}\resources\icons\broker"; Flags: ignoreversion

[Registry]
; Kullanıcı oturumu açıldığında okidia-broker-tray.exe'yi otomatik başlat
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "OkidiaBrokerTray"; ValueData: """{app}\bin\{#MyTrayExeName}"""; Flags: uninsdeletevalue
; Kullanıcı ayarlarını kaldırma sırasında temizle
Root: HKCU; Subkey: "Software\okidia\broker"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\okidia\broker-tray"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\okidia"; Flags: uninsdeletekeyifempty

[Run]
; Kurulum tamamlandığında aktif kullanıcı oturumu için Tray uygulamasını kullanıcı yetkileriyle başlat
Filename: "{app}\bin\{#MyTrayExeName}"; Flags: nowait runasoriginaluser

[UninstallDelete]
Type: files; Name: "{userdesktop}\OKIDIA_Kurulum_Kodu.txt"
Type: files; Name: "{commondesktop}\OKIDIA_Kurulum_Kodu.txt"
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\resources"
Type: filesandordirs; Name: "{app}"

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  // Eski çalışan servisi ve tüm süreçleri sonlandır
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyTrayExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), '-NoProfile -Command "Stop-Process -Name okidia-broker, okidia-broker-tray -Force -ErrorAction SilentlyContinue"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1500);
  Result := True;
end;

function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  // Kaldırma başlar başlamaz servisi ve tray'i durdur
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\sc.exe'), 'delete OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyTrayExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1000);
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  AppBinPath, TrayBinPath, AppDataPath, BrokerDataPath: String;
  CodePath, AltCodePath, DesktopContent: String;
  SetupCode: AnsiString;
  ResultCode, i: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    AppBinPath := ExpandConstant('{app}\bin\{#MyAppExeName}');
    TrayBinPath := ExpandConstant('{app}\bin\{#MyTrayExeName}');
    AppDataPath := ExpandConstant('{commonappdata}\Okidia');
    BrokerDataPath := ExpandConstant('{commonappdata}\Okidia\Broker');

    // 1. NTFS Klasör İzinleri
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AppDataPath + '" /grant:r "SYSTEM":(OI)(CI)F /grant:r "Administrators":(OI)(CI)F /grant "Users":(OI)(CI)RX', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + BrokerDataPath + '" /grant:r "SYSTEM":(OI)(CI)F /grant:r "Administrators":(OI)(CI)F /grant "Users":(OI)(CI)RX', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + BrokerDataPath + '\logs" /grant "Users":(OI)(CI)M', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + BrokerDataPath + '\run" /grant "Users":(OI)(CI)M', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + BrokerDataPath + '\db" /grant "Users":(OI)(CI)M', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + BrokerDataPath + '\updates" /grant "Users":(OI)(CI)M', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // 2. Güvenlik Duvarı Kuralları
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Broker App" dir=in action=allow program="' + AppBinPath + '" enable=yes profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Broker App Out" dir=out action=allow program="' + AppBinPath + '" enable=yes profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Broker Tray App" dir=in action=allow program="' + TrayBinPath + '" enable=yes profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Broker mDNS In" dir=in action=allow protocol=UDP localport=5353 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Broker mDNS Out" dir=out action=allow protocol=UDP remoteport=5353 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Broker TCP In" dir=in action=allow protocol=TCP localport=11200 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Broker TCP Out" dir=out action=allow protocol=TCP remoteport=11200 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // 3. Servis Yapılandırması ve Başlatma (sc delete KULLANILMAZ; Win32 1072 hatasını önler)
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    if Exec(ExpandConstant('{sys}\sc.exe'), 'query OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    begin
      Exec(ExpandConstant('{sys}\sc.exe'), 'config OkidiaBroker binPath= "\"' + AppBinPath + '\"" start= auto obj= LocalSystem DisplayName= "OKIDIA Broker Service"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end
    else
    begin
      Exec(ExpandConstant('{sys}\sc.exe'), 'create OkidiaBroker binPath= "\"' + AppBinPath + '\"" start= auto obj= LocalSystem DisplayName= "OKIDIA Broker Service"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;

    Exec(ExpandConstant('{sys}\sc.exe'), 'description OkidiaBroker "OKIDIA Okul İçi Denetim ve İletişim Ağı Merkez Sunucu Servisi"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'failure OkidiaBroker reset= 60 actions= restart/5000/restart/10000/restart/60000', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'start OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // 5. Kurulum kodunun veritabanı tarafından üretilmesini bekle (en fazla 10 sn)
    CodePath := ExpandConstant('{commonappdata}\Okidia\Broker\db\setup_code.txt');
    AltCodePath := ExpandConstant('{commonappdata}\Okidia\Broker\setup_code.txt');
    for i := 1 to 20 do
    begin
      if FileExists(CodePath) or FileExists(AltCodePath) then Break;
      Sleep(500);
    end;

    if not FileExists(CodePath) and FileExists(AltCodePath) then
      CodePath := AltCodePath;

    if FileExists(CodePath) then
    begin
      if LoadStringFromFile(CodePath, SetupCode) then
      begin
        // Kullanıcının Masaüstüne dosyayı kaydet
        DesktopContent := 'OKIDIA Master ilk kurulumu icin gereken kod:' + #13#10 + #13#10 +
                          String(SetupCode) + #13#10 + #13#10 +
                          'Bu kod tek kullanimliktir ve Master kurulumu tamamlandiktan sonra gecersiz olacaktir.' + #13#10 +
                          'Master ilk kurulum sihirbazinda (SetupWizard) bu kodu girerek sunucuya baglanabilirsiniz.' + #13#10;
        SaveStringToFile(ExpandConstant('{userdesktop}\OKIDIA_Kurulum_Kodu.txt'), DesktopContent, False);

        if not WizardSilent then
        begin
          MsgBox('OKIDIA Broker Sunucusu başarıyla başlatıldı!' + #13#10 + #13#10 +
                 'İlk Yönetici (Master) Kurulum Kodunuz:' + #13#10 +
                 String(SetupCode) + #13#10 + #13#10 +
                 'Bu kodu Master ilk kurulumunda (SetupWizard) girerek yönetici hesabınızı oluşturabilirsiniz.' + #13#10 + #13#10 +
                 'Not: Kod ayrıca Masaüstünüze "OKIDIA_Kurulum_Kodu.txt" dosyası olarak da kaydedilmiştir.',
                 mbInformation, MB_OK);
        end;
      end;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppPath, BrokerDataPath, ParentDataPath, ParentAppPath: String;
  ResultCode: Integer;
  i: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    // 1. Servis ve süreçleri durdur/sil
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyTrayExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete OkidiaBroker', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(2000);

    // 2. Güvenlik Duvarı kurallarını sil
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Broker App"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Broker App Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Broker Tray App"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Broker mDNS In"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Broker mDNS Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Broker TCP In"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Broker TCP Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    AppPath := ExpandConstant('{app}');
    BrokerDataPath := ExpandConstant('{commonappdata}\Okidia\Broker');
    ParentDataPath := ExpandConstant('{commonappdata}\Okidia');
    ParentAppPath := ExpandConstant('{autopf}\Okidia');
    
    // Süreçlerin kapalı olduğundan emin ol
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyTrayExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);

    // 1. Kendi uygulama dizinini temizle ({autopf}\Okidia\Broker)
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

    // 2. Kendi veri dizinini temizle ({commonappdata}\Okidia\Broker)
    if DirExists(BrokerDataPath) then
    begin
      Exec(ExpandConstant('{cmd}'), '/C rmdir /s /q "' + BrokerDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      for i := 1 to 5 do
      begin
        DelTree(BrokerDataPath, True, True, True);
        if not DirExists(BrokerDataPath) then Break;
        Sleep(500);
      end;
    end;

    // 3. Masaüstüne çıkarılmış olan OKIDIA_Kurulum_Kodu.txt dosyalarını temizle
    DeleteFile(ExpandConstant('{userdesktop}\OKIDIA_Kurulum_Kodu.txt'));
    DeleteFile(ExpandConstant('{commondesktop}\OKIDIA_Kurulum_Kodu.txt'));
    Exec(ExpandConstant('{cmd}'), '/C for /d %u in ("C:\Users\*") do if exist "%u\Desktop\OKIDIA_Kurulum_Kodu.txt" del /f /q "%u\Desktop\OKIDIA_Kurulum_Kodu.txt"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // 4. Eski geriye dönük uyumluluk dizini kalıntılarını temizle ({commonappdata}\Okidia\certs)
    if DirExists(ExpandConstant('{commonappdata}\Okidia\certs')) then
    begin
      DelTree(ExpandConstant('{commonappdata}\Okidia\certs'), True, True, True);
    end;

    // 5. Registry kayıtlarını temizle
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\okidia\broker');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\okidia\broker-tray');
    RegDeleteKeyIfEmpty(HKCU, 'Software\okidia');

    // 6. Eğer {commonappdata}\Okidia ve {autopf}\Okidia boşsa (başka bileşen yoksa) üst dizinleri de kaldır
    Exec(ExpandConstant('{cmd}'), '/C rmdir "' + ParentDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{cmd}'), '/C rmdir "' + ParentAppPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
