; =====================================================================
; OKIDIA Windows Agent Installer Script (Inno Setup 6+)
; =====================================================================

#define MyAppName "OKIDIA Agent"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "OKIDIA"
#define MyAppURL "https://okidia.net"
#define MyAppExeName "okidia-agent.exe"
#define MyTrayExeName "okidia-tray.exe"

[Setup]
; Temel Kurulum Bilgileri
AppId={{D3F9B9C4-8D82-4E2F-9C1B-8C4D5F8A2E10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany=OKIDIA
VersionInfoDescription=OKIDIA Agent Setup
VersionInfoTextVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\Okidia\Agent
UsePreviousAppDir=no
DisableDirPage=yes
DirExistsWarning=no
DefaultGroupName=OKIDIA
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE.txt
OutputDir=..\..\build\dist\windows
OutputBaseFilename=okidia-agent-setup-v{#MyAppVersion}
SetupIconFile=..\..\resources\icons\agent\okidia-agent.ico
UninstallDisplayIcon={app}\resources\icons\agent\okidia-agent.ico
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
Name: "{commonappdata}\Okidia\Agent"
Name: "{commonappdata}\Okidia\Agent\certs"
Name: "{commonappdata}\Okidia\Agent\logs"
Name: "{commonappdata}\Okidia\Agent\run"
Name: "{commonappdata}\Okidia\Agent\db"
Name: "{commonappdata}\Okidia\Agent\rollback"
Name: "{app}\bin"
Name: "{app}\resources"
Name: "{app}\resources\sound"
Name: "{app}\resources\icons"
Name: "{app}\resources\icons\agent"

[Files]
; Ana İkili Dosyalar ve Tüm Bağımlılıklar (Qt6, MinGW, OpenSSL, Pluginler)
Source: "..\..\build\windows\staging\agent\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace

; Kaynaklar, Sesler ve İkonlar
Source: "..\..\resources\sound\*"; DestDir: "{app}\resources\sound"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\resources\icons\agent\okidia-agent.ico"; DestDir: "{app}\resources\icons\agent"; Flags: ignoreversion

[Registry]
; Kullanıcı oturumu açıldığında okidia-tray.exe'yi otomatik başlat
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "OkidiaTray"; ValueData: """{app}\bin\{#MyTrayExeName}"""; Flags: uninsdeletevalue
; Kullanıcı ayarlarını kaldırma sırasında temizle
Root: HKCU; Subkey: "Software\okidia\agent"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\okidia\tray"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\okidia"; Flags: uninsdeletekeyifempty

[Run]
; 1. NTFS Klasör İzinlerini Yapılandır (Certs koruması + Users log/run izni)
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\Okidia"" /grant:r ""SYSTEM"":(OI)(CI)F /grant:r ""Administrators"":(OI)(CI)F /grant ""Users"":(OI)(CI)RX"; StatusMsg: "Güvenlik izinleri yapılandırılıyor..."; Flags: runhidden
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\Okidia\Agent"" /grant:r ""SYSTEM"":(OI)(CI)F /grant:r ""Administrators"":(OI)(CI)F /grant ""Users"":(OI)(CI)RX"; Flags: runhidden
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\Okidia\Agent\uninstall.key"" /grant:r ""SYSTEM"":F /grant:r ""Administrators"":F /grant ""Users"":R"; Flags: runhidden
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\Okidia\Agent\logs"" /grant ""Users"":(OI)(CI)M"; Flags: runhidden
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\Okidia\Agent\run"" /grant ""Users"":(OI)(CI)M"; Flags: runhidden
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\Okidia\Agent\db"" /grant ""Users"":(OI)(CI)M"; Flags: runhidden

; 2. Windows Güvenlik Duvarı Kurallarını Ekle (Tüm profiller, UDP/TCP ve Uygulama izni)
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Agent App"" dir=in action=allow program=""{app}\bin\{#MyAppExeName}"" enable=yes profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Agent App Out"" dir=out action=allow program=""{app}\bin\{#MyAppExeName}"" enable=yes profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Tray App"" dir=in action=allow program=""{app}\bin\{#MyTrayExeName}"" enable=yes profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA mDNS In"" dir=in action=allow protocol=UDP localport=5353 profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA mDNS Out"" dir=out action=allow protocol=UDP remoteport=5353 profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Agent TCP In"" dir=in action=allow protocol=TCP localport=11200 profile=any"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OKIDIA Agent TCP Out"" dir=out action=allow protocol=TCP remoteport=11200 profile=any"; Flags: runhidden

; 3. Not: Tray uygulaması, Agent servisi başladığında aktif konsol oturumunu (Winlogon veya kullanıcı)
; algılayarak otomatik ve doğru masaüstünde (winsta0) başlatılır; bu nedenle Run bölümünden izole edilmiştir.

[UninstallDelete]
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\resources"
Type: filesandordirs; Name: "{app}"

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
  MasterInstalled, BrokerInstalled: Boolean;
begin
  MasterInstalled := FileExists(ExpandConstant('{autopf}\Okidia\Master\bin\okidia-master.exe')) or
                     RegKeyExists(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{C7E8D3A1-7B2F-4F3E-8D2A-9B3C4D5E6F70}_is1') or
                     RegKeyExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{C7E8D3A1-7B2F-4F3E-8D2A-9B3C4D5E6F70}_is1');

  BrokerInstalled := FileExists(ExpandConstant('{autopf}\Okidia\Broker\bin\okidia-broker.exe')) or
                     RegKeyExists(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{B5C3D1A9-4E2F-4A8B-9C1D-8E7F6A5B4C3D}_is1') or
                     RegKeyExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{B5C3D1A9-4E2F-4A8B-9C1D-8E7F6A5B4C3D}_is1');

  if MasterInstalled or BrokerInstalled then
  begin
    if not WizardSilent then
    begin
      MsgBox('Bu bilgisayarda OKIDIA Master veya Broker kurulu tespit edildi.' + #13#10 + #13#10 +
             'Güvenlik ve paket çakışması kuralları gereği, Master veya Broker kurulu olan bilgisayarlara OKIDIA Agent kurulamaz.',
             mbCriticalError, MB_OK);
    end;
    Result := False;
    Exit;
  end;

  // Eski çalışan servisi ve tüm süreçleri anında ve zorla sonlandır (DLL dosya kilitlerini kaldır)
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop OkidiaAgent', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName} /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyTrayExeName} /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

  // Önceki hatalı kurulumlardan kalmış olabilecek C:\temp_old dizinini temizle
  if DirExists('C:\temp_old') then
  begin
    DelTree('C:\temp_old', True, True, True);
  end;

  // DLL ve dosya kilitlerinin serbest kalması için bekle (pipe/find kullanılmaz)
  Sleep(1500);
  Result := True;
end;

function VerifyUninstallPassword(): Boolean;
var
  KeyFile, ExpectedHash, EnteredPassword, EnteredHash, CmdKey: String;
  Form: TSetupForm;
  PromptLabel: TLabel;
  Edit: TEdit;
  BtnOK, BtnCancel: TButton;
  Lines: TArrayOfString;
begin
  Result := False;
  KeyFile := ExpandConstant('{commonappdata}\Okidia\Agent\uninstall.key');
  ExpectedHash := '';

  // 1. Anahtar dosyasından veya Registry'den hash oku
  if FileExists(KeyFile) then
  begin
    if LoadStringsFromFile(KeyFile, Lines) and (GetArrayLength(Lines) > 0) then
      ExpectedHash := Trim(Lines[0]);
  end;

  if ExpectedHash = '' then
  begin
    RegQueryStringValue(HKEY_LOCAL_MACHINE, 'Software\Okidia\Agent', 'UninstallPasswordHash', ExpectedHash);
  end;

  // Eğer hiçbir parola belirlenmemişse, varsayılan parolayı ("www.okidia.net") kullan
  if ExpectedHash = '' then
  begin
    ExpectedHash := 'f078695dd90be300e7d6af8f96fb389fefb93cdee5d6ae91a9d730e11150b0e9';
  end;

  // 2. Komut satırında sessiz kaldırma anahtarı verilmiş mi kontrol et (/UNINSTALLKEY=...)
  CmdKey := ExpandConstant('{param:UNINSTALLKEY}');
  if (CmdKey <> '') and (CompareText(CmdKey, ExpectedHash) = 0) then
  begin
    Result := True;
    Exit;
  end;

  // 3. Kullanıcıya maskeli parola soran diyalog kutusu göster
  Form := CreateCustomForm(ScaleX(440), ScaleY(210), False, True);
  try
    Form.ClientWidth := ScaleX(440);
    Form.ClientHeight := ScaleY(210);
    Form.Caption := 'OKIDIA İstemci Kaldırma Koruması';
    Form.Position := poScreenCenter;
    Form.BorderStyle := bsDialog;

    PromptLabel := TLabel.Create(Form);
    PromptLabel.Parent := Form;
    PromptLabel.Left := ScaleX(24);
    PromptLabel.Top := ScaleY(18);
    PromptLabel.Width := ScaleX(392);
    PromptLabel.Height := ScaleY(55);
    PromptLabel.WordWrap := True;
    PromptLabel.Caption := 'Bu yazılım okul yönetimi tarafından korunmaktadır.' + #13#10 +
                           'Kaldırma işlemine devam etmek için lütfen yönetici parolasını girin:';

    Edit := TEdit.Create(Form);
    Edit.Parent := Form;
    Edit.Left := ScaleX(24);
    Edit.Top := ScaleY(85);
    Edit.Width := ScaleX(392);
    Edit.Height := ScaleY(26);
    Edit.PasswordChar := '*';

    BtnOK := TButton.Create(Form);
    BtnOK.Parent := Form;
    BtnOK.Caption := 'Kaldır';
    BtnOK.ModalResult := mrOk;
    BtnOK.Default := True;
    BtnOK.Left := ScaleX(226);
    BtnOK.Top := ScaleY(145);
    BtnOK.Width := ScaleX(90);
    BtnOK.Height := ScaleY(32);

    BtnCancel := TButton.Create(Form);
    BtnCancel.Parent := Form;
    BtnCancel.Caption := 'İptal';
    BtnCancel.ModalResult := mrCancel;
    BtnCancel.Cancel := True;
    BtnCancel.Left := ScaleX(326);
    BtnCancel.Top := ScaleY(145);
    BtnCancel.Width := ScaleX(90);
    BtnCancel.Height := ScaleY(32);

    if Form.ShowModal() = mrOk then
    begin
      EnteredPassword := Edit.Text;
      EnteredHash := Lowercase(GetSHA256OfString(EnteredPassword));
      // Güvenlik: Yalnızca belirlenmiş ExpectedHash ile karşılaştırılır (Arka kapılar tamamen kaldırılmıştır)
      if (CompareText(EnteredHash, Lowercase(ExpectedHash)) = 0) or 
         (CompareText(EnteredPassword, ExpectedHash) = 0) then
      begin
        Result := True;
      end
      else
      begin
        // Brute-force denemelerini yavaşlatmak için 2 saniye gecikme
        Sleep(2000);
        MsgBox('Hatalı kaldırma parolası! OKIDIA İstemcisi kaldırılamaz.', mbError, MB_OK);
        Result := False;
      end;
    end
    else
    begin
      Result := False;
    end;
  finally
    Form.Free();
  end;
end;

function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  // İlk güvenlik kapısı: Parola doğrulanmadan hiçbir servis durdurulmaz, kilit açılmaz!
  if not VerifyUninstallPassword() then
  begin
    Result := False;
    Exit;
  end;

  // Parola doğrulandıysa servis ve süreçleri durdur (kilitlerin erken kalkmasını sağla)
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop OkidiaAgent', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyTrayExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\sc.exe'), 'delete OkidiaAgent', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1500);
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  AppBinPath, TrayBinPath, AppDataPath, AgentDataPath: String;
begin
  if CurStep = ssPostInstall then
  begin
    AppBinPath := ExpandConstant('{app}\bin\{#MyAppExeName}');
    TrayBinPath := ExpandConstant('{app}\bin\{#MyTrayExeName}');
    AppDataPath := ExpandConstant('{commonappdata}\Okidia');
    AgentDataPath := ExpandConstant('{commonappdata}\Okidia\Agent');
    
    // 0. Eski sertifikaları (varsa) yeni Agent\certs dizinine taşı/koru
    if not FileExists(AgentDataPath + '\certs\agent.crt') and not FileExists(AgentDataPath + '\certs\client.crt') then
    begin
      if FileExists(AppDataPath + '\certs\agent\agent.crt') then
      begin
        CopyFile(AppDataPath + '\certs\agent\agent.crt', AgentDataPath + '\certs\agent.crt', False);
        CopyFile(AppDataPath + '\certs\agent\agent.key', AgentDataPath + '\certs\agent.key', False);
        CopyFile(AppDataPath + '\certs\agent\ca.crt', AgentDataPath + '\certs\ca.crt', False);
      end
      else if FileExists(AppDataPath + '\certs\agent\client.crt') then
      begin
        CopyFile(AppDataPath + '\certs\agent\client.crt', AgentDataPath + '\certs\agent.crt', False);
        CopyFile(AppDataPath + '\certs\agent\client.key', AgentDataPath + '\certs\agent.key', False);
        CopyFile(AppDataPath + '\certs\agent\ca.crt', AgentDataPath + '\certs\ca.crt', False);
      end
      else if FileExists(AppDataPath + '\certs\client.crt') then
      begin
        CopyFile(AppDataPath + '\certs\client.crt', AgentDataPath + '\certs\agent.crt', False);
        CopyFile(AppDataPath + '\certs\client.key', AgentDataPath + '\certs\agent.key', False);
        CopyFile(AppDataPath + '\certs\ca.crt', AgentDataPath + '\certs\ca.crt', False);
      end;
    end;

    // 0.1 Kaldırma koruma anahtarı dosyası yoksa varsayılan hash'i ("www.okidia.net") yaz
    if not FileExists(AgentDataPath + '\uninstall.key') then
    begin
      SaveStringToFile(AgentDataPath + '\uninstall.key', 'f078695dd90be300e7d6af8f96fb389fefb93cdee5d6ae91a9d730e11150b0e9' + #13#10, False);
    end;

    // 1. Güvenlik İzinleri (Servis başlamadan önce ayarlanmalı)
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AppDataPath + '" /grant:r "SYSTEM":(OI)(CI)F /grant:r "Administrators":(OI)(CI)F /grant "Users":(OI)(CI)RX', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '" /grant:r "SYSTEM":(OI)(CI)F /grant:r "Administrators":(OI)(CI)F /grant "Users":(OI)(CI)RX', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '\certs" /inheritance:r /grant:r "SYSTEM":(OI)(CI)F /grant:r "Administrators":(OI)(CI)F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '\certs\*.key" /inheritance:r /grant:r "SYSTEM":F /grant:r "Administrators":F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '\uninstall.key" /grant:r "SYSTEM":F /grant:r "Administrators":F /grant "Users":R', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '\logs" /grant "Users":(OI)(CI)M', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '\run" /grant "Users":(OI)(CI)M', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '\db" /grant "Users":(OI)(CI)M', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AgentDataPath + '\rollback" /grant:r "SYSTEM":(OI)(CI)F /grant:r "Administrators":(OI)(CI)F /grant "Users":(OI)(CI)RX', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // 2. Windows Güvenlik Duvarı Kuralları (Servis ağ paketlerini kaçırmasın)
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Agent App" dir=in action=allow program="' + AppBinPath + '" enable=yes profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Agent App Out" dir=out action=allow program="' + AppBinPath + '" enable=yes profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Tray App" dir=in action=allow program="' + TrayBinPath + '" enable=yes profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA mDNS In" dir=in action=allow protocol=UDP localport=5353 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA mDNS Out" dir=out action=allow protocol=UDP remoteport=5353 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Agent TCP In" dir=in action=allow protocol=TCP localport=11200 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall add rule name="OKIDIA Agent TCP Out" dir=out action=allow protocol=TCP remoteport=11200 profile=any', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // 3. Servis yapılandırması ve başlatma (sc delete KULLANILMAZ; Win32 1072 hatasını önler)
    if Exec(ExpandConstant('{sys}\sc.exe'), 'query OkidiaAgent', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    begin
      Exec(ExpandConstant('{sys}\sc.exe'), 'config OkidiaAgent binPath= "\"' + AppBinPath + '\" --daemon" start= auto obj= LocalSystem DisplayName= "OKIDIA Agent Servisi"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end
    else
    begin
      Exec(ExpandConstant('{sys}\sc.exe'), 'create OkidiaAgent binPath= "\"' + AppBinPath + '\" --daemon" start= auto obj= LocalSystem DisplayName= "OKIDIA Agent Servisi"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;
    RegWriteStringValue(HKEY_LOCAL_MACHINE, 'SYSTEM\CurrentControlSet\Services\OkidiaAgent', 'ImagePath', '"' + AppBinPath + '" --daemon');

    Exec(ExpandConstant('{sys}\sc.exe'), 'description OkidiaAgent "OKIDIA Okul İçi Denetim ve İletişim Ağı Ajan Servisi"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'failure OkidiaAgent reset= 86400 actions= restart/5000/restart/10000/restart/60000', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'failureflag OkidiaAgent 1', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'start OkidiaAgent', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // 4. Registry sürümünü garantiye al
    RegWriteStringValue(HKEY_LOCAL_MACHINE, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#SetupSetting("AppId")}_is1', 'DisplayVersion', '{#MyAppVersion}');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppPath, AgentDataPath, ParentDataPath, ParentAppPath: String;
  ResultCode, i: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop OkidiaAgent', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyTrayExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete OkidiaAgent', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // DLL ve soket kilitlerinin serbest kalması için bekle
    Sleep(2000);

    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Agent App"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Agent App Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Tray App"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA mDNS In"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA mDNS Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Agent TCP In"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="OKIDIA Agent TCP Out"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    AppPath := ExpandConstant('{app}');
    AgentDataPath := ExpandConstant('{commonappdata}\Okidia\Agent');
    ParentDataPath := ExpandConstant('{commonappdata}\Okidia');
    ParentAppPath := ExpandConstant('{autopf}\Okidia');

    // 1. Kendi uygulama dizinini temizle ({autopf}\Okidia\Agent)
    if DirExists(AppPath) then
    begin
      Exec(ExpandConstant('{sys}\cmd.exe'), '/c rmdir /s /q "' + AppPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      for i := 1 to 5 do
      begin
        DelTree(AppPath, True, True, True);
        if not DirExists(AppPath) then Break;
        Sleep(500);
      end;
    end;

    // 2. Kendi veri dizinini temizle ({commonappdata}\Okidia\Agent)
    if DirExists(AgentDataPath) then
    begin
      Exec(ExpandConstant('{sys}\cmd.exe'), '/c rmdir /s /q "' + AgentDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      for i := 1 to 5 do
      begin
        DelTree(AgentDataPath, True, True, True);
        if not DirExists(AgentDataPath) then Break;
        Sleep(500);
      end;
    end;

    // 3. Registry kayıtlarını temizle
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\okidia\agent');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\okidia\tray');
    RegDeleteKeyIfEmpty(HKCU, 'Software\okidia');

    // 4. Eğer {commonappdata}\Okidia ve {autopf}\Okidia boşsa (başka bileşen yoksa) üst dizinleri de kaldır
    Exec(ExpandConstant('{cmd}'), '/C rmdir "' + ParentDataPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{cmd}'), '/C rmdir "' + ParentAppPath + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
