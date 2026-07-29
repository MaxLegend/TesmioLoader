$ErrorActionPreference = "Stop"

$launcher = Join-Path $PSScriptRoot "..\build\tesmiolauncher.exe"
$loader = Join-Path $PSScriptRoot "..\build\tesmioloader.dll"

if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
    throw "Launcher not found at '$launcher'. Run build.bat first."
}

if (-not (Test-Path -LiteralPath $loader -PathType Leaf)) {
    throw "Loader not found at '$loader'. Run build.bat first."
}

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) "tesmiolauncher-path-fallback-$([guid]::NewGuid())"
$launcherDirectory = Join-Path $testRoot "tesmioloader\build"
$gameDirectory = Join-Path $testRoot "game"
$explicitDirectory = Join-Path $testRoot "explicit"

function Invoke-TestLauncher {
    param([string[]] $LauncherArguments = @())

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = Join-Path $launcherDirectory "tesmiolauncher.exe"
    $startInfo.WorkingDirectory = $gameDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Arguments = ($LauncherArguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join " "

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo

    try {
        $process.Start() | Out-Null
        $output = $process.StandardOutput.ReadToEnd() + $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        return $output
    }
    finally {
        $process.Dispose()
    }
}

function Assert-Contains {
    param(
        [string] $Output,
        [string] $Expected,
        [string] $Failure
    )

    if (-not $Output.Contains($Expected)) {
        throw "$Failure`nExpected: $Expected`nOutput:`n$Output"
    }
}

try {
    New-Item -ItemType Directory -Path $launcherDirectory, $gameDirectory, $explicitDirectory | Out-Null
    Copy-Item -LiteralPath $launcher -Destination $launcherDirectory
    Copy-Item -LiteralPath $loader -Destination $launcherDirectory

    $currentGame = Join-Path $gameDirectory "SOVIET64.exe"
    New-Item -ItemType File -Path $currentGame | Out-Null

    $output = Invoke-TestLauncher
    Assert-Contains $output "[tesmiolauncher] game $currentGame" `
        "Launcher did not fall back to SOVIET64.exe in the current directory."

    $legacyGame = Join-Path $testRoot "SOVIET64.exe"
    New-Item -ItemType File -Path $legacyGame | Out-Null

    $output = Invoke-TestLauncher
    Assert-Contains $output "[tesmiolauncher] game $legacyGame" `
        "Launcher did not preserve the existing nested-layout default."

    $explicitGame = Join-Path $explicitDirectory "SOVIET64.exe"
    New-Item -ItemType File -Path $explicitGame | Out-Null

    $output = Invoke-TestLauncher @("--game", $explicitGame)
    Assert-Contains $output "[tesmiolauncher] game $explicitGame" `
        "Launcher did not honor an existing explicit --game path."

    $missingExplicitGame = Join-Path $explicitDirectory "missing-SOVIET64.exe"
    $output = Invoke-TestLauncher @("--game", $missingExplicitGame)
    Assert-Contains $output "[tesmiolauncher] game not found: $missingExplicitGame" `
        "Launcher fell back instead of reporting a missing explicit --game path."
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
