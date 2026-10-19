<#
.SYNOPSIS
    Run the SFTP integration test on Windows against a local test server.

.DESCRIPTION
    Installs the pinned paramiko (scripts\ci\sftp-test-server-requirements.txt)
    into a virtual environment and starts scripts\ci\sftp_test_server.py twice
    on 127.0.0.1: an SFTP server, which also holds an account that asks for a
    one-time code after the password, and in front of it a jump host. The test
    then runs once directly and once through the jump host, which OpenSCP
    reaches with Windows' ssh.exe. A skipped test counts as a failure here,
    since the servers are meant to be there.

.PARAMETER BuildDir
    The configured build directory that holds the test. Default: build

.PARAMETER Work
    Folder for the servers' files and the virtual environment.
    Default: openscp-sftp in $env:RUNNER_TEMP, or in the temporary folder
    outside GitHub Actions.

.PARAMETER KnownHosts
    The known_hosts file through which ssh.exe checks the jump host, to
    which the script adds it. Default on a GitHub runner: the user's own.
    Elsewhere the jump host is tested only when this is given, so that the
    script never changes a developer's known_hosts.
#>
param(
    [string]$BuildDir = 'build',
    [string]$Work = '',
    [string]$KnownHosts = ''
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$RepoDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$LogPrefix = '[sftp]'
. (Join-Path $RepoDir 'scripts\lib\windows\build.ps1')

if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $RepoDir $BuildDir }
$test = Join-Path $BuildDir 'tests\openscp_sftp_integration_tests.exe'
if (-not (Test-Path $test -PathType Leaf)) {
    $test = Join-Path $BuildDir 'tests\Release\openscp_sftp_integration_tests.exe'
}
if (-not (Test-Path $test -PathType Leaf)) { Stop-WithError "The SFTP integration test was not built in $BuildDir." }

if (-not $Work) {
    $temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    $Work = Join-Path $temp 'openscp-sftp'
}
New-Item -ItemType Directory -Force $Work | Out-Null
$venv = Join-Path $Work 'venv'
Write-Log 'Installing the test server'
Invoke-Native 'python' @('-m', 'venv', $venv)
$python = Join-Path $venv 'Scripts\python.exe'
Invoke-Native $python @('-m', 'pip', 'install', '--disable-pip-version-check', '--quiet',
    '--require-hashes', '--only-binary=:all:',
    '-r', (Join-Path $PSScriptRoot 'sftp-test-server-requirements.txt'))

$keys = Join-Path $Work 'keys'
$root = Join-Path $Work 'root'
New-Item -ItemType Directory -Force (Join-Path $root 'upload') | Out-Null

function Start-TestServer([int]$Port, [string[]]$Extra) {
    # Start-Process joins the arguments with spaces and quotes none of them.
    $arguments = @('-I', (Join-Path $PSScriptRoot 'sftp_test_server.py'),
        '--port', $Port, '--root', $root, '--keys', $keys) + $Extra |
        ForEach-Object { if ("$_" -match '\s') { "`"$_`"" } else { "$_" } }
    $server = Start-Process -FilePath $python -ArgumentList $arguments -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $Work "server-$Port.out") `
        -RedirectStandardError (Join-Path $Work "server-$Port.log")
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        if ($server.HasExited) { break }
        $client = [Net.Sockets.TcpClient]::new()
        try {
            $client.Connect('127.0.0.1', $Port)
            return $server
        } catch {
            Start-Sleep -Milliseconds 250
        } finally {
            $client.Dispose()
        }
    }
    Get-Content (Join-Path $Work "server-$Port.log") -ErrorAction SilentlyContinue
    Stop-WithError "The test server on port $Port did not start."
}

$servers = @()
try {
    # The first server makes the keys, which the jump host then shares. It
    # also holds an account that asks for a one-time code after the password,
    # made for each run so that no password is written here.
    $twoFactor = @{
        User = 'openscp_2fa'
        Password = [guid]::NewGuid().ToString('N')
        Code = '135790'
    }
    $servers += Start-TestServer 2222 @('--two-factor',
        "$($twoFactor.User):$($twoFactor.Password):$($twoFactor.Code)")
    $servers += Start-TestServer 2223 @('--forward')

    # Windows' ssh.exe refuses a private key that other users can read.
    $clientKey = Join-Path $keys 'client_ed25519'
    Invoke-Native 'icacls' @($clientKey, '/inheritance:r', '/grant:r', "$($env:USERNAME):F")

    $qtBin = Find-QtBinDirectory $BuildDir
    if ($qtBin) { $env:PATH = "$qtBin;$env:PATH" }
    $env:OPENSCP_IT_SFTP_HOST = '127.0.0.1'
    $env:OPENSCP_IT_SFTP_PORT = '2222'
    $env:OPENSCP_IT_SFTP_USER = 'openscp_it'
    $env:OPENSCP_IT_SFTP_KEY = $clientKey
    $env:OPENSCP_IT_REMOTE_BASE = '/upload'
    $env:OPENSCP_IT_2FA_USER = $twoFactor.User
    $env:OPENSCP_IT_2FA_PASS = $twoFactor.Password
    $env:OPENSCP_IT_2FA_CODE = $twoFactor.Code

    Write-Log 'Testing SFTP directly'
    Invoke-Native $test @()

    if (-not $KnownHosts -and $env:GITHUB_ACTIONS -eq 'true') {
        $KnownHosts = Join-Path $env:USERPROFILE '.ssh\known_hosts'
    }
    if (-not $KnownHosts) {
        Write-Log 'Skipped the jump host: pass -KnownHosts outside GitHub Actions.'
        return
    }
    # OpenSCP runs ssh.exe in batch mode, which refuses an unknown host.
    New-Item -ItemType Directory -Force (Split-Path -Parent $KnownHosts) | Out-Null
    $hostKey = (Get-Content (Join-Path $keys 'host_ed25519.pub') -Raw).Split(' ')[0..1] -join ' '
    $separator = ''
    if ((Test-Path $KnownHosts) -and -not [IO.File]::ReadAllText($KnownHosts).EndsWith("`n") -and
        (Get-Item $KnownHosts).Length -gt 0) {
        $separator = "`n"
    }
    [IO.File]::AppendAllText($KnownHosts, "${separator}[127.0.0.1]:2223 $hostKey`n")

    Write-Log 'Testing SFTP through the jump host'
    $env:OPENSCP_IT_JUMP_HOST = '127.0.0.1'
    $env:OPENSCP_IT_JUMP_PORT = '2223'
    $env:OPENSCP_IT_JUMP_USER = 'openscp_it'
    $env:OPENSCP_IT_JUMP_KEY = $clientKey
    Invoke-Native $test @()
} finally {
    foreach ($server in $servers) {
        if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    }
}
