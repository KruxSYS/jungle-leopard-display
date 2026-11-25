<#
.SYNOPSIS
  Builds a Jungle Leopard Display release: clean Release x64 build, both test
  suites, then the installer and the portable zip in dist\.

.DESCRIPTION
  Run from a clone on Windows (PowerShell 5.1 or 7):

      powershell -ExecutionPolicy Bypass -File tools\release.ps1

  Needs Visual Studio 2022 (C++ desktop + .NET desktop workloads, found with
  vswhere), the .NET 9 SDK, and Inno Setup 6.6 or later (ISCC.exe on PATH, or
  installed per-user or machine-wide in the default place).

  The version comes from <Version> in JLDisplayManager\JLDisplayManager.csproj,
  and must agree with JLDisplayCore\jl_version.h (the native binaries) - the
  script stops if it does not.

  Produces:
      dist\JungleLeopardDisplay-<ver>-win-x64-setup.exe
      dist\JungleLeopardDisplay-<ver>-win-x64-portable.zip
      dist\stage-<ver>\            the files both were made from

  Any build warning, test failure or missing tool stops the script with a
  non-zero exit code. Nothing is signed (as with Plexdio).
#>
[CmdletBinding()]
param(
    # Skip the tests. For investigating a packaging problem only; a release
    # must not be made this way.
    [switch]$NoTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$Sln = Join-Path $Root 'Jungle Leopard Display.sln'
$Dist = Join-Path $Root 'dist'

function Find-Exe($name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    return $null
}

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Fail($text) { Write-Host "release: $text" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
Step 'Version'

$csproj = Get-Content (Join-Path $Root 'JLDisplayManager\JLDisplayManager.csproj') -Raw
if ($csproj -notmatch '<Version>([0-9]+\.[0-9]+\.[0-9]+)</Version>') { Fail 'no <Version> in JLDisplayManager.csproj' }
$Version = $Matches[1]

$header = Get-Content (Join-Path $Root 'JLDisplayCore\jl_version.h') -Raw
$parts = foreach ($n in 'MAJOR', 'MINOR', 'PATCH') {
    if ($header -notmatch "#define JL_VERSION_$n\s+(\d+)") { Fail "JL_VERSION_$n missing from jl_version.h" }
    $Matches[1]
}
$native = $parts -join '.'
if ($native -ne $Version) { Fail "csproj says $Version but jl_version.h says $native - bump both" }
Write-Host "version $Version"

# ---------------------------------------------------------------------------
Step 'Tools'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found - install Visual Studio 2022' }
# Visual Studio 2022 specifically (the v143 toolset the projects name), even
# when a newer Visual Studio is installed beside it.
$vs = & $vswhere -latest -products * -version '[17.0,18.0)' `
    -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
if (-not $vs) { Fail 'no Visual Studio 2022 with MSBuild found' }
$MSBuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $MSBuild)) { Fail "MSBuild not at $MSBuild" }
Write-Host "msbuild  $MSBuild"

$Dotnet = Find-Exe 'dotnet.exe'
if (-not $Dotnet) {
    $candidate = Join-Path $env:ProgramFiles 'dotnet\dotnet.exe'
    if (Test-Path $candidate) { $Dotnet = $candidate; $env:PATH = (Split-Path $candidate) + ';' + $env:PATH }
}
if (-not $Dotnet) { Fail 'dotnet.exe not found - install the .NET 9 SDK' }
Write-Host "dotnet   $Dotnet"

$Iscc = Find-Exe 'ISCC.exe'
if (-not $Iscc) {
    foreach ($c in @((Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
                     (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
                     (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'))) {
        if (Test-Path $c) { $Iscc = $c; break }
    }
}
if (-not $Iscc) { Fail 'ISCC.exe not found - install Inno Setup 6.6+ (winget install JRSoftware.InnoSetup)' }
Write-Host "iscc     $Iscc"

# ---------------------------------------------------------------------------
Step 'Clean'

foreach ($d in 'x64\Release', 'test-out') {
    $p = Join-Path $Root $d
    if (Test-Path $p) { Remove-Item $p -Recurse -Force }
}

# ---------------------------------------------------------------------------
Step 'Build (Release|x64, rebuild)'

$log = Join-Path $env:TEMP "jld-release-build-$Version.log"
& $MSBuild $Sln -t:Rebuild -restore -m -nologo -v:minimal `
    -p:Configuration=Release -p:Platform=x64 `
    "-flp:logfile=$log;verbosity=normal" -clp:"ErrorsOnly;WarningsOnly;Summary"
$buildExit = $LASTEXITCODE

$warnings = @(Select-String -Path $log -Pattern ': warning [A-Z]+[0-9]+' |
              ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
if ($buildExit -ne 0) { Fail "build failed (exit $buildExit); full log: $log" }
if ($warnings.Count -gt 0) {
    $warnings | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
    Fail "$($warnings.Count) build warning(s); the release baseline is zero"
}
Write-Host 'build: 0 errors, 0 warnings'

# ---------------------------------------------------------------------------
if ($NoTests) {
    Write-Host "`n!! tests skipped (-NoTests): do not publish this build" -ForegroundColor Yellow
}
else {
    Step 'Native tests'
    & (Join-Path $Root 'test-out\x64\Release\JLDisplayCore.Tests.exe')
    if ($LASTEXITCODE -ne 0) { Fail 'native tests failed' }

    Step 'Managed tests'
    & $Dotnet test (Join-Path $Root 'test-out\managed\JLDisplayManager.Tests.dll') --nologo
    if ($LASTEXITCODE -ne 0) { Fail 'managed tests failed' }
}

# ---------------------------------------------------------------------------
Step 'Stage'

$Out = Join-Path $Root 'x64\Release'
$Stage = Join-Path $Dist "stage-$Version"
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null

# Exactly what ships, by name: the build folder also holds .lib/.exp files.
$files = @(
    'JungleLeopardDisplayManager.exe',
    'JungleLeopardDisplayManager.dll',
    'JungleLeopardDisplayManager.deps.json',
    'JungleLeopardDisplayManager.runtimeconfig.json',
    'JLDisplayNative.dll',
    'Jungle Leopard Display.exe',
    'yt-dlp.exe'
)
foreach ($f in $files) {
    $src = Join-Path $Out $f
    if (-not (Test-Path $src)) { Fail "missing from the build: $f" }
    Copy-Item $src $Stage
}

$readme = @"
Jungle Leopard Display $Version
by KruxSYS - https://kruxsys.com

Drives the TZMRIT PF360 "Jungle Leopard" pump LCD (960x480) from Windows 10/11 x64.

  JungleLeopardDisplayManager.exe   the tray app: library, playlist, overlays,
                                    music following, AI images
  Jungle Leopard Display.exe        the command-line tool (run with no arguments
                                    for usage)

Needs:
  - .NET 9 Desktop Runtime (the manager only):
        winget install Microsoft.DotNet.DesktopRuntime.9
  - ffmpeg, to convert images and videos:
        winget install "FFmpeg (Essentials Build)"
    or put ffmpeg.exe in this folder.

Only one program can hold the panel at a time: do not run the command-line tool
while the manager is running (Settings > Release device hands it over).

Your library and settings live in %LOCALAPPDATA%\JungleLeopardDisplay; removing
this folder does not touch them.

Source, documentation and releases:
  https://github.com/KruxSYS/jungle-leopard-display

(c) 2026 KruxSYS
"@
Set-Content -Path (Join-Path $Stage 'README.txt') -Value $readme -Encoding UTF8
Get-ChildItem $Stage | ForEach-Object { '{0,-48} {1,12:N0}' -f $_.Name, $_.Length } | Write-Host

# ---------------------------------------------------------------------------
Step 'Installer'

$setupName = "JungleLeopardDisplay-$Version-win-x64-setup.exe"
$setup = Join-Path $Dist $setupName
if (Test-Path $setup) { Remove-Item $setup -Force }
& $Iscc /Q "/DSourceDir=$Stage" "/DAppVersion=$Version" "/O$Dist" (Join-Path $Root 'installer\jungle-leopard-display.iss')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $setup)) { Fail 'Inno Setup failed' }

# ---------------------------------------------------------------------------
Step 'Portable zip'

$zip = Join-Path $Dist "JungleLeopardDisplay-$Version-win-x64-portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $zip -CompressionLevel Optimal

# ---------------------------------------------------------------------------
Step 'Done'

foreach ($a in $setup, $zip) {
    $h = (Get-FileHash $a -Algorithm SHA256).Hash.ToLowerInvariant()
    '{0}  {1,12:N0}  sha256 {2}' -f (Split-Path $a -Leaf), (Get-Item $a).Length, $h | Write-Host
}
exit 0
