; SPDX-License-Identifier: Apache-2.0
; Inno Setup script of the OpenMine Suite Windows installer.
;   iscc /DAppVersion=0.4.0 /DStage=<staged folder> /DOutputDir=<dir> packaging\windows\openmine-suite.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Stage
  #define Stage "..\..\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
AppId={{6B1B3E2A-5F2C-4C47-9C1E-0B7E4A0D3F51}
AppName=OpenMine Suite
AppVersion={#AppVersion}
AppVerName=OpenMine Suite {#AppVersion}
AppPublisher=OpenMine Suite contributors
DefaultDirName={autopf}\OpenMine Suite
DefaultGroupName=OpenMine Suite
DisableProgramGroupPage=yes
LicenseFile={#Stage}\docs\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=OpenMine-Suite-{#AppVersion}-Setup
SetupIconFile=openmine-suite.ico
UninstallDisplayIcon={app}\openmine-desktop.exe
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "associate"; Description: "Open .openmine project files with OpenMine Suite"; GroupDescription: "File types:"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\OpenMine Suite"; Filename: "{app}\openmine-desktop.exe"
Name: "{group}\User guide"; Filename: "{app}\docs\user-guide.md"
Name: "{group}\Uninstall OpenMine Suite"; Filename: "{uninstallexe}"
Name: "{autodesktop}\OpenMine Suite"; Filename: "{app}\openmine-desktop.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.openmine"; ValueType: string; ValueName: ""; ValueData: "OpenMineSuite.Project"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\OpenMineSuite.Project"; ValueType: string; ValueName: ""; ValueData: "OpenMine Suite project"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\OpenMineSuite.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\openmine-desktop.exe,0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\OpenMineSuite.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\openmine-desktop.exe"" ""%1"""; Tasks: associate

[Run]
Filename: "{app}\openmine-desktop.exe"; Description: "{cm:LaunchProgram,OpenMine Suite}"; Flags: nowait postinstall skipifsilent
