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
$RecommendedQtVersion = '6.11.2'
$QtCompilerDir = 'msvc2022_64'

function Write-Log([string]$Message) {
    Write-Host "[windows] $Message" -ForegroundColor Cyan
}

function Stop-WithError([string]$Message) {
    Write-Host "[err ] $Message" -ForegroundColor Red
    exit 1
}

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

function Get-EnvValue([string]$Name, [string]$Default = '') {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrEmpty($value)) { return $Default }
    return $value
}

function Invoke-Native([string]$FilePath, [string[]]$Arguments) {
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        Stop-WithError "$FilePath exited with code $LASTEXITCODE."
    }
}

# Captured before the developer shell starts, because Visual Studio's own
# vcpkg sets VCPKG_ROOT inside that shell.
$UserVcpkgRoot = Get-EnvValue 'VCPKG_ROOT'

function Enter-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Stop-WithError 'Visual Studio was not found. Install Visual Studio 2022 or its Build Tools with the "Desktop development with C++" workload.'
    }
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ([string]::IsNullOrEmpty($vsPath)) {
        Stop-WithError 'No Visual Studio installation has the x64 C++ tools.'
    }
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64 -no_logo' | Out-Null
}

function Resolve-QtPrefix {
    $prefix = Get-EnvValue 'QT_PREFIX'
    if ($prefix) { return $prefix }

    $qt6Dir = Get-EnvValue 'Qt6_DIR'
    if ($qt6Dir) { return (Resolve-Path (Join-Path $qt6Dir '..\..\..')).Path }

    $candidates = @(Get-ChildItem -Path 'C:\Qt' -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^6(\.\d+)+$' -and
            (Test-Path (Join-Path $_.FullName "$QtCompilerDir\lib\cmake\Qt6\Qt6Config.cmake")) } |
        Sort-Object { [version]$_.Name } -Descending)
    $recommended = $candidates | Where-Object { $_.Name -eq $RecommendedQtVersion }
    if ($recommended) { return (Join-Path $recommended.FullName $QtCompilerDir) }
    if ($candidates.Count -gt 0) {
        return (Join-Path $candidates[0].FullName $QtCompilerDir)
    }
    return ''
}

function Resolve-VcpkgToolchain {
    if (-not $UserVcpkgRoot) {
        Stop-WithError 'Set VCPKG_ROOT to a vcpkg checkout; it builds libssh2, OpenSSL, libcurl and tinyxml2 from vcpkg.json.'
    }
    $toolchain = Join-Path $UserVcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path $toolchain)) {
        Stop-WithError "VCPKG_ROOT does not contain scripts\buildsystems\vcpkg.cmake: $UserVcpkgRoot"
    }
    return $toolchain
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
    $qtPrefix = Resolve-QtPrefix
    $arguments = @(
        '-S', $RepoDir,
        '-B', $Settings.BuildDir,
        "-DCMAKE_BUILD_TYPE=$($Settings.BuildType)",
        "-DCMAKE_TOOLCHAIN_FILE=$(Resolve-VcpkgToolchain)",
        "-DVCPKG_TARGET_TRIPLET=$($Settings.Triplet)"
    )
    # CMake keeps the generator a cache was created with.
    $cache = Join-Path $Settings.BuildDir 'CMakeCache.txt'
    if (-not (Get-EnvValue 'CMAKE_GENERATOR') -and -not (Test-Path $cache)) {
        $arguments += @('-G', 'Ninja')
    }
    if ($qtPrefix) {
        $arguments += "-DCMAKE_PREFIX_PATH=$qtPrefix"
    }
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

function Find-QtBinDirectory($Settings) {
    $cache = Join-Path $Settings.BuildDir 'CMakeCache.txt'
    if (Test-Path $cache) {
        $line = Select-String -Path $cache -Pattern '^Qt6Core_DIR:PATH=(.+)$' |
            Select-Object -First 1
        if ($line) {
            $bin = Join-Path $line.Matches[0].Groups[1].Value '..\..\..\bin'
            if (Test-Path $bin) { return (Resolve-Path $bin).Path }
        }
    }
    return ''
}

function Invoke-Run($Settings) {
    $exe = Join-Path $Settings.BuildDir 'OpenSCP.exe'
    if (-not (Test-Path $exe)) {
        Stop-WithError "Application not found in $($Settings.BuildDir). Run '.\scripts\windows.ps1 build' first."
    }
    # The build tree has no deployed Qt; the application loads it from the
    # Qt installation.
    $qtBin = Find-QtBinDirectory $Settings
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
