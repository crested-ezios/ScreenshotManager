#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
[CmdletBinding()]
param(
    [string]$OutputRoot = '',
    [string]$VisualStudioPath = '',
    [string]$WindowsSdkRoot = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repo 'build'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$testDir = Join-Path $OutputRoot 'tests'
$objDir = Join-Path $testDir 'obj'
New-Item -ItemType Directory -Path $testDir, $objDir -Force | Out-Null

$vs = $VisualStudioPath
if ([string]::IsNullOrWhiteSpace($vs)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ([string]::IsNullOrWhiteSpace($vs)) { throw 'MSVC x64 toolset not found.' }
$msvc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1

$sdk = $WindowsSdkRoot
if ([string]::IsNullOrWhiteSpace($sdk)) {
    $sdk = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
}
if ([string]::IsNullOrWhiteSpace($sdk)) {
    $sdk = @(Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'; Join-Path $env:ProgramFiles 'Windows Kits\10') |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Include') } | Select-Object -First 1
}
$sdkVer = Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory |
    Where-Object {
        $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt\stddef.h') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $sdk "Lib\$($_.Name)\um\x64\advapi32.lib") -PathType Leaf)
    } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdkVer) { throw 'Windows SDK not found.' }

$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include'),
    (Join-Path $sdk "Include\$($sdkVer.Name)\ucrt"),
    (Join-Path $sdk "Include\$($sdkVer.Name)\um"),
    (Join-Path $sdk "Include\$($sdkVer.Name)\shared"),
    (Join-Path $sdk "Include\$($sdkVer.Name)\winrt")
) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64'),
    (Join-Path $sdk "Lib\$($sdkVer.Name)\ucrt\x64"),
    (Join-Path $sdk "Lib\$($sdkVer.Name)\um\x64")
) -join ';'
$env:PATH = (Join-Path $msvc.FullName 'bin\Hostx64\x64') + ';' + $env:PATH

$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$exe = Join-Path $testDir 'test_screenshot_manager.exe'
$obj = Join-Path $objDir 'test_screenshot_manager.obj'

& $cl /nologo /std:c++20 /utf-8 /O2 /EHsc /MT /W4 /D_CRT_SECURE_NO_WARNINGS `
    "-I$(Join-Path $repo 'include')" "-I$(Join-Path $repo 'src')" `
    (Join-Path $PSScriptRoot 'test_screenshot_manager.cpp') `
    "/Fo:$obj" '/link' "/OUT:$exe" windowscodecs.lib ole32.lib shell32.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw "test_screenshot_manager compile failed" }

& $exe
if ($LASTEXITCODE -ne 0) { throw "test_screenshot_manager failed" }

$contractCheck = Join-Path $repo 'tools\check_contract_headers.ps1'
if (Test-Path -LiteralPath $contractCheck -PathType Leaf) {
    $global:LASTEXITCODE = 0
    & $contractCheck -RepoRoot $repo
    if ($LASTEXITCODE -ne 0) { throw "check_contract_headers failed" }
}
