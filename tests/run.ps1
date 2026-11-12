# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$ObsBin,
    [Parameter(Mandatory=$true)][string]$TestExe,
    [string]$WorkerExe,
    [int]$TimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
$savedPath = $env:PATH
$testDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('cp-obs-tests-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $testDirectory)
$localTest = Join-Path $testDirectory ([System.IO.Path]::GetFileName($TestExe))
$testExitCode = 1
$runBin = $ObsBin
Push-Location $ObsBin
try {
    # OBS 33's dynamic loader excludes PATH and the working directory. Keep
    # the test host beside the runtime DLLs, as obs64.exe is in deployment.
    $runtimeRoot = Join-Path $testDirectory 'OBS runtime'
    $runBin = Join-Path $runtimeRoot 'bin\64bit'
    [void](New-Item -ItemType Directory -Path $runBin, (Join-Path $runtimeRoot 'data'))
    Copy-Item -Path (Join-Path $ObsBin '*.dll') -Destination $runBin
    Copy-Item -LiteralPath (Join-Path $ObsBin '..\..\data\libobs') -Destination (Join-Path $runtimeRoot 'data\libobs') -Recurse
    $localTest = Join-Path $runBin ([System.IO.Path]::GetFileName($TestExe))
    if ($WorkerExe) {
        # Reproduce the deployed layout without replacing a live OBS worker.
        Copy-Item -LiteralPath $WorkerExe -Destination (Join-Path $runBin 'window-size-capture-worker.exe')
    }
    Copy-Item -LiteralPath $TestExe -Destination $localTest
    $env:PATH = "$runBin;$savedPath"
    # WinRT capture access resolves the executable's identity from a local path.
    $stdoutPath = Join-Path $testDirectory 'stdout.txt'
    $stderrPath = Join-Path $testDirectory 'stderr.txt'
    $testProcess = Start-Process -FilePath $localTest -WorkingDirectory $runBin -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    # Retain the native handle before waiting; otherwise Windows PowerShell can
    # return a null ExitCode for an already exited Start-Process child.
    $null = $testProcess.Handle
    if (-not $testProcess.WaitForExit($TimeoutSeconds * 1000)) {
        $testProcess.Kill()
        $testProcess.WaitForExit()
        Get-Content -LiteralPath $stdoutPath -Encoding UTF8
        Get-Content -LiteralPath $stderrPath -Encoding UTF8
        throw "Test timed out after $TimeoutSeconds seconds: $TestExe"
    }
    Get-Content -LiteralPath $stdoutPath -Encoding UTF8
    Get-Content -LiteralPath $stderrPath -Encoding UTF8
    $testExitCode = $testProcess.ExitCode
    if ($null -eq $testExitCode) {
        throw "Test process exit code is unavailable: $TestExe"
    }
} finally {
    $env:PATH = $savedPath
    Pop-Location
    Remove-Item -LiteralPath $testDirectory -Recurse -Force
}
exit $testExitCode
