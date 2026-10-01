; Inno Setup script: builds dist\GuitarPedalboard-<version>-Setup.exe
; Compile with packaging\build-windows.ps1 (or: ISCC.exe packaging\GuitarPedalboard.iss)

#define AppName    "Guitar Pedalboard"
#define AppVersion "1.2.1"
#define BuildDir   "..\build\GuitarPedalboard_artefacts\Release"

[Setup]
AppId={{7E0C8E52-3C6B-4B7A-9D1E-6A9F2C4B8D11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=DIY Audio
AppPublisherURL=https://github.com/Anyouhuang/guitar-pedalboard
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; the VST3 plugin goes to C:\Program Files\Common Files\VST3, which needs admin rights
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=GuitarPedalboard-{#AppVersion}-Setup
SetupIconFile=..\build\GuitarPedalboard_artefacts\JuceLibraryCode\icon.ico
UninstallDisplayIcon={app}\{#AppName}.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Types]
Name: "full";   Description: "App + VST3 plugin"
Name: "app";    Description: "App only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "app";  Description: "Guitar Pedalboard app (runs on its own)"; Types: full app custom; Flags: fixed
Name: "vst3"; Description: "VST3 plugin (for DAWs such as Reaper, Cubase, Studio One, FL Studio)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#BuildDir}\Standalone\{#AppName}.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "QuickStart-zh-TW.txt"; DestDir: "{app}"; Components: app; Flags: ignoreversion isreadme
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Components: app; Flags: ignoreversion
Source: "{#BuildDir}\VST3\{#AppName}.vst3\*"; DestDir: "{commoncf64}\VST3\{#AppName}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppName}.exe"
Name: "{autoprograms}\{#AppName} Quick Start"; Filename: "{app}\QuickStart-zh-TW.txt"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppName}.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
