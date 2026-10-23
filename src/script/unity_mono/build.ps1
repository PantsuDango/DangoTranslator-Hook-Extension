param(
    [Parameter(Mandatory=$true)][string]$ManagedReferenceRoot,
    [string]$ReferenceRoot=(Join-Path $PSScriptRoot '../../../third_party/LunaTranslator'),
    [string]$BuildRoot
)
$ErrorActionPreference='Stop'
if(-not $BuildRoot){$BuildRoot=Join-Path $env:TEMP ('dango-unitymono-'+[Guid]::NewGuid().ToString('N'))}
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$engine=Join-Path $BuildRoot 'unity_mono'
New-Item -ItemType Directory -Path $engine -Force | Out-Null
$managed=Join-Path $engine 'Dango.UnityMono.Reader.dll'
$argsList=@('/nologo','/noconfig','/nostdlib','/target:library','/optimize+',('/out:'+$managed))
foreach($name in @('mscorlib.dll','System.dll','System.Core.dll','UnityEngine.dll')){$argsList+='/r:'+(Join-Path $ManagedReferenceRoot $name)}
foreach($name in @('UnityEngine.CoreModule.dll','UnityEngine.UIModule.dll','UnityEngine.TextRenderingModule.dll')) { $ref=Join-Path $ManagedReferenceRoot $name;if(Test-Path -LiteralPath $ref){$argsList+='/r:'+$ref} }
$argsList+=(Join-Path $PSScriptRoot 'Reader.cs')
& (Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe') @argsList
if($LASTEXITCODE -ne 0){throw 'Shared Mono bridge compilation failed'}
$bytes=[IO.File]::ReadAllBytes($managed)
[IO.File]::WriteAllText((Join-Path $engine 'teamnimbus_reader.generated.h'),('#pragma once'+"`n"+'inline unsigned char teamNimbusReader[] = {'+(($bytes|ForEach-Object{$_.ToString()}) -join ',')+'};'),[Text.UTF8Encoding]::new($false))
& (Join-Path $PSScriptRoot '../hook_common/build_first_text.ps1') -BuildRoot $BuildRoot -ReferenceRoot $ReferenceRoot -Engines @('unity_mono')
if($LASTEXITCODE -ne 0){throw 'Shared Mono native build failed'}
$release=Join-Path $BuildRoot 'release/unity_mono'
$host32=Join-Path $engine 'host32'
& cmake -S $PSScriptRoot -B $host32 -G 'Visual Studio 17 2022' -A Win32
if($LASTEXITCODE -ne 0){throw 'Shared Mono x86 host configuration failed'}
& cmake --build $host32 --config Release -- /m:2 /verbosity:minimal
if($LASTEXITCODE -ne 0){throw 'Shared Mono x86 host build failed'}
foreach($name in @('UnityMonoHost32.exe','UnityMonoInjector32.exe')){Copy-Item -LiteralPath (Join-Path $host32 ('bin/Release/'+$name)) -Destination $release -Force}
Write-Output "Shared Unity Mono components: $release"
