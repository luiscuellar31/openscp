<#
.SYNOPSIS
    Build the OpenSCP setup and MSI for Windows x64.

.DESCRIPTION
    Builds a Release in its own directory, stages the application with its Qt
    libraries and plugins, the Qt translations, the Visual C++ runtime DLLs it
    imports and the licenses, checks the staged files with
    scripts\verify\windows-package.ps1, and writes to dist\:

      setup  OpenSCP-<version>-x64-setup.exe   Inno Setup; per user by default
      msi    OpenSCP-<version>-x64.msi         WiX v5; for all users

    with a .sha256 file next to each.

    Environment variables, besides those of scripts\windows.ps1 (QT_PREFIX,
    Qt6_DIR, VCPKG_ROOT, VCPKG_TARGET_TRIPLET, JOBS, CMAKE_GENERATOR):
      BUILD_DIR          Default: build-windows-package
      PACKAGE_FORMATS    Comma-separated: setup,msi (default: both)
      ISCC               Inno Setup compiler (default: the installed ISCC.exe)
      OPENSCP_WIX        WiX v5 command (default: wix). WIX is not read: the
                         WiX v3 installer sets it to its own folder.
      OPENSCP_UPDATE_PUBLIC_KEY
                         Base64 Ed25519 key that enables in-app updates
      OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION
                         ON for official artifacts
#>
param(
    [Parameter(Position = 0)]
    [string]$Command = 'package'
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$RepoDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$LogPrefix = '[pack]'
. (Join-Path $RepoDir 'scripts\lib\windows\build.ps1')
. (Join-Path $RepoDir 'scripts\lib\windows\pe.ps1')

if ($Command -in 'help', '-h', '--help') {
    Get-Help $PSCommandPath -Detailed
    exit 0
}
if ($Command -ne 'package') {
    Stop-WithError "Unknown command: $Command. Run with 'help' for the options."
}

function Get-AppVersion([string]$BuildDir) {
    $header = Join-Path $BuildDir 'AppVersion.hpp'
    $line = Select-String -Path $header -Pattern '#define OPENSCP_APP_VERSION "([0-9.]+)"' |
        Select-Object -First 1
    if (-not $line) { Stop-WithError "No version in $header." }
    return $line.Matches[0].Groups[1].Value
}

function Find-Iscc {
    $iscc = Get-EnvValue 'ISCC'
    if ($iscc) { return $iscc }
    foreach ($candidate in @(
            (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'))) {
        if (Test-Path $candidate) { return $candidate }
    }
    Stop-WithError 'Inno Setup 6 was not found. Install it or set ISCC to ISCC.exe.'
}

# Copies the Visual C++ runtime DLLs that the staged binaries import, and the
# ones those import in turn. Windows itself provides the Universal CRT.
function Copy-VcRuntime([string]$Stage) {
    $redist = Get-EnvValue 'VCToolsRedistDir'
    $crt = if ($redist) {
        Get-ChildItem -Path (Join-Path $redist 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
            Select-Object -First 1
    }
    if (-not $crt) {
        Stop-WithError 'The Visual C++ runtime redistributable was not found (VCToolsRedistDir).'
    }
    $available = @{}
    Get-ChildItem -Path $crt.FullName -Filter '*.dll' |
        ForEach-Object { $available[$_.Name.ToLowerInvariant()] = $_.FullName }

    $pending = [Collections.Generic.Queue[string]]::new()
    Get-ChildItem -Path $Stage -Recurse -File -Include '*.exe', '*.dll' |
        ForEach-Object { $pending.Enqueue($_.FullName) }
    while ($pending.Count -gt 0) {
        foreach ($import in (Read-PeImage $pending.Dequeue()).Imports) {
            $name = $import.ToLowerInvariant()
            $target = Join-Path $Stage $import
            if ($available.ContainsKey($name) -and -not (Test-Path $target)) {
                Copy-Item -Path $available[$name] -Destination $target
                $pending.Enqueue($target)
            }
        }
    }
}

function New-Stage([string]$BuildDir, [string]$QtBin) {
    $stage = Join-Path $BuildDir 'stage\OpenSCP'
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item -Path (Join-Path $BuildDir 'OpenSCP.exe') -Destination $stage

    Write-Log 'Deploying Qt'
    # OpenSCP needs no OpenSSL for Qt: Qt's HTTPS uses Schannel, and its own
    # protocols link OpenSSL statically. Its translations are compiled in;
    # Qt's come from the qtbase catalogs below.
    Invoke-Native (Join-Path $QtBin 'windeployqt.exe') @(
        '--release', '--no-compiler-runtime', '--no-translations',
        '--no-opengl-sw', '--no-system-d3d-compiler', '--no-system-dxc-compiler',
        '--no-ffmpeg', '--no-quick-import',
        '--exclude-plugins', 'qopensslbackend,qcertonlybackend',
        '--dir', $stage, (Join-Path $stage 'OpenSCP.exe'))

    # main.cpp loads qtbase_<language>.qm, for the languages OpenSCP has.
    $translations = Join-Path $stage 'translations'
    New-Item -ItemType Directory -Force $translations | Out-Null
    foreach ($catalog in 'qtbase_de.qm', 'qtbase_es.qm', 'qtbase_fr.qm', 'qtbase_pt_BR.qm') {
        Copy-Item -Path (Join-Path $QtBin "..\translations\$catalog") -Destination $translations
    }

    Write-Log 'Copying the Visual C++ runtime'
    Copy-VcRuntime $stage

    $licenses = Join-Path $stage 'licenses'
    New-Item -ItemType Directory -Force $licenses | Out-Null
    $credits = Join-Path $RepoDir 'docs\credits'
    Copy-Item -Path (Join-Path $credits 'LICENSES\*') -Destination $licenses
    Copy-Item -Path (Join-Path $credits 'CREDITS.md'), (Join-Path $credits 'LGPL_COMPLIANCE.md') `
        -Destination $licenses
    Copy-Item -Path (Join-Path $RepoDir 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
    return $stage
}

function Write-Checksum([string]$Artifact) {
    $hash = (Get-FileHash -Algorithm SHA256 -Path $Artifact).Hash.ToLowerInvariant()
    $name = Split-Path -Leaf $Artifact
    [IO.File]::WriteAllText("$Artifact.sha256", "$hash  $name`n")
    Write-Log "Wrote $name"
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    Stop-WithError 'Windows packages must be built on Windows.'
}

$buildDir = Get-EnvValue 'BUILD_DIR' (Join-Path $RepoDir 'build-windows-package')
if (-not [IO.Path]::IsPathRooted($buildDir)) { $buildDir = Join-Path $RepoDir $buildDir }
$jobs = Get-EnvValue 'JOBS' ([string][Environment]::ProcessorCount)
$formats = (Get-EnvValue 'PACKAGE_FORMATS' 'setup,msi').Split(',') |
    ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ }
foreach ($format in $formats) {
    if ($format -notin 'setup', 'msi') { Stop-WithError "Unknown package format: $format" }
}
$distDir = Join-Path $RepoDir 'dist'

Enter-MsvcEnvironment
$configure = Get-ConfigureArguments $buildDir 'Release' (Get-EnvValue 'VCPKG_TARGET_TRIPLET' 'x64-windows-static-md')
$configure += @(
    "-DOPENSCP_UPDATE_PUBLIC_KEY=$(Get-EnvValue 'OPENSCP_UPDATE_PUBLIC_KEY')",
    "-DOPENSCP_ENFORCE_RECOMMENDED_QT_VERSION=$(Get-EnvValue 'OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION' 'OFF')"
)
Write-Log "Configuring Release build: $buildDir"
Invoke-Native 'cmake' $configure
Write-Log 'Building OpenSCP'
Invoke-Native 'cmake' @('--build', $buildDir, '--parallel', $jobs, '--target', 'openscp')

$qtBin = Find-QtBinDirectory $buildDir
if (-not $qtBin) { Stop-WithError "Qt's bin folder was not found from $buildDir." }
$version = Get-AppVersion $buildDir
$stage = New-Stage $buildDir $qtBin

Write-Log 'Checking the staged files'
& (Join-Path $RepoDir 'scripts\verify\windows-package.ps1') -Directory $stage
if ($LASTEXITCODE -ne 0) { Stop-WithError 'The staged application failed its checks.' }

New-Item -ItemType Directory -Force $distDir | Out-Null
$packaging = Join-Path $RepoDir 'packaging\windows'
if ('setup' -in $formats) {
    Write-Log 'Building the setup'
    Invoke-Native (Find-Iscc) @('/Q', "/DAppVersion=$version", "/DStageDir=$stage",
        "/DOutputDir=$distDir", (Join-Path $packaging 'OpenSCP.iss'))
    Write-Checksum (Join-Path $distDir "OpenSCP-$version-x64-setup.exe")
}
if ('msi' -in $formats) {
    Write-Log 'Building the MSI'
    $msi = Join-Path $distDir "OpenSCP-$version-x64.msi"
    Invoke-Native (Get-EnvValue 'OPENSCP_WIX' 'wix') @('build', '-arch', 'x64', '-nologo',
        '-d', "Version=$version", '-d', "StageDir=$stage",
        '-d', "IconFile=$(Join-Path $RepoDir 'assets\windows\OpenSCP.ico')",
        '-o', $msi, (Join-Path $packaging 'OpenSCP.wxs'))
    # wix also writes a .wixpdb with build details, which is not shipped.
    Remove-Item -Force -ErrorAction SilentlyContinue ([IO.Path]::ChangeExtension($msi, '.wixpdb'))
    Write-Checksum $msi
}
