<#
  build.ps1 - build MaxBeiDou-ijl15 (ezorsia) without opening Visual Studio.

  USAGE
    .\build.ps1                       # Release | Win32  (recommended, matches README)
    .\build.ps1 -Config Debug
    .\build.ps1 -Rebuild              # clean + full rebuild
    .\build.ps1 -Toolset v143         # override PlatformToolset (default v142)
    .\build.ps1 -Deploy               # build + copy ijl15.dll to the client folder
    .\build.ps1 -Deploy -DeployConfig # also overwrite config.ini in the client folder

  NOTES
    * Requires Visual Studio 2019/2022 (MSBuild) + Windows 10 SDK.
    * Output: <root>\out\Release\ijl15.dll   (Debug -> out\Debug)
    * Deploy target defaults to ..\BeiDou-Client ; override with -ClientDir <path>
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Config = 'Release',

    [ValidateSet('Win32', 'x64')]
    [string] $Platform = 'Win32',

    # v142 = VS2019 toolset (as configured in ezorsia.vcxproj), v143 = VS2022
    [string] $Toolset = 'v142',

    [switch] $Rebuild,
    [switch] $Deploy,
    [switch] $DeployConfig,

    [string] $ClientDir = '..\BeiDou-Client'
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
# build the vcxproj directly: the .sln maps "x86" -> project platform "Win32",
# which makes /p:Platform=Win32 fail on the solution metaproject.
$proj = Join-Path $root 'ezorsia\ezorsia.vcxproj'

function Write-Step($msg) { Write-Host "[build] $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "[build] $msg" -ForegroundColor Green }
function Fail($msg)       { Write-Host "[build] ERROR: $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- locate MSBuild
function Find-MSBuild {
    if ($env:MSBUILD -and (Test-Path $env:MSBUILD)) { return $env:MSBUILD }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
                            -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null
        if ($found) { return ($found | Select-Object -First 1).Trim() }
    }

    $fallback = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    if ($fallback) { return $fallback }
    return $null
}

# ---------------------------------------------------------------- pre-checks
if (-not (Test-Path $proj)) { Fail "project not found: $proj" }

$detoursLib = Join-Path $root 'detours\detours.lib'
if (-not (Test-Path $detoursLib)) { Fail "missing $detoursLib (needed at link time)" }

$msbuild = Find-MSBuild
if (-not $msbuild) { Fail 'MSBuild not found. Install Visual Studio 2019/2022 (Desktop C++ workload) or set $env:MSBUILD.' }
Write-Step "MSBuild : $msbuild"
Write-Step "Config  : $Config | $Platform (toolset $Toolset)"

# ---------------------------------------------------------------- build
$target  = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$before  = Get-Date
# SolutionDir must be passed explicitly when building a vcxproj directly,
# otherwise $(SolutionDir) is empty and detours.lib / OutDir resolve to the wrong place.
$slnDir  = $root.TrimEnd('\') + '\'

& $msbuild $proj `
    "/t:$target" `
    "/p:Configuration=$Config" `
    "/p:Platform=$Platform" `
    "/p:PlatformToolset=$Toolset" `
    "/p:SolutionDir=$slnDir" `
    '/nologo' '/m' '/v:minimal'
if ($LASTEXITCODE -ne 0) { Fail "MSBuild failed with exit code $LASTEXITCODE" }

# ---------------------------------------------------------------- locate output
$candidates = @(
    (Join-Path $root "out\$Config\ijl15.dll"),
    (Join-Path $root "ezorsia\$Platform\$Config\ijl15.dll"),
    (Join-Path $root "ezorsia\out\$Config\ijl15.dll")
)
$dll = $candidates | Where-Object { Test-Path $_ } |
       Sort-Object { (Get-Item $_).LastWriteTime } -Descending |
       Select-Object -First 1

if (-not $dll) { Fail 'ijl15.dll was not produced - check the build log above.' }
if ((Get-Item $dll).LastWriteTime -lt $before.AddSeconds(-2)) {
    Write-Host "[build] note: ijl15.dll was not rebuilt (everything up-to-date)." -ForegroundColor Yellow
}

$item = Get-Item $dll
Write-Ok "built: $($item.FullName)  ($([math]::Round($item.Length / 1KB)) KB, $($item.LastWriteTime))"

# ---------------------------------------------------------------- deploy
if (-not $Deploy) { exit 0 }

if (-not [System.IO.Path]::IsPathRooted($ClientDir)) { $ClientDir = Join-Path $root $ClientDir }
try { $client = (Resolve-Path $ClientDir).Path } catch { Fail "client dir not found: $ClientDir" }

Write-Step "deploying to: $client"

# the game locks ijl15.dll while running
$procs = @('BeiDou', 'MapleStory', 'BingLangIsland') | ForEach-Object { Get-Process $_ -ErrorAction SilentlyContinue }
if ($procs) {
    Write-Step 'stopping running game process(es)...'
    $procs | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

$dst = Join-Path $client 'ijl15.dll'
$bak = Join-Path $client 'ijl15.dll.bak'
if ((Test-Path $dst) -and -not (Test-Path $bak)) { Copy-Item $dst $bak -Force }

Copy-Item $dll $dst -Force
Write-Ok "copied ijl15.dll -> $dst"

$srcIni  = Join-Path $root 'ezorsia\config.ini'
$dstIni  = Join-Path $client 'config.ini'
if (Test-Path $srcIni) {
    if ($DeployConfig -or -not (Test-Path $dstIni)) {
        Copy-Item $srcIni $dstIni -Force
        Write-Ok "copied config.ini -> $dstIni"
    } else {
        Write-Host "[build] keeping existing config.ini (use -DeployConfig to overwrite)" -ForegroundColor Yellow
    }
}

$pdb = Join-Path (Split-Path $dll -Parent) 'ijl15.pdb'
if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $client 'ijl15.pdb') -Force }

Write-Ok 'deploy done.'
