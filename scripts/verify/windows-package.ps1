<#
.SYNOPSIS
    Check a staged Windows OpenSCP before it goes into the setup and the MSI.

.DESCRIPTION
    Every executable and DLL must be x64 and built with ASLR, high-entropy
    ASLR and DEP; OpenSCP.exe also with Control Flow Guard and CET shadow
    stacks. No binary may need a debug C runtime or a DLL that is neither
    staged nor part of Windows, and the Qt plugins and translations that
    OpenSCP loads must be there.

.PARAMETER Directory
    The staged application folder, which holds OpenSCP.exe.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Directory
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot '..\lib\windows\pe.ps1')

$problems = [Collections.Generic.List[string]]::new()
$root = (Resolve-Path $Directory).Path
$system = Join-Path $env:SystemRoot 'System32'
$staged = @{}
Get-ChildItem -Path $root -File | ForEach-Object { $staged[$_.Name.ToLowerInvariant()] = $true }

$required = @(
    'OpenSCP.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
    'Qt6Svg.dll', 'platforms\qwindows.dll', 'styles\qmodernwindowsstyle.dll',
    'imageformats\qsvg.dll', 'iconengines\qsvgicon.dll', 'tls\qschannelbackend.dll',
    'translations\qtbase_de.qm', 'translations\qtbase_es.qm',
    'translations\qtbase_fr.qm', 'translations\qtbase_pt_BR.qm',
    'licenses\CREDITS.md', 'licenses\LGPL_COMPLIANCE.md', 'LICENSE.txt'
)
foreach ($file in $required) {
    if (-not (Test-Path (Join-Path $root $file) -PathType Leaf)) {
        $problems.Add("Missing $file")
    }
}

$binaries = Get-ChildItem -Path $root -Recurse -File -Include '*.exe', '*.dll'
foreach ($binary in $binaries) {
    $name = $binary.FullName.Substring($root.Length + 1)
    $image = Read-PeImage $binary.FullName
    if ($image.Machine -ne $PeMachineAmd64) {
        $problems.Add("$name is not an x64 image")
    }
    foreach ($flag in @(
            @($PeDynamicBase, 'ASLR (/DYNAMICBASE)'),
            @($PeHighEntropyVa, 'high-entropy ASLR (/HIGHENTROPYVA)'),
            @($PeNxCompat, 'DEP (/NXCOMPAT)'))) {
        if (($image.DllCharacteristics -band $flag[0]) -eq 0) {
            $problems.Add("$name is built without $($flag[1])")
        }
    }
    if ($name -eq 'OpenSCP.exe') {
        if (($image.DllCharacteristics -band $PeGuardCf) -eq 0) {
            $problems.Add('OpenSCP.exe is built without Control Flow Guard (/guard:cf)')
        }
        if (-not $image.CetCompatible) {
            $problems.Add('OpenSCP.exe is built without CET shadow stacks (/CETCOMPAT)')
        }
    }
    foreach ($import in $image.Imports) {
        $lower = $import.ToLowerInvariant()
        if ($lower -match '^(ucrtbased|vcruntime140(_1)?d|msvcp140(_\w+)?d|concrt140d)\.dll$') {
            $problems.Add("$name needs the debug runtime $import")
        } elseif ($lower -match '^((vcruntime|msvcp|msvcr|concrt|vccorlib|vcomp|vcamp)\d|(mfcm?|atl)1\d\d)') {
            # A Visual C++ redistributable on this computer puts these in
            # System32, but a clean Windows does not have them: the C and C++
            # runtimes of every version, OpenMP, C++ AMP, MFC and ATL.
            if (-not $staged.ContainsKey($lower)) {
                $problems.Add("$name needs $import, which the package must include")
            }
        } elseif (-not ($staged.ContainsKey($lower) -or $lower -match '^(api|ext)-ms-' -or
                (Test-Path (Join-Path $system $import)))) {
            # Windows finds a plugin's DLLs in the application folder.
            $problems.Add("$name needs $import, which is neither staged nor part of Windows")
        }
    }
}

if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Host "[fail] $_" -ForegroundColor Red }
    exit 1
}
Write-Host "[ok  ] $($binaries.Count) binaries in $root passed the package checks." -ForegroundColor Green
# The caller reads $LASTEXITCODE, which a script that ends without exit leaves
# as the last native command set it.
exit 0
