# Build settings shared by scripts\windows.ps1 and scripts\package\windows.ps1.
# Dot-source it after setting $RepoDir and $LogPrefix.

$RecommendedQtVersion = '6.11.2'
$QtCompilerDir = 'msvc2022_64'

function Write-Log([string]$Message) {
    Write-Host "$LogPrefix $Message" -ForegroundColor Cyan
}

function Stop-WithError([string]$Message) {
    Write-Host "[err ] $Message" -ForegroundColor Red
    exit 1
}

function Get-EnvValue([string]$Name, [string]$Default = '') {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrEmpty($value)) { return $Default }
    return $value
}

function Invoke-Native([string]$FilePath, [string[]]$Arguments) {
    # To the console, not into the result of the calling function.
    & $FilePath @Arguments | Out-Host
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

# The CMake arguments that select the compiler toolchain, vcpkg and Qt for a
# build directory.
function Get-ConfigureArguments([string]$BuildDir, [string]$BuildType, [string]$Triplet) {
    $arguments = @(
        '-S', $RepoDir,
        '-B', $BuildDir,
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_TOOLCHAIN_FILE=$(Resolve-VcpkgToolchain)",
        "-DVCPKG_TARGET_TRIPLET=$Triplet"
    )
    # CMake keeps the generator a cache was created with.
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Get-EnvValue 'CMAKE_GENERATOR') -and -not (Test-Path $cache)) {
        $arguments += @('-G', 'Ninja')
    }
    $qtPrefix = Resolve-QtPrefix
    if ($qtPrefix) {
        $arguments += "-DCMAKE_PREFIX_PATH=$qtPrefix"
    }
    return $arguments
}

# The bin folder of the Qt that a configured build directory uses.
function Find-QtBinDirectory([string]$BuildDir) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
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
