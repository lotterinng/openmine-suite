# SPDX-License-Identifier: Apache-2.0
# Runs a workflow step's commands, keeps their output in $RUNNER_TEMP/<Name>.log and, when they fail,
# reports the end of that output (and of every log file it names, e.g. vcpkg build logs) as error
# annotations. Annotations are visible through the public checks API, so a failure can be diagnosed
# without access to the full job log.
#
#   ./.github/scripts/logged.ps1 -Name configure -Script { cmake --preset windows-msvc }
param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][scriptblock]$Script,
    [int]$Tail = 80,
    [string]$Grep = ''     # also report the first lines matching this pattern (compiler errors, failed tests)
)

function Write-Annotation([string]$Title, [string[]]$Lines) {
    $text = ($Lines | ForEach-Object { if ($_.Length -gt 400) { $_.Substring(0, 400) + ' …' } else { $_ } }) -join "`n"
    $text = $text.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
    $Title = $Title.Replace('%', '%25').Replace(',', '%2C').Replace(':', '%3A').Replace("`n", ' ')
    Write-Output "::error title=$Title::$text"
}

$log = Join-Path $env:RUNNER_TEMP "$Name.log"
$ErrorActionPreference = 'Continue'
$global:LASTEXITCODE = 0
$failed = $false
try {
    & $Script *>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log | Out-Host
    if (-not $?) { $failed = $true }
    if ($LASTEXITCODE -ne 0) { $failed = $true }
} catch {
    "$_" | Out-File -Append -FilePath $log
    "$_" | Out-Host
    $failed = $true
}
if (-not $failed) { exit 0 }

$lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)
Write-Annotation "$Name failed (exit $LASTEXITCODE): last $Tail lines" ($lines | Select-Object -Last $Tail)

if ($Grep) {
    $hits = @($lines | Select-String -Pattern $Grep | Select-Object -First 150 | ForEach-Object { $_.Line })
    if ($hits.Count -gt 0) { Write-Annotation "$Name failed: lines matching '$Grep'" $hits }
}

# Log files the output points to (vcpkg: "See logs for more information: <path>").
$named = $lines | Select-String -Pattern '([A-Za-z]:[\\/][^\s"''<>|]+\.(log|txt))' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique -First 6
foreach ($file in $named) {
    $content = @(Get-Content -LiteralPath $file)
    $errors = @($content | Select-String -Pattern 'error|fatal|failed' | Select-Object -First 40 | ForEach-Object { $_.Line })
    Write-Annotation "$(Split-Path -Leaf $file): error lines" $errors
    Write-Annotation "$(Split-Path -Leaf $file): last 60 lines" ($content | Select-Object -Last 60)
}
throw "$Name failed"
