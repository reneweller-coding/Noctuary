; Noctuary -- the Windows installer.
;
; Built by Deploy/build_release.ps1, which stages everything under Deploy/stage first. Nothing in
; here reaches into the build tree: what is in the staging folder is exactly what gets installed,
; so the setup can be looked at before it is run.
;
;   ISCC.exe /DVersion=1.0.0 Deploy\Noctuary.iss
;
; The binaries are linked against the static runtime (AMBIENT_STATIC_RUNTIME), so there is no
; redistributable to chase and no DLL beside the executable: two files and the presets.

#ifndef Version
  #define Version "1.0.0"
#endif
#define AppName "Noctuary"
#define Publisher "Rene Weller"
#define AppURL "https://github.com/reneweller-coding/Noctuary"
#define Stage "stage"
; Where the sample library is downloaded from. The archives are release assets; the lines that
; name them, with their sizes and hashes, are generated into content-files.iss by
; Tools/make_content_pack.py, because both change with every rebuild of the package.
#ifndef ContentBaseUrl
  ; Only a fallback for a hand-run compile. build_release.ps1 passes the real one
  ; (/DContentBaseUrl=...): the archives live with the release that introduced them, and this
  ; line pointing at a fixed old tag is what produced "Download failed: 404 Not Found".
  #define ContentBaseUrl "https://github.com/reneweller-coding/Noctuary/releases/download/library-v5"
#endif
#define HaveContent FileExists(AddBackslash(SourcePath) + "content-files.iss")
#if HaveContent
  #define ContentSize FileRead(FileOpen(AddBackslash(SourcePath) + "content-size.txt"))
#endif

[Setup]
AppId={{7C3B9E14-5A2D-4C88-9E1F-2B6A0D5F3A41}
AppName={#AppName}
AppVersion={#Version}
AppVerName={#AppName} {#Version}
AppPublisher={#Publisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile={#Stage}\LICENSE.txt
OutputDir=out
OutputBaseFilename={#AppName}-{#Version}-Setup
SetupIconFile={#Stage}\logo.ico
UninstallDisplayIcon={app}\Noctuary.exe
UninstallDisplayName={#AppName} {#Version}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dynamic
; Lets the [Files] section unpack a downloaded archive on its own (Inno 6.4+).
ArchiveExtraction=auto
; For everybody on the machine by default -- the VST3 belongs in the shared plug-in folder, which
; needs administrator rights. Anyone who does not have them can choose "just for me" in the first
; dialog (or pass /CURRENTUSER) and gets the per-user plug-in folder instead; the instrument looks
; in both. Every path below is an {auto...} one, which is what makes the two modes the same script.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
; The compiler warns that a per-user folder is written while the default mode is admin. It is
; guarded: the {localappdata} line runs only under "Check: not IsAdminInstallMode", which is the
; per-user mode itself. Silenced knowingly rather than left as noise on every build.
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
VersionInfoVersion={#Version}
VersionInfoCompany={#Publisher}
VersionInfoDescription={#AppName} installer

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[CustomMessages]
en.CompStandalone=Standalone application
en.CompVst3=VST3 plug-in (for a DAW)
en.CompPacks=Preset library (56 packs, 14336 presets)
en.CompContent=Sample library: the samples, wavetables and impulse responses the presets use (downloaded, %1 GB)
en.TaskDesktop=Create a desktop shortcut
en.DownloadFailed=The sample library could not be downloaded:%n%n%1%n%nEverything else installs and works without it; presets that want a sample fall back to the built-in sources. You can install the library later by unpacking the content archives from the release into the Noctuary folder.%n%nInstall without the sample library?
en.NoAvx2=This processor reports no AVX2 support.%n%nNoctuary is built for AVX2, which every x86-64 processor since 2013 has. Without it, it will not start.%n%nInstall anyway?
de.CompStandalone=Eigenstaendiges Programm
de.CompVst3=VST3-Plugin (fuer eine DAW)
de.CompPacks=Preset-Bibliothek (56 Pakete, 14336 Presets)
de.CompContent=Sample-Bibliothek: die Samples, Wavetables und Impulsantworten der Presets (wird geladen, %1 GB)
de.TaskDesktop=Verknuepfung auf dem Desktop anlegen
de.DownloadFailed=Die Sample-Bibliothek konnte nicht geladen werden:%n%n%1%n%nAlles andere wird installiert und funktioniert auch ohne sie; Presets, die ein Sample moechten, greifen auf die eingebauten Quellen zurueck. Die Bibliothek laesst sich spaeter nachlegen, indem man die Content-Archive aus dem Release in den Noctuary-Ordner entpackt.%n%nOhne die Sample-Bibliothek installieren?
de.NoAvx2=Dieser Prozessor meldet keine AVX2-Unterstuetzung.%n%nNoctuary ist fuer AVX2 gebaut, das jeder x86-64-Prozessor seit 2013 hat. Ohne AVX2 startet es nicht.%n%nTrotzdem installieren?

[Types]
Name: "full"; Description: "{code:FullTypeName}"
Name: "custom"; Description: "{code:CustomTypeName}"; Flags: iscustom

[Components]
Name: "standalone"; Description: "{cm:CompStandalone}"; Types: full custom; Flags: fixed
Name: "vst3";       Description: "{cm:CompVst3}";       Types: full custom
Name: "packs";      Description: "{cm:CompPacks}";      Types: full custom
#if HaveContent
; The presets name their samples by relative path, so the sample library only makes sense
; alongside the packs -- hence "packs" is a dependency, not just a suggestion.
Name: "packs\content"; Description: "{cm:CompContent,{#ContentSize}}"; Types: full custom
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktop}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; Components: standalone

[Files]
Source: "{#Stage}\Noctuary.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\LICENSE.txt";      DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\README.txt";       DestDir: "{app}"; Components: standalone; Flags: ignoreversion isreadme
; The manual, so it is on the machine rather than only on a web page somewhere.
Source: "{#Stage}\Noctuary-Manual.pdf"; DestDir: "{app}"; Components: standalone;     Flags: ignoreversion skipifsourcedoesntexist
; The VST3 is a bundle: a folder that the host reads as one plug-in.
Source: "{#Stage}\Noctuary.vst3\*"; DestDir: "{autocf}\VST3\Noctuary.vst3"; \
    Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
; Packs go into a folder of the installer's own -- machine-wide or, for an install without
; administrator rights, per user -- so that removing them again can never take a pack the user put
; there themselves along with it. The instrument reads both of these as well as each user's own
; Documents\Noctuary\Packs (see Core/src/PresetPacks.cpp).
Source: "{#Stage}\Packs\*.ambientpack"; DestDir: "{code:LibDir}\Packs"; \
    Components: packs; Flags: ignoreversion
; The journey templates (presets in a row, Core/include/ambient/Journey.h), beside the packs;
; the player's own go to Documents\Noctuary\Journeys and are never touched.
Source: "{#Stage}\Journeys\*.journey"; DestDir: "{code:LibDir}\Journeys"; \
    Components: packs; Flags: ignoreversion skipifsourcedoesntexist
#if HaveContent
; The sample library: fetched by the [Code] section below (which can survive a failure), checked
; against its hash on the way in, and unpacked here into the library folder beside the packs --
; the archives hold Textures\, FieldRecordings\, Wavetables\, Impulses\ and Archive\ (the
; recordings the near layer plays), which is exactly what the packs' and the near bank's
; relative paths expect.
#include "content-files.iss"
#endif

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\Noctuary.exe"; Components: standalone
Name: "{group}\Manual"; Filename: "{app}\Noctuary-Manual.pdf"; Components: standalone;     Check: FileExists(ExpandConstant('{app}\Noctuary-Manual.pdf'))
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Noctuary.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Noctuary.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent; Components: standalone

[UninstallDelete]
; The plug-in bundle and the pack folders are ones the installer made; what a user put in their
; own Documents is theirs and is never touched.
Type: filesandordirs; Name: "{autocf}\VST3\Noctuary.vst3"
Type: filesandordirs; Name: "{commonappdata}\Noctuary\Packs"
Type: filesandordirs; Name: "{commonappdata}\Noctuary\Textures"
Type: filesandordirs; Name: "{commonappdata}\Noctuary\FieldRecordings"
Type: filesandordirs; Name: "{commonappdata}\Noctuary\Wavetables"
Type: filesandordirs; Name: "{commonappdata}\Noctuary\Impulses"
Type: filesandordirs; Name: "{commonappdata}\Noctuary\Archive"
Type: filesandordirs; Name: "{commonappdata}\Noctuary\Journeys"
Type: dirifempty;     Name: "{commonappdata}\Noctuary"
Type: filesandordirs; Name: "{localappdata}\Noctuary\Packs"
Type: filesandordirs; Name: "{localappdata}\Noctuary\Textures"
Type: filesandordirs; Name: "{localappdata}\Noctuary\FieldRecordings"
Type: filesandordirs; Name: "{localappdata}\Noctuary\Wavetables"
Type: filesandordirs; Name: "{localappdata}\Noctuary\Impulses"
Type: filesandordirs; Name: "{localappdata}\Noctuary\Archive"
Type: filesandordirs; Name: "{localappdata}\Noctuary\Journeys"
Type: dirifempty;     Name: "{localappdata}\Noctuary"

[Code]
#if HaveContent
var
  DownloadPage: TDownloadWizardPage;
  ContentOk: Boolean;

// Which archives to fetch, with their hashes: generated, because both change with the package.
#include "content-code.iss"

// Guards the [Files] lines for the archives. False when the component was not chosen, or when
// the download did not happen -- there is nothing in {tmp} to unpack then.
function ContentDownloaded: Boolean;
begin
  Result := ContentOk;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
  ContentOk := False;
end;

// The sample library is three gigabytes over somebody else's network, and it is an extra: a
// download that fails must not cost the instrument as well. So it is fetched here, where the
// answer to "it did not work" can be "install the rest anyway" -- which is also what a silent
// install does, since that is the default button.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsComponentSelected('packs\content') then begin
    DownloadPage.Clear;
    AddContentDownloads(DownloadPage);
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        ContentOk := True;
      except
        if DownloadPage.AbortedByUser then
          Result := False
        else
          Result := SuppressibleMsgBox(FmtMessage(CustomMessage('DownloadFailed'), [GetExceptionMessage]),
                                       mbError, MB_YESNO or MB_DEFBUTTON1, IDYES) = IDYES;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;
#endif

// AVX2 is a hard requirement of the shipped binaries, and a processor without it does not fail
// gracefully -- it takes an illegal instruction and dies with no explanation at all. Asked here,
// where there is still somewhere to say it. Answered rather than enforced: the query is a Windows
// feature flag, and being told "no" by an old Windows is not the same as the processor lacking it.
function IsProcessorFeaturePresent(Feature: DWord): Boolean;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsProcessorFeaturePresent(40) then                    // PF_AVX2_INSTRUCTIONS_AVAILABLE
    Result := MsgBox(CustomMessage('NoAvx2'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
end;

// Where the preset packs and their samples live: one folder, machine-wide or per user, so that
// the relative paths inside the packs ("../Textures/x.wav") land where they are looked for. What
// actually arrives there is "x.flac" -- the same audio at half the download -- and the synth looks
// for the FLAC beside the name a pack gives, so both spellings work. Never the user's own
// Documents\Noctuary, which is theirs and must survive an uninstall.
function LibDir(Param: String): String;
begin
  if IsAdminInstallMode then
    Result := ExpandConstant('{commonappdata}\Noctuary')
  else
    Result := ExpandConstant('{localappdata}\Noctuary');
end;

function FullTypeName(Param: String): String;
begin
  if ActiveLanguage = 'de' then Result := 'Vollstaendig' else Result := 'Full installation';
end;

function CustomTypeName(Param: String): String;
begin
  if ActiveLanguage = 'de' then Result := 'Benutzerdefiniert' else Result := 'Custom installation';
end;
