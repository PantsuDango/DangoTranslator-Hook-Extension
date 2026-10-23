param(
    [Parameter(Mandatory=$true)][string]$ManagedReferenceRoot,
    [string]$ReferenceRoot=(Join-Path $PSScriptRoot '../../../third_party/LunaTranslator'),
    [string]$BuildRoot
)
$ErrorActionPreference='Stop'
if (-not $BuildRoot) { $BuildRoot=Join-Path $env:TEMP ('dango-teamnimbus-build-'+[Guid]::NewGuid().ToString('N')) }
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$engineRoot=Join-Path $BuildRoot 'teamnimbus'
New-Item -ItemType Directory -Path $engineRoot -Force | Out-Null
$managed=Join-Path $engineRoot 'Dango.TeamNimbus.Reader.dll'
$compiler=Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$arguments=@('/nologo','/noconfig','/nostdlib','/target:library','/optimize+',('/out:'+$managed))
foreach($name in @('mscorlib.dll','System.dll','System.Core.dll','UnityEngine.dll','UnityEngine.CoreModule.dll','UnityEngine.TextRenderingModule.dll','UnityEngine.UIModule.dll','UnityEngine.UI.dll','Unity.TextMeshPro.dll')) {
    $path=Join-Path $ManagedReferenceRoot $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing managed reference $name" }
    $arguments+='/r:'+$path
}
$arguments+=(Join-Path $PSScriptRoot 'bridge/Reader.cs')
& $compiler @arguments
if($LASTEXITCODE -ne 0){ throw 'TeamNimbus managed build failed' }
$bytes=[IO.File]::ReadAllBytes($managed)
$header='#pragma once'+"`n"+'inline unsigned char teamNimbusReader[] = {'+(($bytes | ForEach-Object {$_.ToString()}) -join ',')+'};'
[IO.File]::WriteAllText((Join-Path $engineRoot 'teamnimbus_reader.generated.h'),$header,[Text.UTF8Encoding]::new($false))
& (Join-Path $PSScriptRoot '../hook_common/build_first_text.ps1') -BuildRoot $BuildRoot -ReferenceRoot $ReferenceRoot -Engines @('teamnimbus')
if($LASTEXITCODE -ne 0){ throw 'TeamNimbus native build failed' }
$release=Join-Path $BuildRoot 'release/teamnimbus'
Write-Output "TeamNimbus Hook components: $release"
