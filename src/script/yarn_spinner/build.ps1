param(
    [string]$BuildRoot,
    [string]$ReferenceRoot = (Join-Path $PSScriptRoot '../../../third_party/LunaTranslator'),
    [Parameter(Mandatory = $true)][string]$ManagedReferenceRoot
)
$ErrorActionPreference = 'Stop'
if (-not $BuildRoot) { $BuildRoot = Join-Path $env:TEMP ('dango-yarn-build-' + [Guid]::NewGuid().ToString('N')) }
$engineRoot = Join-Path $BuildRoot 'yarn_spinner'
New-Item -ItemType Directory -Path $engineRoot -Force | Out-Null
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$layout = Get-Content -LiteralPath (Join-Path $repo 'src/game/format/yarnspinner/runtime_layout.json') -Raw | ConvertFrom-Json
$records = foreach ($c in $layout) {
    $keys = @($c.tokens.PSObject.Properties.Name | Sort-Object { [int]$_ })
    $values = @($keys | ForEach-Object { ConvertTo-Json -InputObject $c.tokens.$_ -Compress })
    $args = @($c.class, $c.name, $c.signature, $c.code, $c.locals) | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress }
    'new Contract(' + ($args -join ',') + ',new int[]{' + ($keys -join ',') + '},new string[]{' + ($values -join ',') + '}),'
}
$generated = 'namespace Dango.YarnSpinner { public static partial class Reader { static readonly Contract[] Contracts = new Contract[]{' + ($records -join "`n") + '}; } }'
$contractsPath = Join-Path $engineRoot 'Contracts.cs'
[IO.File]::WriteAllText($contractsPath, $generated, [Text.UTF8Encoding]::new($false))
$display = Get-Content -LiteralPath (Join-Path $repo 'src/game/format/yarnspinner/display_layout.json') -Raw | ConvertFrom-Json
$displayGenerated = 'namespace Dango.YarnSpinner { public sealed partial class Display {'
foreach ($group in @(@{ Name='ChatContracts'; Assembly='Assembly-CSharp.dll'; Phone=$false }, @{ Name='CustomPhoneContracts'; Assembly='Assembly-CSharp.dll'; Phone=$true }, @{ Name='PhoneContracts'; Assembly='YarnSpinner.Unity.dll'; Phone=$true })) {
    $displayGenerated += 'static readonly Reader.Contract[] ' + $group.Name + '=new Reader.Contract[]{'
    foreach ($c in $display.($group.Assembly)) {
        $isPhone = $c.class -eq 'CupidChatroom.Yarn.ChatroomLineView'
        if ($group.Assembly -eq 'Assembly-CSharp.dll' -and $isPhone -ne $group.Phone) { continue }
        if ($c.exceptions) { throw 'Unsupported Yarn display exception contract' }
        $keys = @($c.tokens.PSObject.Properties.Name | Sort-Object { [int]$_ })
        $values = @($keys | ForEach-Object { ConvertTo-Json -InputObject $c.tokens.$_ -Compress })
        $args = @($c.class, $c.name, $c.signature, $c.code, $c.locals) | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress }
        $displayGenerated += 'new Reader.Contract(' + ($args -join ',') + ',new int[]{' + ($keys -join ',') + '},new string[]{' + ($values -join ',') + '},' + ([bool]$c.static).ToString().ToLowerInvariant() + '),'
    }
    $displayGenerated += '};'
}
$displayGenerated += '} }'
$displayPath = Join-Path $engineRoot 'DisplayContracts.cs'
[IO.File]::WriteAllText($displayPath, $displayGenerated, [Text.UTF8Encoding]::new($false))
$interface = Get-Content -LiteralPath (Join-Path $repo 'src/game/format/yarnspinner/interface_layout.json') -Raw | ConvertFrom-Json
$interfaceGenerated = 'namespace Dango.YarnSpinner { public sealed partial class Display { static readonly Reader.Contract[] InterfaceContracts=new Reader.Contract[]{'
foreach ($c in $interface) {
    $keys = @($c.tokens.PSObject.Properties.Name | Sort-Object { [int]$_ })
    $values = @($keys | ForEach-Object { ConvertTo-Json -InputObject $c.tokens.$_ -Compress })
    $args = @($c.class, $c.name, $c.signature, $c.code, $c.locals) | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress }
    $interfaceGenerated += 'new Reader.Contract(' + ($args -join ',') + ',new int[]{' + ($keys -join ',') + '},new string[]{' + ($values -join ',') + '}),'
}
$interfaceGenerated += '}; } }'
$interfacePath = Join-Path $engineRoot 'InterfaceContracts.cs'
[IO.File]::WriteAllText($interfacePath, $interfaceGenerated, [Text.UTF8Encoding]::new($false))
$choices = Get-Content -LiteralPath (Join-Path $repo 'src/game/format/yarnspinner/choices_layout.json') -Raw | ConvertFrom-Json
$choicesGenerated = 'namespace Dango.YarnSpinner { public sealed partial class Display {'
foreach ($group in @(@{ Name='ChoiceViewContracts'; Assembly='YarnSpinner.Unity.dll' }, @{ Name='ChoiceChatContracts'; Assembly='Assembly-CSharp.dll' })) {
    $choicesGenerated += 'static readonly Reader.Contract[] ' + $group.Name + '=new Reader.Contract[]{'
    foreach ($c in $choices.($group.Assembly)) {
        $keys = @($c.tokens.PSObject.Properties.Name | Sort-Object { [int]$_ })
        $values = @($keys | ForEach-Object { ConvertTo-Json -InputObject $c.tokens.$_ -Compress })
        $args = @($c.class, $c.name, $c.signature, $c.code, $c.locals) | ForEach-Object { ConvertTo-Json -InputObject $_ -Compress }
        $handlers = ConvertTo-Json -InputObject ([string]$c.handlers) -Compress
        $choicesGenerated += 'new Reader.Contract(' + ($args -join ',') + ',new int[]{' + ($keys -join ',') + '},new string[]{' + ($values -join ',') + '},false,' + $handlers + '),'
    }
    $choicesGenerated += '};'
}
$choicesGenerated += '} }'
$choicesPath = Join-Path $engineRoot 'ChoicesContracts.cs'
[IO.File]::WriteAllText($choicesPath, $choicesGenerated, [Text.UTF8Encoding]::new($false))
$managed = Join-Path $engineRoot 'Dango.YarnSpinner.Reader.dll'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$arguments = @('/nologo','/noconfig','/nostdlib','/target:library','/optimize+',('/out:'+$managed))
foreach ($file in @('mscorlib.dll','netstandard.dll','System.dll','System.Core.dll','UnityEngine.CoreModule.dll','UnityEngine.TextRenderingModule.dll','UnityEngine.TextCoreFontEngineModule.dll','UnityEngine.UI.dll','Unity.TextMeshPro.dll')) {
    $path = Join-Path $ManagedReferenceRoot $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "缺少托管编译引用: $file" }
    $arguments += '/r:' + $path
}
$arguments += @((Join-Path $PSScriptRoot 'bridge/Reader.cs'),(Join-Path $PSScriptRoot 'bridge/Display.cs'),(Join-Path $PSScriptRoot 'bridge/Interface.cs'),(Join-Path $PSScriptRoot 'bridge/Choices.cs'),$contractsPath,$displayPath,$interfacePath,$choicesPath)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Yarn Spinner托管读取器编译失败' }
$bytes = [IO.File]::ReadAllBytes($managed)
$header = '#pragma once' + "`n" + 'inline const unsigned char readerBytes[] = {' + (($bytes | ForEach-Object { $_.ToString() }) -join ',') + '};'
[IO.File]::WriteAllText((Join-Path $engineRoot 'reader_bytes.generated.h'), $header, [Text.UTF8Encoding]::new($false))
& (Join-Path $PSScriptRoot '../hook_common/build_first_text.ps1') -BuildRoot $BuildRoot -ReferenceRoot $ReferenceRoot -Engines @('yarn_spinner')
if ($LASTEXITCODE -ne 0) { throw 'Yarn Spinner原生组件编译失败' }
$font = Join-Path $repo 'config/SourceHanSansSC-Regular.otf'
if ((Get-FileHash -LiteralPath $font -Algorithm SHA256).Hash.ToLowerInvariant() -ne '84bbd4ace91d327b3ad1a581c688196278a4e41308520176f419180064e4af2b') { throw 'Yarn Spinner中文字体需要更新' }
Copy-Item -LiteralPath $font -Destination (Join-Path $BuildRoot 'release/yarn_spinner/SourceHanSansSC-Regular.otf') -Force
Copy-Item -LiteralPath (Join-Path $repo 'src/script/unity_eventscript/SourceHanSansSC-LICENSE.txt') -Destination (Join-Path $BuildRoot 'release/yarn_spinner/SourceHanSansSC-LICENSE.txt') -Force
