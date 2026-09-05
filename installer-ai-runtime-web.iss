#define MyAppName "Custom Mic AI Voice Runtime Web Setup"
#define MyAppVersion "1.0.0"
#define RuntimeInstallerUrl "https://drive.usercontent.google.com/download?id=1Mx9Fs2TaelDi2u2sdoX31thj68oRCkdI&export=download&confirm=t"
#define RuntimeInstallerFile "Custom Mic AI Voice Runtime Setup 1.0.0.exe"

[Setup]
AppId={{197C1F0E-3788-49BA-A29D-33A65C8D6CB2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=xurco
DefaultDirName={commonappdata}\Custom Mic
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename=Custom Mic AI Voice Runtime Web Setup 1.0.0
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
WizardStyle=modern
SetupIconFile=icon.ico
Uninstallable=no

[Code]
var
  DownloadPage: TDownloadWizardPage;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Downloading AI Voice Runtime', 'Custom Mic is downloading the full AI voice runtime. This can take a while.', nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Error: String;
  InstallerPath: String;
  ResultCode: Integer;
begin
  Result := True;
  if CurPageID = wpReady then begin
    DownloadPage.Clear;
    DownloadPage.Add('{#RuntimeInstallerUrl}', '{#RuntimeInstallerFile}', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        InstallerPath := ExpandConstant('{tmp}\{#RuntimeInstallerFile}');
        if not Exec(InstallerPath, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin
          SuppressibleMsgBox('Could not start the AI voice runtime installer.', mbCriticalError, MB_OK, IDOK);
          Result := False;
        end else if ResultCode <> 0 then begin
          SuppressibleMsgBox('AI voice runtime installer failed with code ' + IntToStr(ResultCode) + '.', mbCriticalError, MB_OK, IDOK);
          Result := False;
        end;
      except
        if DownloadPage.AbortedByUser then
          Log('Runtime download aborted by user.')
        else begin
          Error := Format('%s: %s', [DownloadPage.LastBaseNameOrUrl, GetExceptionMessage]);
          SuppressibleMsgBox(AddPeriod(Error), mbCriticalError, MB_OK, IDOK);
        end;
        Result := False;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;
