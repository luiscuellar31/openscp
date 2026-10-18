<#
.SYNOPSIS
    Install the pinned Inno Setup and WiX for a Windows packaging job.

.DESCRIPTION
    Installs Inno Setup 6.7.3, checked against its pinned SHA-256, for the
    current user in $env:RUNNER_TEMP\InnoSetup, and WiX 5.0.2 as a .NET global
    tool. ISCC and OPENSCP_WIX, which scripts\package\windows.ps1 reads, go to
    GITHUB_ENV for the next steps.
#>
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$FilePath, [string[]]$Arguments) {
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE."
    }
}

$installer = Join-Path $env:RUNNER_TEMP 'innosetup-6.7.3.exe'
Invoke-Checked 'curl.exe' @('--fail', '--location', '--proto', '=https', '--silent',
    '--show-error', '--output', $installer,
    'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe')
$expected = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
if ((Get-FileHash -Algorithm SHA256 $installer).Hash.ToLowerInvariant() -ne $expected) {
    throw 'The Inno Setup installer does not match its pinned SHA-256.'
}
$innoDir = Join-Path $env:RUNNER_TEMP 'InnoSetup'
$install = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER',
    "/DIR=`"$innoDir`"")
if ($install.ExitCode -ne 0) {
    throw "Inno Setup did not install: $($install.ExitCode)"
}
"ISCC=$(Join-Path $innoDir 'ISCC.exe')" >> $env:GITHUB_ENV

Invoke-Checked 'dotnet' @('tool', 'install', '--global', 'wix', '--version', '5.0.2')
Join-Path $env:USERPROFILE '.dotnet\tools' >> $env:GITHUB_PATH
# By its path: the runner's WiX v3 sets WIX to its own folder.
"OPENSCP_WIX=$(Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe')" >> $env:GITHUB_ENV
