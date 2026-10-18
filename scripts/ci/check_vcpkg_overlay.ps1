<#
.SYNOPSIS
    Warn when the OpenSSL overlay port strays from vcpkg's own port.

.DESCRIPTION
    vcpkg-ports/openssl is vcpkg's openssl port from the builtin-baseline
    commit with another version (vcpkg-ports/README.md). This compares it
    with the port in $env:VCPKG_ROOT, which scripts\ci\fetch_vcpkg.ps1 checks
    out at that commit. The version, the port version and the SHA512 of the
    sources do not count. Every other difference becomes a GitHub Actions
    warning, because copying the port again is a maintainer's task; the
    script fails only when it cannot compare.
#>
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

if (-not $env:VCPKG_ROOT) {
    throw 'VCPKG_ROOT is not set; run scripts\ci\fetch_vcpkg.ps1 first.'
}
$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$overlay = Join-Path $repository 'vcpkg-ports\openssl'
$upstream = Join-Path $env:VCPKG_ROOT 'ports\openssl'
if (-not (Test-Path $upstream -PathType Container)) {
    throw "vcpkg has no openssl port in $upstream."
}

# The paths of a port's files, relative to the port and with forward slashes.
function Get-PortFiles([string]$Port) {
    $prefix = (Get-Item -LiteralPath $Port).FullName.TrimEnd('\') + '\'
    Get-ChildItem -LiteralPath $Port -Recurse -File -Force |
        ForEach-Object { $_.FullName.Substring($prefix.Length).Replace('\', '/') }
}

# A port file's text without what the overlay changes on purpose.
function Get-ComparableText([string]$Port, [string]$File) {
    $text = [IO.File]::ReadAllText((Join-Path $Port $File)).Replace("`r`n", "`n")
    if ($File -ceq 'vcpkg.json') {
        $text = $text -creplace '(?m)^  "(version|port-version)": [^\n]*\n', ''
    } elseif ($File -ceq 'portfile.cmake') {
        $text = $text -creplace 'SHA512 [0-9a-f]{128}', 'SHA512'
    }
    return $text
}

$overlayFiles = @(Get-PortFiles $overlay)
$upstreamFiles = @(Get-PortFiles $upstream)
$theirs = "vcpkg's openssl port at the baseline"
$advice = 'Copy the port again as vcpkg-ports/README.md explains.'
$differences = 0
foreach ($file in @($overlayFiles + $upstreamFiles | Sort-Object -Unique)) {
    $annotation = "::warning file=vcpkg-ports/openssl/${file}::"
    if ($file -notin $upstreamFiles) {
        $message = "$theirs has no $file."
    } elseif ($file -notin $overlayFiles) {
        $annotation = '::warning::'
        $message = "$theirs has $file, which vcpkg-ports/openssl lacks."
    } elseif ((Get-ComparableText $overlay $file) -cne (Get-ComparableText $upstream $file)) {
        $message = "$theirs has a different $file."
    } else {
        continue
    }
    Write-Output "$annotation$message $advice"
    $differences++
}
if ($differences -eq 0) {
    Write-Output "vcpkg-ports/openssl matches $theirs, apart from the version and the SHA512."
}
