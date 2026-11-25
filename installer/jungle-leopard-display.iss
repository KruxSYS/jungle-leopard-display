; Jungle Leopard Display installer (Inno Setup 6.6+, for WizardStyle "modern dark").
; Built by tools/release.ps1:
;   ISCC.exe /DSourceDir=<staged files> /DAppVersion=1.7.0 /O<output folder> installer\jungle-leopard-display.iss
;
; Modelled on Plexdio's (KruxSYS sister project): per-user install by default, no admin prompt;
; "for all users" offered when elevated; Start menu shortcut, optional desktop icon, uninstaller.
; User data in %LOCALAPPDATA%\JungleLeopardDisplay (and the shared %LOCALAPPDATA%\jl_display cache)
; is kept on uninstall. Unsigned, as Plexdio's is.

#ifndef SourceDir
  #define SourceDir "..\dist\stage-installer"
#endif
#ifndef AppVersion
  #error Pass the version: /DAppVersion=x.y.z (tools/release.ps1 reads it from JLDisplayManager.csproj)
#endif

#define AppName "Jungle Leopard Display"
#define AppExe "JungleLeopardDisplayManager.exe"
#define CliExe "Jungle Leopard Display.exe"
#define AppPublisher "KruxSYS"
#define PublisherUrl "https://kruxsys.com"
; The one place this file names the repository.
#define RepoUrl "https://github.com/KruxSYS/jungle-leopard-display"
; Held by the running manager (App.xaml.cs). Never rename: it is how this
; installer and a second launch find the running instance.
#define AppMutexName "Local\JungleLeopardDisplayManager.Instance"
; Registered by the manager's "Start when I sign in" (Services/StartupTask.cs).
#define TaskName "JungleLeopardDisplayManager"
#define DotNetUrl "https://dotnet.microsoft.com/download/dotnet/9.0"
#define FfmpegUrl "https://www.gyan.dev/ffmpeg/builds/"

[Setup]
AppId={{6F1C2B9E-4D7A-4E5B-9C3F-8A2D1E7B5C40}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#PublisherUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases
AppCopyright=© 2026 KruxSYS
; {autopf} is %LOCALAPPDATA%\Programs for a per-user install, Program Files for all users.
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=JungleLeopardDisplay-{#AppVersion}-win-x64-setup
SetupIconFile=..\JLDisplayManager\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dark
WizardImageFile=wizard-100.bmp,wizard-200.bmp
WizardSmallImageFile=wizard-small-100.bmp,wizard-small-200.bmp
VersionInfoCompany={#AppPublisher}
VersionInfoCopyright=© 2026 KruxSYS
VersionInfoProductName={#AppName}
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} setup
; The manager usually runs as a tray icon. The Restart Manager closes it before
; its files are replaced (interactively Setup asks first); PrepareToInstall below
; makes sure, because a tray app with no visible window is easy to miss.
CloseApplications=force
RestartApplications=no
UsePreviousAppDir=yes
UsePreviousTasks=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
en.UninstalledAll=Jungle Leopard Display has been removed.%n%nYour library, playlist, settings and overlays were kept in %LOCALAPPDATA%\JungleLeopardDisplay, and preprocessed frames in %LOCALAPPDATA%\jl_display. Delete those folders to remove them too.
en.UninstalledMost=Jungle Leopard Display has been removed, but some files could not be deleted.%n%nYour library, playlist, settings and overlays were kept in %LOCALAPPDATA%\JungleLeopardDisplay.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\JungleLeopardDisplayManager.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\JungleLeopardDisplayManager.deps.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\JungleLeopardDisplayManager.runtimeconfig.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\JLDisplayNative.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\{#CliExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\yt-dlp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Drive the Jungle Leopard pump LCD"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; Offered only when the manager can actually start: without the .NET 9 Desktop
; Runtime it would fail at once.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent; Check: DotNetDesktopPresent

[UninstallRun]
; The logon task lives outside {app}. Deleting one that was never registered
; fails harmlessly.
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""{#TaskName}"" /F"; Flags: runhidden; RunOnceId: "DeleteLogonTask"

[Code]
var
  DepsPage: TWizardPage;
  NeedDotNet, NeedFfmpeg: Boolean;

{ .NET 9 Desktop Runtime: any 9.x folder under the machine-wide runtime. }
function DotNetDesktopPresent: Boolean;
var
  FindRec: TFindRec;
begin
  Result := False;
  if FindFirst(ExpandConstant('{commonpf64}\dotnet\shared\Microsoft.WindowsDesktop.App\9.*'), FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          Result := True;
      until Result or not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ ffmpeg: winget's links folder, or anywhere on PATH. (Not {app}: this runs
  before the install folder is chosen, and {app} cannot be expanded yet.) }
function FfmpegPresent: Boolean;
var
  Path, Dir: String;
  P: Integer;
begin
  Result := FileExists(ExpandConstant('{localappdata}\Microsoft\WinGet\Links\ffmpeg.exe'));
  if Result then Exit;

  Path := GetEnv('PATH');
  while (not Result) and (Path <> '') do
  begin
    P := Pos(';', Path);
    if P = 0 then begin Dir := Path; Path := ''; end
    else begin Dir := Copy(Path, 1, P - 1); Delete(Path, 1, P); end;
    Dir := RemoveQuotes(Trim(Dir));
    if Dir <> '' then
      Result := FileExists(AddBackslash(Dir) + 'ffmpeg.exe');
  end;
end;

procedure OpenUrl(Url: String);
var
  Code: Integer;
begin
  ShellExec('open', Url, '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

procedure DotNetClick(Sender: TObject);
begin
  OpenUrl('{#DotNetUrl}');
end;

procedure FfmpegClick(Sender: TObject);
begin
  OpenUrl('{#FfmpegUrl}');
end;

function AddLabel(Page: TWizardPage; Top: Integer; Text: String; Bold: Boolean): TNewStaticText;
begin
  Result := TNewStaticText.Create(Page);
  Result.Parent := Page.Surface;
  Result.Top := Top;
  Result.Width := Page.SurfaceWidth;
  Result.AutoSize := False;
  Result.WordWrap := True;
  Result.Caption := Text;
  if Bold then Result.Font.Style := [fsBold];
  Result.Height := ScaleY(48);
  Result.AdjustHeight;
end;

function AddButton(Page: TWizardPage; Top: Integer; Text: String; OnClick: TNotifyEvent): TNewButton;
begin
  Result := TNewButton.Create(Page);
  Result.Parent := Page.Surface;
  Result.Top := Top;
  Result.Width := ScaleX(220);
  Result.Height := ScaleY(24);
  Result.Caption := Text;
  Result.OnClick := OnClick;
end;

{ One page, shown only when something is missing. Nothing is bundled: each item
  says what it is for, and offers the download page or the winget line. }
procedure InitializeWizard;
var
  Top: Integer;
  L: TNewStaticText;
begin
  NeedDotNet := not DotNetDesktopPresent;
  NeedFfmpeg := not FfmpegPresent;

  DepsPage := CreateCustomPage(wpSelectTasks, 'Before you start',
    'Jungle Leopard Display needs these, and they are not installed yet. You can install them now or later.');

  Top := 0;
  if NeedDotNet then
  begin
    L := AddLabel(DepsPage, Top, '.NET 9 Desktop Runtime (required)', True);
    Top := L.Top + L.Height + ScaleY(4);
    L := AddLabel(DepsPage, Top, 'The display manager is a .NET 9 app and will not start without it. ' +
      'Install it from Microsoft, or run:' + #13#10 +
      '    winget install Microsoft.DotNet.DesktopRuntime.9', False);
    Top := L.Top + L.Height + ScaleY(6);
    AddButton(DepsPage, Top, 'Open the .NET download page', @DotNetClick);
    Top := Top + ScaleY(40);
  end;

  if NeedFfmpeg then
  begin
    L := AddLabel(DepsPage, Top, 'ffmpeg (recommended)', True);
    Top := L.Top + L.Height + ScaleY(4);
    L := AddLabel(DepsPage, Top, 'Converts images and videos for the panel. Without it only a ' +
      'ready-made 960x480 JPEG can be shown. Install it with:' + #13#10 +
      '    winget install "FFmpeg (Essentials Build)"' + #13#10 +
      'or put ffmpeg.exe in the install folder.', False);
    Top := L.Top + L.Height + ScaleY(6);
    AddButton(DepsPage, Top, 'Open the ffmpeg download page', @FfmpegClick);
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (DepsPage <> nil) and (PageID = DepsPage.ID) and not (NeedDotNet or NeedFfmpeg);
end;

{ Stops a running manager - usually a tray icon with no window - before its
  files are replaced or removed. Restart Manager normally does this; this is
  the fallback for when it could not, waiting for the instance mutex to go. }
procedure StopRunningManager;
var
  Code, Waited: Integer;
begin
  if not CheckForMutexes('{#AppMutexName}') then Exit;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM {#AppExe} /F', '', SW_HIDE,
    ewWaitUntilTerminated, Code);
  Waited := 0;
  while CheckForMutexes('{#AppMutexName}') and (Waited < 5000) do
  begin
    Sleep(250);
    Waited := Waited + 250;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningManager;
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  StopRunningManager;
  Result := True;
end;
