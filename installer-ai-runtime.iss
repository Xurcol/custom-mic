#define MyAppName "Custom Mic AI Voice Runtime"
#define MyAppVersion "1.0.0"
#define RuntimeSource "C:\Users\xurco\Desktop\Voice changer"

[Setup]
AppId={{D4ED7584-2F19-4B64-8E3B-8B69EDDC9A21}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=xurco
DefaultDirName={commonappdata}\Custom Mic\ai-voice-runtime
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename=Custom Mic AI Voice Runtime Setup 1.0.0
Compression=lzma2
SolidCompression=yes
LZMAUseSeparateProcess=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
WizardStyle=modern
SetupIconFile=icon.ico
UninstallDisplayName=Custom Mic AI Voice Runtime

[Files]
Source: "{#RuntimeSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "voice-changer-native-client.exe,vcclient.log,*.lib,torch\lib\*train64_8.dll,torch\bin\*.exe"

[Icons]
