#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#ifndef AppBinary
  #define AppBinary "..\build\Release\ScreenAITranslator.exe"
#endif

[Setup]
AppId={{2B5E7B5E-E962-42F0-A42D-6A5843E925FA}
AppName=屏幕翻译
AppVersion={#AppVersion}
AppVerName=屏幕翻译 {#AppVersion}
AppPublisher=Screen AI Translator
DefaultDirName={localappdata}\Programs\ScreenAITranslator
DefaultGroupName=Screen AI Translator
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\__release_packages__\{#AppVersion}
OutputBaseFilename=ScreenAITranslator-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\ScreenAITranslator.exe
VersionInfoVersion={#AppVersion}
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
Source: "{#AppBinary}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\nlohmann\LICENSE.MIT"; DestDir: "{app}\licenses"; DestName: "nlohmann-json-LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Screen AI Translator\Screen AI Translator"; Filename: "{app}\ScreenAITranslator.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Screen AI Translator\卸载屏幕翻译"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Screen AI Translator"; Filename: "{app}\ScreenAITranslator.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ScreenAITranslator"; ValueData: """{app}\ScreenAITranslator.exe"" --background"; Tasks: startup; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "ScreenAITranslator"; Tasks: not startup; Flags: deletevalue

[Run]
Filename: "{app}\ScreenAITranslator.exe"; Description: "打开屏幕翻译"; Flags: nowait postinstall skipifsilent

; User settings and DPAPI ciphertext live outside {app}.
; Uninstall preserves user data, including untouched legacy files, and never launches the program.





