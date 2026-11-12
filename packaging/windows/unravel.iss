; Windows installer (Inno Setup 6 or 7), built by the nightly workflow:
;
;   iscc /O<output dir> /DAppVersion=0.1.0 /DAppFiles=<deployed app folder> unravel.iss
;
; It installs into a fixed program folder, without administrator rights
; for the current user (%LOCALAPPDATA%\Programs\Unravel) or, chosen in a
; dialog, for all users (Program Files). Two checkboxes (both ticked) add
; a Start menu entry and a desktop shortcut. It registers an uninstaller
; in Windows' list of apps.
;
; The program folder holds unravel.exe, unravel-cli.exe, libclang.dll and
; clang's own headers (lib\clang). The indexes live in the unravel folder
; (%LOCALAPPDATA%\Unravel), the settings in the registry
; (HKCU\Software\Unravel); the uninstaller removes the indexes only.
; /NOLAUNCH=1 keeps the program from starting after installing (for
; tests).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppFiles
  #define AppFiles "..\..\Unravel"
#endif

[Setup]
; Never change the AppId: it is how Windows and later versions recognise
; an existing installation
AppId={{63E86A9C-FF58-4A9C-826D-50AB8080057C}
AppName=Unravel
AppVersion={#AppVersion}
AppVerName=Unravel {#AppVersion}
AppPublisher=Unravel
AppPublisherURL=https://iam-peter.github.io/unravel/
AppSupportURL=https://github.com/iam-peter/unravel/issues
AppUpdatesURL=https://github.com/iam-peter/unravel/releases
AppCopyright=MIT License
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Unravel Setup
; Always the same, known place: no folder to choose
DefaultDirName={autopf}\Unravel
DisableDirPage=yes
DefaultGroupName=Unravel
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=unravel-windows-x64-setup
SetupIconFile=..\..\logo\unravel.ico
UninstallDisplayIcon={app}\unravel.exe
UninstallDisplayName=Unravel
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Close a running Unravel before replacing its files
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startmenuicon"; Description: "Start menu entry"; GroupDescription: "{cm:AdditionalIcons}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; Qt's folders and clang's headers (lib) from the previous version, so no
; outdated plugin, QML module or header stays behind. The DLLs next to
; the program are overwritten.
Type: filesandordirs; Name: "{app}\qml"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\lib"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\qmltooling"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\translations"

[Files]
Source: "{#AppFiles}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Unravel"; Filename: "{app}\unravel.exe"; Tasks: startmenuicon
Name: "{autodesktop}\Unravel"; Filename: "{app}\unravel.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\unravel.exe"; Description: "{cm:LaunchProgram,Unravel}"; Flags: nowait postinstall; Check: ShouldLaunch

[UninstallDelete]
; Leftovers that are no one's work: the indexes and the QML cache
Type: filesandordirs; Name: "{localappdata}\Unravel"

[Code]
function ShouldLaunch: Boolean;
begin
  Result := ExpandConstant('{param:NOLAUNCH|0}') = '0';
end;
