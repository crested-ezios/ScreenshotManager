#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
#
# tools/check_contract_headers.ps1 -- verify SHA256 parity of vendored
# GakuLoader and GakuMotion contract headers against sibling checkouts.
#
#   GakuLoader (..\..\GakuLoader\dev\include\):
#     include\GakuPlugin.h
#     include\GakuPluginMetadata.h
#     include\GakuPaths.h
#     include\GakuPaths.hpp
#     include\GakuWebPanel.h
#
#   GakuMotion (..\..\GakuMotion\dev\include\):
#     include\GakuMotionStageState.h
#
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$LoaderIncludeRoot = '',
    [string]$MotionIncludeRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
        $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
    }
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)

if ([string]::IsNullOrWhiteSpace($LoaderIncludeRoot)) {
    $mainLoader = [IO.Path]::GetFullPath((Join-Path $RepoRoot '..\..\GakuLoader\main\include'))
    $devLoader = [IO.Path]::GetFullPath((Join-Path $RepoRoot '..\..\GakuLoader\dev\include'))
    $LoaderIncludeRoot = if (Test-Path -LiteralPath $mainLoader -PathType Container) { $mainLoader } else { $devLoader }
}
if ([string]::IsNullOrWhiteSpace($MotionIncludeRoot)) {
    $MotionIncludeRoot = [IO.Path]::GetFullPath((Join-Path $RepoRoot '..\..\GakuMotion\dev\include'))
}

Write-Host "ScreenshotManager : $RepoRoot"
Write-Host "GakuLoader        : $LoaderIncludeRoot"
Write-Host "GakuMotion        : $MotionIncludeRoot"

$groups = @(
    @{
        Label = 'GakuLoader'
        Root = $LoaderIncludeRoot
        Headers = @(
            'GakuPlugin.h',
            'GakuPluginMetadata.h',
            'GakuPaths.h',
            'GakuPaths.hpp',
            'GakuWebPanel.h'
        )
    },
    @{
        Label = 'GakuMotion'
        Root = $MotionIncludeRoot
        Headers = @(
            'GakuMotionStageState.h'
        )
    }
)

$failures = 0
foreach ($group in $groups) {
    if (-not (Test-Path -LiteralPath $group.Root)) {
        Write-Host ("  SKIP: {0} include directory not found; nothing to compare against." -f $group.Label) -ForegroundColor Yellow
        continue
    }

    foreach ($header in $group.Headers) {
        $mine = Join-Path (Join-Path $RepoRoot 'include') $header
        $theirs = Join-Path $group.Root $header
        if (-not (Test-Path -LiteralPath $mine -PathType Leaf)) {
            Write-Host ("  MISSING (ScreenshotManager): include\{0}" -f $header) -ForegroundColor Red
            $failures++
            continue
        }
        if (-not (Test-Path -LiteralPath $theirs -PathType Leaf)) {
            Write-Host ("  MISSING ({0}): {1}" -f $group.Label, $header) -ForegroundColor Red
            $failures++
            continue
        }
        $left = (Get-FileHash -LiteralPath $mine -Algorithm SHA256).Hash
        $right = (Get-FileHash -LiteralPath $theirs -Algorithm SHA256).Hash
        if ($left -ne $right) {
            Write-Host ("  DRIFT: {0}" -f $header) -ForegroundColor Red
            Write-Host ("    ScreenshotManager include\{0}  {1}" -f $header, $left)
            Write-Host ("    {0} {1}  {2}" -f $group.Label, $header, $right)
            $failures++
            continue
        }
        Write-Host ("  OK: {0} ({1})" -f $header, $left.Substring(0, 16)) -ForegroundColor Green
    }
}

if ($failures -ne 0) {
    Write-Host ("Contract header parity FAILED ({0})." -f $failures) -ForegroundColor Red
    exit 1
}

Write-Host 'Contract header parity passed.'
exit 0
