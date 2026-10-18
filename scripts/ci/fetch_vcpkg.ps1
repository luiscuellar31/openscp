<#
.SYNOPSIS
    Fetch vcpkg at the builtin-baseline of vcpkg.json for a CI job.

.DESCRIPTION
    Checks out only that commit of microsoft/vcpkg into $env:RUNNER_TEMP\vcpkg,
    bootstraps it, and sets VCPKG_ROOT and VCPKG_DEFAULT_BINARY_CACHE for the
    next steps. The step output `msvc` gives the Visual Studio version, which
    binary cache keys include because vcpkg rebuilds what another compiler
    built.
#>
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$FilePath, [string[]]$Arguments) {
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE."
    }
}

$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$baseline = (Get-Content (Join-Path $repository 'vcpkg.json') -Raw |
        ConvertFrom-Json).'builtin-baseline'
$root = Join-Path $env:RUNNER_TEMP 'vcpkg'
Invoke-Checked 'git' @('init', '--quiet', $root)
# Some vcpkg test ports go past the 260-character path limit.
Invoke-Checked 'git' @('-C', $root, 'config', 'core.longpaths', 'true')
Invoke-Checked 'git' @('-C', $root, 'fetch', '--quiet', '--depth', '1',
    'https://github.com/microsoft/vcpkg.git', $baseline)
Invoke-Checked 'git' @('-C', $root, 'checkout', '--quiet', 'FETCH_HEAD')
Invoke-Checked (Join-Path $root 'bootstrap-vcpkg.bat') @('-disableMetrics')

$archives = Join-Path $env:RUNNER_TEMP 'vcpkg-archives'
New-Item -ItemType Directory -Force $archives | Out-Null
"VCPKG_ROOT=$root" >> $env:GITHUB_ENV
"VCPKG_DEFAULT_BINARY_CACHE=$archives" >> $env:GITHUB_ENV

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msvc = & $vswhere -latest -products * -property installationVersion
"msvc=$msvc" >> $env:GITHUB_OUTPUT
