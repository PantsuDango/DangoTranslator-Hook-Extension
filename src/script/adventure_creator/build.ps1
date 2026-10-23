param([string]$BuildRoot, [string]$ReferenceRoot)
$ErrorActionPreference = 'Stop'
$options = @{ Engines = @('adventure_creator') }
if ($BuildRoot) { $options.BuildRoot = $BuildRoot }
if ($ReferenceRoot) { $options.ReferenceRoot = $ReferenceRoot }
& (Join-Path $PSScriptRoot '../hook_common/build_first_text.ps1') @options
if ($LASTEXITCODE -ne 0) { throw 'Hook component build failed' }
