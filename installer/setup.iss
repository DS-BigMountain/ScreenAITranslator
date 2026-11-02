#define AppName "你给我翻译翻译什么叫翻译"
#define AppExeName AppName + ".exe"
#ifndef AppVersion
  #define AppVersion "1.6.0"
#endif
#ifndef AppStage
  #define AppStage "..\build\package-stage\Release"
#endif
#ifndef PackageOutput
  #error PackageOutput must identify a new delivery directory supplied by scripts/build.ps1
#endif
#ifndef PackageName
  #define PackageName AppName + "-Setup-V" + AppVersion
#endif
#ifndef AppIconName
  #error AppIconName must identify the icon included in AppStage
#endif

[Setup]
AppId={{2B5E7B5E-E962-42F0-A42D-6A5843E925FA}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} V{#AppVersion}
AppPublisher={#AppName}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#PackageOutput}
OutputBaseFilename={#PackageName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppIconName}
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName}
VersionInfoProductName={#AppName}
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
ShowLanguageDialog=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"

[Tasks]
Name: "startup"; Description: "登录 Windows 后在后台启动"; GroupDescription: "启动选项："
Name: "desktopicon"; Description: "创建桌面快捷方式"; Flags: unchecked

[Files]
Source: "{#AppStage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: files; Name: "{app}\ScreenAITranslator.exe"
Type: files; Name: "{autoprograms}\Screen AI Translator\Screen AI Translator.lnk"
Type: files; Name: "{autoprograms}\Screen AI Translator\卸载屏幕翻译.lnk"
Type: dirifempty; Name: "{autoprograms}\Screen AI Translator"
Type: files; Name: "{autodesktop}\Screen AI Translator.lnk"

[Icons]
Name: "{autoprograms}\{#AppName}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppIconName}"; AppUserModelID: "ScreenAITranslator.Desktop"
Name: "{autoprograms}\{#AppName}\卸载{#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon; IconFilename: "{app}\{#AppIconName}"; AppUserModelID: "ScreenAITranslator.Desktop"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ScreenAITranslator"; ValueData: """{app}\{#AppExeName}"" --background"; Tasks: startup; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "ScreenAITranslator"; Tasks: not startup; Flags: deletevalue

[Run]
Filename: "{app}\{#AppExeName}"; Description: "打开{#AppName}"; Flags: nowait postinstall skipifsilent

; User settings and DPAPI ciphertext live outside {app}.
; Uninstall preserves user data, including untouched legacy files, and never launches the program.
