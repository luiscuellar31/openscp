<#
.SYNOPSIS
    Check the repository's PowerShell scripts.

.DESCRIPTION
    Windows counterpart of scripts/checks/shell-quality.sh. Parses every
    PowerShell script under scripts\, exercises the help path of the
    user-facing commands, and runs PSScriptAnalyzer error diagnostics when
    PSScriptAnalyzer is installed, as it is on GitHub's Windows runners.
#>
param(
    [Parameter(Position = 0)]
    [string]$Command = ''
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

if ($Command -in 'help', '-h', '--help') {
    Get-Help $PSCommandPath -Detailed
    exit 0
}
if ($Command) {
    Write-Output "Unknown argument: $Command. Run with 'help' for the usage."
    exit 2
}

$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$scriptsDir = Join-Path $repository 'scripts'
$scripts = @(Get-ChildItem -Path $scriptsDir -Recurse -File -Filter '*.ps1' | Sort-Object FullName)
if ($scripts.Count -eq 0) {
    Write-Output 'No PowerShell scripts were found.'
    exit 1
}

function Get-RelativePath([string]$Path) {
    return $Path.Substring($repository.Length + 1)
}

$failed = $false
foreach ($file in $scripts) {
    $tokens = $null
    $problems = $null
    [void][Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens,
        [ref]$problems)
    foreach ($problem in $problems) {
        Write-Output ('{0}:{1}:{2}: {3}' -f (Get-RelativePath $file.FullName),
            $problem.Extent.StartLineNumber, $problem.Extent.StartColumnNumber, $problem.Message)
        $failed = $true
    }
}

# In a PowerShell of their own, as users start them.
$powershell = (Get-Process -Id $PID).Path
foreach ($userScript in 'scripts\windows.ps1', 'scripts\package\windows.ps1') {
    $path = Join-Path $repository $userScript
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $path help | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Output "$userScript help exited with code $LASTEXITCODE."
        $failed = $true
    }
}

if (Get-Module -ListAvailable -Name PSScriptAnalyzer) {
    $findings = @(Invoke-ScriptAnalyzer -Path $scriptsDir -Recurse -Severity Error, ParseError)
    foreach ($finding in $findings) {
        Write-Output ('{0}:{1}: {2}: {3}' -f (Get-RelativePath $finding.ScriptPath), $finding.Line,
            $finding.RuleName, $finding.Message)
        $failed = $true
    }
} else {
    Write-Output 'PSScriptAnalyzer not found; skipped PSScriptAnalyzer diagnostics.'
}

if ($failed) {
    exit 1
}
Write-Output "Checked $($scripts.Count) PowerShell scripts."
