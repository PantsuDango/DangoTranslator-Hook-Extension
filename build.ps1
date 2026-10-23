param(
    [string[]]$Engines = @('livemaker'),
    [string]$BuildRoot,
    [string]$ManagedReferenceRoot,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
if (-not $BuildRoot) { $BuildRoot = Join-Path $env:TEMP ('dango-hook-' + [Guid]::NewGuid().ToString('N')) }
$common = Join-Path $PSScriptRoot 'src/script/hook_common/build_first_text.ps1'
if ($PrepareOnly) {
    & $common -Engines $Engines -BuildRoot $BuildRoot -PrepareOnly
    if ($LASTEXITCODE -ne 0) { throw 'Source preparation failed' }
    return
}
foreach ($engine in $Engines) {
    if ($engine -in @('teamnimbus', 'unity_mono', 'yarn_spinner')) {
        if (-not $ManagedReferenceRoot) { throw "$engine requires -ManagedReferenceRoot; see BUILDING.md" }
        $entry = Join-Path $PSScriptRoot ('src/script/' + $engine + '/build.ps1')
        $options = @{ BuildRoot = $BuildRoot; ManagedReferenceRoot = $ManagedReferenceRoot }
        & $entry @options
    } elseif ($engine -eq 'nemk_novel') {
        & (Join-Path $PSScriptRoot 'src/script/nemk_novel/build.ps1') -BuildRoot $BuildRoot
    } else {
        & $common -Engines @($engine) -BuildRoot $BuildRoot
    }
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $engine" }
}
Write-Output "Build output: $BuildRoot/release"
