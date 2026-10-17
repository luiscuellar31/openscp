<#
.SYNOPSIS
    Configure, build, run and test OpenSCP on Windows.

.DESCRIPTION
    Windows counterpart of scripts/linux.sh and scripts/macos.sh. It uses the
    newest Visual Studio C++ tools, Qt from the Qt installer and the native
    libraries from vcpkg (vcpkg.json at the repository root).

    Run .\scripts\windows.ps1 help for the commands and variables.
#>
param(
    [Parameter(Position = 0)]
    [string]$Command = 'help'
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$RepoDir = Split-Path -Parent $PSScriptRoot
$LogPrefix = '[windows]'
. (Join-Path $PSScriptRoot 'lib\windows\build.ps1')

function Show-Usage {
    @'
Usage: .\scripts\windows.ps1 <command>

Commands:
  configure   Configure the build directory
  build       Build the OpenSCP application
  run         Run the previously built application
  dev         Configure + build + run
  test        Configure with tests, build them and run CTest
  help        Show this help

Optional environment variables:
  BUILD_DIR=C:\path\to\build           Default: build
  BUILD_TYPE=Release|Debug|...         Default: Release
  JOBS=<number>                        Parallel build jobs (default: CPU count)
  CMAKE_GENERATOR=Ninja|...            Default: Ninja for a new build directory
  QT_PREFIX=C:\Qt\<ver>\msvc2022_64    Qt installation prefix
  Qt6_DIR=<prefix>\lib\cmake\Qt6       Qt package directory
  VCPKG_ROOT=C:\path\to\vcpkg          vcpkg checkout that builds the libraries
  VCPKG_TARGET_TRIPLET=<triplet>       Default: x64-windows-static-md

If no Qt path is provided, the script uses the newest Qt 6 under C:\Qt that
has an msvc2022_64 build, preferring the release baseline.

Examples:
  .\scripts\windows.ps1 dev
  $env:BUILD_TYPE = 'Debug'; .\scripts\windows.ps1 dev
  $env:JOBS = '8'; .\scripts\windows.ps1 test
'@ | Write-Host
}

function Get-BuildSettings {
    $buildDir = Get-EnvValue 'BUILD_DIR' (Join-Path $RepoDir 'build')
    if (-not [IO.Path]::IsPathRooted($buildDir)) {
        $buildDir = Join-Path $RepoDir $buildDir
    }
    $buildType = Get-EnvValue 'BUILD_TYPE' 'Release'
    $jobs = Get-EnvValue 'JOBS' ([string][Environment]::ProcessorCount)
    if ($jobs -notmatch '^[1-9][0-9]*$') {
        Stop-WithError 'JOBS must be a positive integer.'
    }
    return [pscustomobject]@{
        BuildDir  = $buildDir
        BuildType = $buildType
        Jobs      = $jobs
        Triplet   = Get-EnvValue 'VCPKG_TARGET_TRIPLET' 'x64-windows-static-md'
    }
}

function Invoke-Configure($Settings, [bool]$WithTests) {
    $arguments = Get-ConfigureArguments $Settings.BuildDir $Settings.BuildType $Settings.Triplet
    if ($WithTests) {
        $arguments += '-DOPENSCP_BUILD_TESTS=ON'
    }

    Write-Log "Configuring $($Settings.BuildType) build: $($Settings.BuildDir)"
    Invoke-Native 'cmake' $arguments
}

function Invoke-Build($Settings, [string[]]$Targets) {
    Write-Log "Building $($Targets -join ', ')"
    Invoke-Native 'cmake' (@('--build', $Settings.BuildDir, '--parallel', $Settings.Jobs,
            '--target') + $Targets)
}

function Invoke-Run($Settings) {
    $exe = Join-Path $Settings.BuildDir 'OpenSCP.exe'
    if (-not (Test-Path $exe)) {
        Stop-WithError "Application not found in $($Settings.BuildDir). Run '.\scripts\windows.ps1 build' first."
    }
    # The build tree has no deployed Qt; the application loads it from the
    # Qt installation.
    $qtBin = Find-QtBinDirectory $Settings.BuildDir
    if ($qtBin) { $env:PATH = "$qtBin;$env:PATH" }

    Write-Log "Running $exe"
    Start-Process -FilePath $exe -WorkingDirectory $Settings.BuildDir
}

function Invoke-Tests($Settings) {
    Invoke-Configure $Settings $true
    Invoke-Build $Settings @('openscp', 'openscp_test_binaries')
    Write-Log 'Running CTest'
    Invoke-Native 'ctest' @('--test-dir', $Settings.BuildDir, '--output-on-failure',
        '--parallel', $Settings.Jobs)
}

switch ($Command) {
    { $_ -in 'help', '-h', '--help' } { Show-Usage; exit 0 }
    { $_ -in 'configure', 'build', 'run', 'dev', 'test' } { break }
    default { Show-Usage; Stop-WithError "Unknown command: $Command" }
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    Stop-WithError 'This development flow must run on Windows.'
}

$settings = Get-BuildSettings
Enter-MsvcEnvironment

switch ($Command) {
    'configure' { Invoke-Configure $settings $false }
    'build' { Invoke-Build $settings @('openscp') }
    'run' { Invoke-Run $settings }
    'dev' {
        Invoke-Configure $settings $false
        Invoke-Build $settings @('openscp')
        Invoke-Run $settings
    }
    'test' { Invoke-Tests $settings }
}
