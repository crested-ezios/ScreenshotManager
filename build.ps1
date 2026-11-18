#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
[CmdletBinding()]
param(
    [ValidateSet('Runtime', 'Frontend', 'Tests', 'All')]
    [string]$Target = 'All',
    [string]$OutputRoot = '',
    [string]$WindowsSdkRoot = '',
    [string]$VisualStudioPath = '',
    [ValidateSet('Auto', 'Ci', 'Install', 'Skip')]
    [string]$FrontendDependencies = 'Auto'
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repo 'build'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

$vs = $VisualStudioPath
if (($Target -in @('Runtime', 'Tests', 'All')) -and [string]::IsNullOrWhiteSpace($vs)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found.' }
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($vs)) { throw 'MSVC x64 toolset not found.' }
}

if ($Target -in @('Runtime', 'All')) {
    # Collapse PATH / Path / path duplicate keys so MSBuild ToolTask does not fail with MSB6001.
    $pathValue = (($env:Path -split ';') | Where-Object { $_ } | Select-Object -Unique) -join ';'
    foreach ($key in @('PATH', 'Path', 'path')) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
    $env:Path = $pathValue

    $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
    $toolset = if ([IO.File]::Exists((Join-Path $vs 'MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\v145\Toolset.props'))) { 'v145' } else { 'v143' }
    $nativeOutput = Join-Path $OutputRoot 'native'
    $nativeObjects = Join-Path $OutputRoot 'obj\native'
    New-Item -ItemType Directory -Path $nativeOutput, $nativeObjects -Force | Out-Null

    & $msbuild (Join-Path $repo 'ScreenshotManager.vcxproj') '/p:Configuration=Release' '/p:Platform=x64' "/p:PlatformToolset=$toolset" `
        "/p:OutDir=$nativeOutput/" "/p:IntDir=$nativeObjects/" '/nologo' '/verbosity:minimal'
    if ($LASTEXITCODE -ne 0) { throw ('ScreenshotManager runtime build failed: ' + $LASTEXITCODE) }
}

if ($Target -in @('Frontend', 'All')) {
    $resolvedDeps = $FrontendDependencies
    if ($resolvedDeps -eq 'Auto') {
        $viteBin = Join-Path $repo 'frontend\node_modules\.bin\vite.cmd'
        $resolvedDeps = if (Test-Path -LiteralPath $viteBin -PathType Leaf) { 'Skip' } else { 'Ci' }
    }
    $uiStageDir = Join-Path $OutputRoot 'native\ScreenshotManager.UI'
    $global:LASTEXITCODE = 0
    & (Join-Path $repo 'frontend\build.ps1') -Dependencies $resolvedDeps -StageDir $uiStageDir -AllowedStageRoot $OutputRoot
    if ($LASTEXITCODE -ne 0) { throw ('ScreenshotManager frontend build failed: ' + $LASTEXITCODE) }
}

if ($Target -in @('Tests', 'All')) {
    & (Join-Path $repo 'tests\run_tests.ps1') -OutputRoot $OutputRoot -VisualStudioPath $vs -WindowsSdkRoot $WindowsSdkRoot
    if ($LASTEXITCODE -ne 0) { throw ('ScreenshotManager tests failed: ' + $LASTEXITCODE) }
}

Write-Host ('ScreenshotManager build target completed: ' + $Target) -ForegroundColor Green
