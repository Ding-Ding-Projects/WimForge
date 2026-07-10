; WimForge Windows x64 installer
; Values used by CI are supplied with ISCC /D switches from build-release.ps1.

#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif

#ifndef MySourceDir
  #define MySourceDir "..\dist\WimForge-portable-x64-0.1.0"
#endif

#ifndef MyOutputDir
  #define MyOutputDir "..\dist"
#endif

#define MyAppName "WimForge"
#define MyAppPublisher "codingmachineedge"
#define MyAppExeName "WimForge.exe"
#define MyAppUrl "https://github.com/codingmachineedge/WimForge"

[Setup]
AppId={{D72458D7-6214-43E9-8F65-58E046A08F14}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppCopyright=Copyright (c) 2026 codingmachineedge
AppComments=Open-source Windows image customization studio
AppSupportURL={#MyAppUrl}/issues
AppUpdatesURL={#MyAppUrl}/releases/latest
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\assets\app-icon.ico
OutputDir={#MyOutputDir}
OutputBaseFilename=WimForge-Setup-x64-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0.17763
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion={#MyAppVersion}.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright=Copyright (c) 2026 codingmachineedge
VersionInfoDescription=Open-source Windows image customization studio
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
; Keep the legal and handoff documents as required sources instead of relying
; only on a recursive wildcard. Missing documents therefore fail compilation.
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Excludes: "README.md,LICENSE"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\WimForge"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall WimForge"; Filename: "{uninstallexe}"
Name: "{autodesktop}\WimForge"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch WimForge"; Flags: nowait postinstall skipifsilent
