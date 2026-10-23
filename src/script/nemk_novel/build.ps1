param([string]$BuildRoot, [string]$ReferenceRoot)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $env:TEMP ('dango-nemk-hook-' + [Guid]::NewGuid().ToString('N')) }
$options = @{ Engines = @('nemk_novel'); BuildRoot = $BuildRoot }
if ($ReferenceRoot) { $options.ReferenceRoot = $ReferenceRoot }
& (Join-Path $PSScriptRoot '../hook_common/build_first_text.ps1') @options
if ($LASTEXITCODE -ne 0) { throw 'Hook component build failed' }
$release = Join-Path $BuildRoot 'release/nemk_novel'
$font = Join-Path $repo 'config/SourceHanSansSC-Regular.otf'
if ((Get-FileHash -LiteralPath $font -Algorithm SHA256).Hash.ToLowerInvariant() -ne '84bbd4ace91d327b3ad1a581c688196278a4e41308520176f419180064e4af2b') { throw 'Runtime font checksum mismatch' }
Copy-Item -LiteralPath $font -Destination $release -Force
Copy-Item -LiteralPath (Join-Path $repo 'config/SourceHanSansSC-LICENSE.txt') -Destination $release -Force
Write-Output "Nemk Novel Hook components: $release"
