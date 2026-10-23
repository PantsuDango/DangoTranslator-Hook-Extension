param(
    [string]$ReferenceRoot,
    [string]$BuildRoot,
    [string[]]$Engines = @('yuris', 'azsystem', 'catsystem2', 'midareageha', 'bolero', 'wolf', 'taskforce2', 'bgi', 'unity_eventscript', 'utage', 'renpy', 'siglus', 'qlie', 'willplus', 'valkyria'),
    [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
if (-not $ReferenceRoot) { $ReferenceRoot = Join-Path $repositoryRoot 'third_party/LunaTranslator' }
$upstreamVersionSource = Get-Content -LiteralPath (Join-Path $ReferenceRoot 'src/NativeImpl/version.cmake') -Raw
$upstreamCopyright = [regex]::Match($upstreamVersionSource, '(?m)^\s*COMPANY_COPYRIGHT\s+"([^"]+)"').Groups[1].Value
if (-not $upstreamCopyright) { throw '上游缺少版权声明，拒绝构建无来源声明的组件' }
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $env:TEMP ('dango-first-text-' + [Guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$BuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path

$configurations = @{
    livemaker = @{ Prefix = 'LiveMaker'; Platforms = @('Win32'); Patches = @('LiveMakerBranding', 'Live.cpp', 'LiveMakerOnly') }
    kirikiri = @{ Prefix = 'KiriKiri'; Platforms = @('Win32'); Patches = @('KiriKiriBranding', 'KiriKiri.cpp', 'KiriKiriOnly') }
    gsx = @{ Prefix = 'GSX'; Platforms = @('Win32'); Patches = @('GSXBranding','GSXOnly','GSXEngine','GSXDisplayDetach') }
    constructor_ssf = @{ Prefix = 'ConstructorSSF'; Platforms = @('Win32'); Patches = @('ConstructorSSFBranding','ConstructorSSFOnly','ConstructorSSFEngine','ConstructorSSFDisplayDetach') }
    abel_ast = @{ Prefix = 'AbelAST'; Platforms = @('Win32'); Patches = @('AbelASTBranding','AbelASTOnly','AbelASTEngine','AbelASTDisplayDetach') }
    dxcompiled = @{ Prefix = 'DxCompiled'; Platforms = @('Win32'); Patches = @('DxCompiledBranding','DxCompiledOnly','DxCompiledEngine','DxCompiledDisplayDetach') }
	unity_mono = @{ Prefix = 'UnityMono'; Platforms = @('Win32','x64'); Patches = @('UnityMono','UnityMonoFlags') }
	teamnimbus = @{ Prefix = 'TeamNimbus'; Platforms = @('x64'); Patches = @('TeamNimbusMono') }
	nemk_novel = @{ Prefix = 'NemkNovel'; Platforms = @('x64'); Patches = @('NemkNovelIL2CPP') }
	yarn_spinner = @{ Prefix = 'YarnSpinner'; Platforms = @('x64'); Patches = @('YarnSpinnerMono') }
	unity_scenario = @{ Prefix = 'UnityScenario'; Platforms = @('x64'); Patches = @('UnityScenarioMono') }
	csystem = @{ Prefix = 'CSystem'; Platforms = @('Win32'); Patches = @('CSystemBranding', 'CSystemOnly', 'CSystemEngine', 'CSystemDisplayDetach') }
	nscripter = @{ Prefix = 'NScripter'; Platforms = @('Win32'); Patches = @('NScripterBranding', 'NScripterOnly', 'NScripterEngine', 'NScripterDisplayDetach') }
    adventure_creator = @{ Prefix = 'AdventureCreator'; Platforms = @('x64'); Patches = @('AdventureCreatorMono') }
    rpgmaker_unite = @{ Prefix = 'RPGMakerUnite'; Platforms = @('x64'); Patches = @('RPGMakerUniteMono') }
    siglus = @{ Prefix = 'Siglus'; Platforms = @('Win32'); Patches = @('SiglusBranding', 'SiglusOnly', 'SiglusDisplay') }
    yuris = @{ Prefix = 'Yuris'; Platforms = @('Win32'); Patches = @('YurisBranding', 'YurisOnly', 'YurisBackfill', 'YurisEncoding', 'YurisGdiFont') }
    azsystem = @{ Prefix = 'AZSystem'; Platforms = @('Win32'); Patches = @('AZSystemBranding', 'AZSystemOnly', 'AZSystemLegacy', 'AZSystemEncoding') }
    catsystem2 = @{ Prefix = 'CatSystem2'; Platforms = @('Win32'); Patches = @('CatSystem2Branding', 'CatSystem2Only', 'CatSystem2Backfill', 'CatSystem2Encoding') }
    midareageha = @{ Prefix = 'Midareageha'; Platforms = @('Win32'); Patches = @('MidareagehaBranding', 'MidareagehaOnly', 'MidareagehaBackfill', 'MidareagehaEncoding') }
    bolero = @{ Prefix = 'Bolero'; Platforms = @('Win32'); Patches = @('BoleroBranding', 'BoleroOnly', 'BoleroBackfill', 'BoleroEncoding') }
    wolf = @{ Prefix = 'Wolf'; Platforms = @('Win32'); Patches = @('WolfBranding', 'WolfOnly') }
    taskforce2 = @{ Prefix = 'Taskforce2'; Platforms = @('Win32'); Patches = @('Taskforce2Branding', 'Taskforce2Only') }
    bgi = @{ Prefix = 'BGI'; Platforms = @('x64'); Patches = @('BGIBranding', 'BGIOnly') }
    unity_eventscript = @{ Prefix = 'UnityEventScript'; Platforms = @('x64'); Patches = @('UnityEventScriptBranding', 'UnityEventScriptOnly', 'UnityEventScriptMono') }
    utage = @{ Prefix = 'Utage'; Platforms = @('Win32', 'x64'); Patches = @('UtageBranding', 'UtageOnly', 'UtageMono') }
    renpy = @{ Prefix = 'RenPy'; Platforms = @('x64'); Patches = @('RenPyBranding', 'RenPyOnly', 'RenPyResource') }
    qlie = @{ Prefix = 'QLIE'; Platforms = @('Win32'); Patches = @('QLIEBranding', 'QLIEOnly') }
    willplus = @{ Prefix = 'WillPlus'; Platforms = @('Win32'); Patches = @('WillPlusBranding', 'WillPlusOnly', 'WillPlusEmbed') }
    ddworks = @{ Prefix = 'DDWorks'; Platforms = @('Win32'); Patches = @('DDWorksBranding', 'DDWorksOnly', 'DDWorksEngine') }
    lcse = @{ Prefix = 'LCSE'; Platforms = @('Win32'); Patches = @('LCSEBranding', 'LCSEOnly', 'LCSEEngine') }
    pjadv = @{ Prefix = 'PJADV'; Platforms = @('Win32'); Patches = @('PJADVBranding', 'PJADVOnly', 'PJADVLayout', 'PJADVStaticGlyph', 'PJADVFontCache', 'PJADVFastSkip') }
    valkyria = @{ Prefix = 'Valkyria'; Platforms = @('Win32'); Patches = @('ValkyriaBranding', 'ValkyriaOnly', 'ValkyriaMessage', 'ValkyriaBackfill', 'ValkyriaStaticGlyph') }
}
$dependency = Join-Path $BuildRoot 'dependencies/minhook-master'
if (-not (Test-Path -LiteralPath (Join-Path $dependency 'include/MinHook.h'))) {
    New-Item -ItemType Directory -Path (Join-Path $BuildRoot 'dependencies') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'third_party/minhook') -Destination $dependency -Recurse
}

foreach ($engine in $Engines) {
    $configuration = $configurations[$engine]
    if (-not $configuration) { throw "未知构建引擎: $engine" }
    $engineRoot = Join-Path $BuildRoot $engine
    $nativeRoot = Join-Path $engineRoot 'src/NativeImpl'
    $sourceRoot = Join-Path $nativeRoot 'LunaHook'
    $engineScript = Join-Path $repositoryRoot ('src/script/' + $engine)
    if (-not (Test-Path -LiteralPath (Join-Path $engineRoot 'prepared.txt'))) {
        if (Test-Path -LiteralPath $sourceRoot) { throw "未完成的隔离源码需要人工检查: $sourceRoot" }
        New-Item -ItemType Directory -Path $nativeRoot -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $ReferenceRoot 'src/NativeImpl/LunaHook') -Destination $sourceRoot -Recurse
        foreach ($item in @('version.cmake', 'version_def.cpp', 'version', 'winxphelper.cmake', 'cmake')) {
            $itemPath = Join-Path $ReferenceRoot ('src/NativeImpl/' + $item)
            if (Test-Path -LiteralPath $itemPath) { Copy-Item -LiteralPath $itemPath -Destination $nativeRoot -Recurse }
        }
        Copy-Item -LiteralPath (Join-Path $ReferenceRoot 'src/version.txt') -Destination (Join-Path $engineRoot 'src/version.txt')
        $patches = @((Join-Path $PSScriptRoot 'patches/RuntimeBranding.patch'))
        if ($engine -eq 'adventure_creator' -or $engine -eq 'rpgmaker_unite' -or $engine -eq 'unity_scenario' -or $engine -eq 'nemk_novel' -or $engine -eq 'yarn_spinner' -or $engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') {
            $prefix = $configuration.Prefix
            $macro = if ($engine -eq 'adventure_creator') { 'DANGO_ADVENTURE_CREATOR_ONLY' } elseif ($engine -eq 'unity_scenario') { 'DANGO_UNITY_SCENARIO_ONLY' } elseif ($engine -eq 'nemk_novel') { 'DANGO_NEMK_NOVEL_ONLY' } elseif ($engine -eq 'yarn_spinner') { 'DANGO_YARN_SPINNER_ONLY' } elseif ($engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') { 'DANGO_TEAMNIMBUS_ONLY' } else { 'DANGO_RPGMAKER_UNITE_ONLY' }
            foreach ($name in @('UtageBranding', 'UtageOnly')) {
                $content = Get-Content -LiteralPath (Join-Path $repositoryRoot ('src/script/utage/patches/' + $name + '.patch')) -Raw
                $content = $content.Replace('DANGO_UTAGE_ONLY', $macro).Replace('Utage', $prefix).Replace('utage', $engine)
                $generated = Join-Path $engineRoot ($name + '.patch')
                [IO.File]::WriteAllText($generated, $content, [Text.UTF8Encoding]::new($false))
                $patches += $generated
            }
        }
        $patches += @($configuration.Patches | ForEach-Object { Join-Path $engineScript ('patches/' + $_ + '.patch') })
        $patches += Join-Path $PSScriptRoot 'patches/FirstTextHandshake.patch'
        if ($engine -eq 'lcse') { $patches += Join-Path $engineScript 'patches/LCSEControl.patch' }
        foreach ($patch in $patches) {
            $patchArguments = @('apply', '--unidiff-zero')
            if ($engine -eq 'unity_mono' -or $engine -eq 'siglus' -or $engine -eq 'valkyria' -or $engine -eq 'lcse' -or ($engine -in @('dxcompiled', 'abel_ast', 'constructor_ssf', 'gsx'))) { $patchArguments += '--ignore-space-change' }
            & git -C $engineRoot @patchArguments $patch
            if ($LASTEXITCODE -ne 0) { throw "应用补丁失败: $patch" }
        }
        foreach ($header in (Get-ChildItem -LiteralPath $engineScript -Filter '*.h' -File)) {
            Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $sourceRoot 'LunaHook/engine32')
        }
        if ($engine -eq 'ddworks' -or $engine -eq 'lcse' -or $engine -eq 'csystem' -or $engine -eq 'nscripter' -or ($engine -in @('dxcompiled', 'abel_ast', 'constructor_ssf', 'gsx'))) {
            Copy-Item -LiteralPath (Join-Path $engineScript ($configuration.Prefix + '.cpp')) -Destination (Join-Path $sourceRoot 'LunaHook/engine32')
        }
        if ($engine -eq 'adventure_creator' -or $engine -eq 'rpgmaker_unite' -or $engine -eq 'unity_scenario' -or $engine -eq 'nemk_novel' -or $engine -eq 'yarn_spinner' -or $engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') {
            $header = if ($engine -eq 'adventure_creator') { 'adventure_creator.hpp' } elseif ($engine -eq 'unity_scenario') { 'unity_scenario.hpp' } elseif ($engine -eq 'nemk_novel') { 'nemk_novel.hpp' } elseif ($engine -eq 'yarn_spinner') { 'yarn_spinner.hpp' } elseif ($engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') { 'teamnimbus.hpp' } else { 'rpgmaker_unite.hpp' }
            Copy-Item -LiteralPath (Join-Path $engineScript $header) -Destination (Join-Path $sourceRoot ('LunaHook/engines/mono/' + $header))
        }
        if ($engine -eq 'renpy') {
            Copy-Item -LiteralPath (Join-Path $engineScript 'resource/Renpy.h') -Destination (Join-Path $sourceRoot 'LunaHook/engine64/Renpy.h')
            foreach ($file in @('python.cpp', 'python3.cpp')) {
                Copy-Item -LiteralPath (Join-Path $engineScript ('resource/' + $file)) -Destination (Join-Path $sourceRoot ('LunaHook/engines/python/' + $file))
            }
            Copy-Item -LiteralPath (Join-Path $engineScript 'resource/renpy_hook_text.py') -Destination (Join-Path $sourceRoot 'LunaHook/resource/renpy_hook_text.py')
        }
        Set-Content -LiteralPath (Join-Path $engineRoot 'prepared.txt') -Value 'ready'
    }
    $componentVersionSource = Get-Content -LiteralPath (Join-Path $nativeRoot 'version.cmake') -Raw
    $componentCopyright = [regex]::Match($componentVersionSource, '(?m)^\s*COMPANY_COPYRIGHT\s+"([^"]+)"').Groups[1].Value
    if ($componentCopyright -cne $upstreamCopyright) {
        throw "组件未保留上游版权声明，请使用新构建目录并检查品牌补丁: $engine"
    }
    if ($PrepareOnly) {
        Write-Output "已准备源码及补丁: $engine"
        continue
    }
    $releaseRoot = Join-Path $BuildRoot ('release/' + $engine)
    New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
    if ($engine -eq 'yuris') {
        & python (Join-Path $engineScript 'generate_layout.py') (Join-Path $repositoryRoot 'src/utils/hook/yuris_display_layout.json') (Join-Path $sourceRoot 'LunaHook/engine32/YurisLayout.generated.h')
        if ($LASTEXITCODE -ne 0) { throw 'YU-RIS共享显示合同生成失败' }
        foreach ($header in (Get-ChildItem -LiteralPath $engineScript -Filter '*.h' -File)) {
            Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $sourceRoot 'LunaHook/engine32') -Force
        }
        $yurisSource = Get-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engine32/Yuris.cpp') -Raw
        if (-not $yurisSource.Contains('YurisBackfill::attachRead(readAddress, menuReadAddress)')) {
            throw 'YU-RIS隔离源码使用旧读取补丁，请使用新的构建目录'
        }
        $hostBuild = Join-Path $engineRoot 'host-build'
        & cmake -S $engineScript -B $hostBuild -G 'Visual Studio 17 2022' -A Win32
        if ($LASTEXITCODE -ne 0) { throw 'YU-RIS Host configure failed' }
        & cmake --build $hostBuild --config Release --clean-first -- /m:2 /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw 'YU-RIS Host build failed' }
        foreach ($name in @('YurisHost32.exe', 'YurisInjector32.exe')) {
            Copy-Item -LiteralPath (Join-Path $hostBuild ('bin/Release/' + $name)) -Destination $releaseRoot
        }
    }
    if ($engine -eq 'adventure_creator' -or $engine -eq 'rpgmaker_unite' -or $engine -eq 'unity_scenario' -or $engine -eq 'nemk_novel' -or $engine -eq 'yarn_spinner' -or $engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') {
        $header = if ($engine -eq 'adventure_creator') { 'adventure_creator.hpp' } elseif ($engine -eq 'unity_scenario') { 'unity_scenario.hpp' } elseif ($engine -eq 'nemk_novel') { 'nemk_novel.hpp' } elseif ($engine -eq 'yarn_spinner') { 'yarn_spinner.hpp' } elseif ($engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') { 'teamnimbus.hpp' } else { 'rpgmaker_unite.hpp' }
        Copy-Item -LiteralPath (Join-Path $engineScript $header) -Destination (Join-Path $sourceRoot ('LunaHook/engines/mono/' + $header)) -Force
    }
    if ($engine -eq 'nemk_novel') { Copy-Item -LiteralPath (Join-Path $engineScript 'NemkControl.h') -Destination (Join-Path $sourceRoot 'LunaHook/engines/mono/NemkControl.h') -Force }
    if ($engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') {
        Copy-Item -LiteralPath (Join-Path $engineRoot 'teamnimbus_reader.generated.h') -Destination (Join-Path $sourceRoot 'LunaHook/engines/mono/teamnimbus_reader.generated.h') -Force
    }
    if ($engine -eq 'yarn_spinner') {
        Copy-Item -LiteralPath (Join-Path $engineRoot 'reader_bytes.generated.h') -Destination (Join-Path $sourceRoot 'LunaHook/engines/mono/reader_bytes.generated.h') -Force
    }
    if ($engine -eq 'ddworks' -or $engine -eq 'lcse' -or $engine -eq 'csystem' -or $engine -eq 'nscripter' -or ($engine -in @('dxcompiled', 'abel_ast', 'constructor_ssf', 'gsx'))) {
        Copy-Item -LiteralPath (Join-Path $engineScript ($configuration.Prefix + '.cpp')) -Destination (Join-Path $sourceRoot ('LunaHook/engine32/' + $configuration.Prefix + '.cpp')) -Force
        foreach ($header in (Get-ChildItem -LiteralPath $engineScript -Filter '*.h' -File)) {
            Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $sourceRoot 'LunaHook/engine32') -Force
        }
    }
    if ($engine -eq 'nscripter') {
        $semanticRoot = Join-Path $sourceRoot 'LunaHook/engine32/native'
        New-Item -ItemType Directory -Path $semanticRoot -Force | Out-Null
        foreach ($semanticFile in (Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'src/game/format/nscripter/native') -File)) {
            Copy-Item -LiteralPath $semanticFile.FullName -Destination $semanticRoot -Force
        }
        $layout = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src/game/format/nscripter/layout.json') -Raw | ConvertFrom-Json
        if ($layout.version -ne 1) { throw 'NScripter布局定义版本不兼容' }
        $generated = @('#pragma once', '#include <cstddef>', 'struct DangoNScripterFunction { const char *name; const char *pattern; size_t hookOffset; };', 'struct DangoNScripterCall { const char *from; size_t offset; const char *to; };', 'struct DangoNScripterPointer { const char *from; size_t offset; const char *name; size_t size; };')
        $readLayouts = @($layout) + @($layout.variants)
        for ($layoutIndex = 0; $layoutIndex -lt $readLayouts.Count; $layoutIndex++) {
            $candidate = $readLayouts[$layoutIndex]
            if ($candidate.version -ne 1 -or $candidate.functions.Count -ne 6) { throw 'NScripter读取布局变体定义无效' }
            $generated += 'constexpr DangoNScripterFunction dangoNScripterFunctions' + $layoutIndex + '[] = {'
            foreach ($spec in $candidate.functions) { $generated += '  {"' + $spec.name + '", "' + $spec.pattern + '", ' + [int]$spec.hookOffset + '},' }
            $generated += '};'
            $generated += 'constexpr DangoNScripterCall dangoNScripterCalls' + $layoutIndex + '[] = {'
            foreach ($spec in $candidate.calls) { $generated += '  {"' + $spec.from + '", ' + [int]$spec.offset + ', "' + $spec.to + '"},' }
            $generated += '};'
            $generated += 'constexpr DangoNScripterPointer dangoNScripterPointers' + $layoutIndex + '[] = {'
            foreach ($spec in $candidate.pointers) { $generated += '  {"' + $spec.from + '", ' + [int]$spec.offset + ', "' + $spec.name + '", ' + [int]$spec.size + '},' }
            $generated += '};'
        }
        $generated += 'struct DangoNScripterReadLayout { const DangoNScripterFunction *functions; size_t functionCount; const DangoNScripterCall *calls; size_t callCount; const DangoNScripterPointer *pointers; size_t pointerCount; };'
        $generated += 'constexpr DangoNScripterReadLayout dangoNScripterReadLayouts[] = {'
        for ($layoutIndex = 0; $layoutIndex -lt $readLayouts.Count; $layoutIndex++) {
            $candidate = $readLayouts[$layoutIndex]
            $generated += ' {dangoNScripterFunctions' + $layoutIndex + ',' + $candidate.functions.Count + ',dangoNScripterCalls' + $layoutIndex + ',' + $candidate.calls.Count + ',dangoNScripterPointers' + $layoutIndex + ',' + $candidate.pointers.Count + '},'
        }
        $generated += '};'
        Set-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engine32/NScripterLayout.generated.h') -Value $generated -Encoding UTF8
        $backfill = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src/game/format/nscripter/backfill_layout.json') -Raw | ConvertFrom-Json
        if ($backfill.version -ne 1) { throw 'NScripter回填布局版本不兼容' }
        $generated = @('#pragma once', '#include <cstddef>', 'struct DangoNScripterBackfillSpan { size_t offset; const char *pattern; };')
        foreach ($spec in $backfill.functions) {
            $generated += 'constexpr DangoNScripterBackfillSpan dangoNScripterBackfill_' + $spec.name + '[] = {'
            foreach ($span in $spec.spans) { $generated += ' {' + [int]$span.offset + ', "' + $span.pattern + '"},' }
            $generated += '};'
        }
        $generated += 'struct DangoNScripterBackfillFunction { const char *name; const DangoNScripterBackfillSpan *spans; size_t spanCount,begin,step,glyph,shadow,end,sourceOffset,indexOffset,verticalPointerOffset,textOutPointerOffset; };'
        $generated += 'constexpr DangoNScripterBackfillFunction dangoNScripterBackfillFunctions[] = {'
        foreach ($spec in $backfill.functions) {
            $values = @($spec.spans.Count)
            foreach ($field in @('begin','step','glyph','shadow','end','sourceOffset','indexOffset','verticalPointerOffset','textOutPointerOffset')) { $values += [int]$spec.$field }
            $generated += ' {"' + $spec.name + '",dangoNScripterBackfill_' + $spec.name + ',' + ($values -join ',') + '},'
        }
        $generated += '};'
        Set-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engine32/NScripterBackfill.generated.h') -Value $generated -Encoding UTF8
    }
    if ($engine -eq 'csystem') {
        $layout = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src/game/format/csystem/layout.json') -Raw | ConvertFrom-Json
        if ($layout.version -ne 1) { throw 'CSystem读取布局定义不兼容' }
        $generated = @('#pragma once', '#include <cstddef>', 'struct DangoCSystemFunctionSpec { const char *name; const char *pattern; size_t callOffset; const char *target; };', 'constexpr DangoCSystemFunctionSpec dangoCSystemFunctions[] = {')
        foreach ($spec in $layout.functions) { $generated += '  {"' + $spec.name + '", "' + $spec.pattern + '", ' + [int]$spec.callOffset + ', "' + $spec.target + '"},' }
        $generated += '};'
        Set-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engine32/CSystemLayout.generated.h') -Value $generated -Encoding UTF8
		$display = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src/game/format/csystem/display_layout.json') -Raw | ConvertFrom-Json
		if ($display.version -ne 2) { throw 'CSystem显示布局定义不兼容' }
        # Keep string tokens below MSVC's literal limit and parenthesize array expressions.
        function CSystemPatternLiteral([string]$pattern) {
            $chunks = @()
            for ($offset = 0; $offset -lt $pattern.Length; $offset += 3000) {
                $chunks += '"' + $pattern.Substring($offset, [Math]::Min(3000, $pattern.Length - $offset)) + '"'
            }
            return ($chunks -join "`n")
        }
        $generatedDisplay = @('#pragma once', '#include <cstddef>', 'struct DangoCSystemDisplayCall { size_t offset; unsigned group; bool followJump; const char *pattern; };')
        $generatedDisplay += 'constexpr const char *dangoCSystemRenderPattern = ' + (CSystemPatternLiteral $display.renderPattern) + ';'
        $generatedDisplay += 'constexpr const char *dangoCSystemReturnPattern = ' + (CSystemPatternLiteral $display.returnPattern) + ';'
        $generatedDisplay += 'constexpr DangoCSystemDisplayCall dangoCSystemDisplayCalls[] = {'
        foreach ($spec in $display.calls) {
            $follow = if ($spec.followJump) { 'true' } else { 'false' }
            $generatedDisplay += '  {' + [int]$spec.offset + ', ' + [int]$spec.group + ', ' + $follow + ', ' + (CSystemPatternLiteral $spec.pattern) + '},'
        }
        $generatedDisplay += '};'
        $generatedDisplay += 'constexpr unsigned dangoCSystemFontGroup = ' + [int]$display.fontGroup + ';'
        $generatedDisplay += 'constexpr size_t dangoCSystemFontHookOffset = ' + [int]$display.fontHookOffset + ';'
        $generatedDisplay += 'constexpr size_t dangoCSystemFontDrawOffsets[] = {' + (($display.fontDrawOffsets | ForEach-Object { [int]$_ }) -join ',') + '};'
        Set-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engine32/CSystemDisplay.generated.h') -Value $generatedDisplay -Encoding UTF8
    }
    if ($engine -eq 'lcse') {
        $layout = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src/game/format/lcse/layout.json') -Raw | ConvertFrom-Json
        if ($layout.version -ne 1) { throw 'LCSE布局定义版本不兼容' }
        $generated = @('#pragma once', '#include <cstddef>', 'struct DangoLCSEFunctionSpec { const char *name; const char *pattern; size_t callOffset; const char *target; };', 'constexpr DangoLCSEFunctionSpec dangoLCSEFunctions[] = {')
        foreach ($spec in (@($layout.functions) + @($layout.glyphFunctions) + @($layout.displayFunctions) + @($layout.presentationFunctions) + @($layout.choiceFunctions) + @($layout.hoverFunctions))) {
            $generated += '  {"' + $spec.name + '", "' + $spec.pattern + '", ' + [int]$spec.callOffset + ', "' + $spec.target + '"},'
        }
        $generated += '};'
        $generated += 'constexpr const char *dangoLCSEDisplayNames[] = {'
        foreach ($spec in $layout.displayFunctions) {
            if ($spec.name -notin @($layout.displayDerived.name)) { $generated += '  "' + $spec.name + '",' }
        }
        $generated += '};'
        $generated += 'struct DangoLCSEDisplayLink { const char *from; size_t offset; const char *to; };'
        $generated += 'constexpr DangoLCSEDisplayLink dangoLCSEDisplayDerived[] = {'
        foreach ($link in $layout.displayDerived) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.name + '"},' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEDisplayLink dangoLCSEDisplayCalls[] = {'
        foreach ($link in $layout.displayCalls) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.to + '"},' }
        $generated += '};'
        $generated += 'struct DangoLCSEDisplayPointer { const char *from; size_t offset; const char *to; const char *pattern; };'
        $generated += 'constexpr DangoLCSEDisplayPointer dangoLCSEDisplayPointers[] = {'
        foreach ($link in $layout.displayPointers) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.to + '", "' + $link.pattern + '"},' }
        $generated += '};'
        $generated += 'struct DangoLCSEDisplayImport { const char *from; size_t offset; const char *module; const char *symbol; };'
        $generated += 'constexpr DangoLCSEDisplayImport dangoLCSEDisplayImports[] = {'
        foreach ($link in $layout.displayImports) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.dll + '", "' + $link.symbol + '"},' }
        $generated += '};'
        $generated += 'struct DangoLCSEDisplayData { const char *name; const char *from; size_t offset; const char *pattern; };'
        $generated += 'constexpr DangoLCSEDisplayData dangoLCSEDisplayData[] = {'
        foreach ($link in $layout.displayData) { $generated += '  {"' + $link.name + '", "' + $link.from + '", ' + [int]$link.offset + ', "' + $link.pattern + '"},' }
        $generated += '};'
        $generated += 'constexpr const char *dangoLCSEPresentationNames[] = {'
        foreach ($spec in $layout.presentationFunctions) { $generated += '  "' + $spec.name + '",' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEDisplayLink dangoLCSEPresentationCalls[] = {'
        foreach ($link in $layout.presentationCalls) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.to + '"},' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEDisplayImport dangoLCSEPresentationImports[] = {'
        foreach ($link in $layout.presentationImports) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.dll + '", "' + $link.symbol + '"},' }
        $generated += '};'
        $generated += 'struct DangoLCSEPresentationGlobal { const char *from; size_t offset; const char *name; size_t size; };'
        $generated += 'constexpr DangoLCSEPresentationGlobal dangoLCSEPresentationGlobals[] = {'
        foreach ($link in $layout.presentationGlobals) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.name + '", ' + [int]$link.size + '},' }
        $generated += '};'
        $generated += 'struct DangoLCSEPresentationPhase { const char *from; size_t offset; const char *name; const char *pattern; int dcFrameOffset; int objectFrameOffset; };'
        $generated += 'constexpr DangoLCSEPresentationPhase dangoLCSEPresentationPhases[] = {'
        foreach ($link in $layout.presentationPhases) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.name + '", "' + $link.pattern + '", ' + [int]$link.dcFrameOffset + ', ' + [int]$link.objectFrameOffset + '},' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEPresentationPhase dangoLCSERefreshPhases[] = {'
        foreach ($link in $layout.refreshPhases) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.name + '", "' + $link.pattern + '", ' + [int]$link.dcFrameOffset + ', ' + [int]$link.objectFrameOffset + '},' }
        $generated += '};'
        $generated += 'constexpr const char *dangoLCSEChoiceNames[] = {'
        foreach ($spec in $layout.choiceFunctions) { $generated += '  "' + $spec.name + '",' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEDisplayLink dangoLCSEChoiceCalls[] = {'
        foreach ($link in $layout.choiceCalls) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.to + '"},' }
        $generated += '};'
        $generated += 'constexpr const char *dangoLCSEHoverNames[] = {'
        foreach ($spec in $layout.hoverFunctions) { $generated += '  "' + $spec.name + '",' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEDisplayLink dangoLCSEHoverCalls[] = {'
        foreach ($link in $layout.hoverCalls) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.to + '"},' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEDisplayImport dangoLCSEHoverImports[] = {'
        foreach ($link in $layout.hoverImports) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.dll + '", "' + $link.symbol + '"},' }
        $generated += '};'
        $generated += 'constexpr DangoLCSEPresentationGlobal dangoLCSEHoverGlobals[] = {'
        foreach ($link in $layout.hoverGlobals) { $generated += '  {"' + $link.from + '", ' + [int]$link.offset + ', "' + $link.name + '", ' + [int]$link.size + '},' }
        $generated += '};'
        Set-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engine32/LCSELayout.generated.h') -Value $generated -Encoding UTF8
    }
    if ($engine -eq 'unity_scenario') {
        $layout = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src/game/format/monoscenario/display_layout.json') -Raw | ConvertFrom-Json
        $generated = @('#pragma once', 'struct ScenarioTokenSpec { size_t offset; const char *semantic; };', 'struct ScenarioDisplaySpec { const char *name; const char *code; const ScenarioTokenSpec *tokens; size_t count; };')
        for ($index = 0; $index -lt $layout.Count; $index++) {
            $spec = $layout[$index]
            if ($spec.class -ne 'TDScenarioWindow' -or $spec.locals -ne '' -or $spec.exceptions -ne '') { throw 'Unsupported Scenario display contract' }
            $generated += "constexpr ScenarioTokenSpec scenarioTokens$index[] = {"
            foreach ($token in $spec.tokens.PSObject.Properties) { $generated += '  {' + [int]$token.Name + ', "' + $token.Value + '"},' }
            $generated += '};'
        }
        $generated += 'constexpr ScenarioDisplaySpec scenarioDisplaySpecs[] = {'
        for ($index = 0; $index -lt $layout.Count; $index++) { $spec = $layout[$index]; $generated += '  {"' + $spec.name + '", "' + $spec.code + '", scenarioTokens' + $index + ', sizeof(scenarioTokens' + $index + ')/sizeof(ScenarioTokenSpec)},' }
        $generated += '};'
        Set-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engines/mono/scenario_display.generated.h') -Value $generated -Encoding UTF8
    }
    # 独立源码仓库始终编译Host/Injector，不依赖翻译器现有二进制。
    if ($engine -eq 'adventure_creator' -or $engine -eq 'rpgmaker_unite' -or $engine -eq 'unity_scenario' -or $engine -eq 'nemk_novel' -or $engine -eq 'yarn_spinner' -or $engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') {
        $prefix = $configuration.Prefix
        $hostBuild = Join-Path $engineRoot 'host-build'
        & cmake -S $engineScript -B $hostBuild -G 'Visual Studio 17 2022' -A x64
        if ($LASTEXITCODE -ne 0) { throw "$prefix Host configure failed" }
        & cmake --build $hostBuild --config Release --clean-first -- /m:2 /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw "$prefix Host build failed" }
        foreach ($name in @("${prefix}Host64.exe", "${prefix}Injector64.exe")) {
            Copy-Item -LiteralPath (Join-Path $hostBuild ('bin/Release/' + $name)) -Destination $releaseRoot
        }
    } elseif ($engine -eq 'ddworks' -or $engine -eq 'lcse' -or $engine -eq 'csystem' -or $engine -eq 'nscripter' -or ($engine -in @('dxcompiled', 'abel_ast', 'constructor_ssf', 'gsx'))) {
        $prefix = $configuration.Prefix
        $hostBuild = Join-Path $engineRoot 'host-build'
        & cmake -S $engineScript -B $hostBuild -G 'Visual Studio 17 2022' -A Win32
        if ($LASTEXITCODE -ne 0) { throw "$prefix Host configure failed" }
        & cmake --build $hostBuild --config Release --clean-first -- /m:2 /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw "$prefix Host build failed" }
        foreach ($name in @("${prefix}Host32.exe", "${prefix}Injector32.exe")) {
            Copy-Item -LiteralPath (Join-Path $hostBuild ('bin/Release/' + $name)) -Destination $releaseRoot
        }
    } elseif ($engine -eq 'pjadv') {
        $layout = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src/utils/hook/pjadv_layout.json') -Raw | ConvertFrom-Json
        if ($layout.version -ne 1) { throw 'PJADV 布局定义版本不兼容' }
        $generated = @('#pragma once', '#include <cstddef>', 'struct DangoPJADVFunctionSpec { const char *name; const char *pattern; size_t checkOffset; const char *check; };', 'constexpr DangoPJADVFunctionSpec dangoPJADVFunctions[] = {')
        foreach ($spec in $layout.functions) {
            $generated += '  {"' + $spec.name + '", "' + $spec.pattern + '", ' + $spec.checkOffset + ', "' + $spec.check + '"},'
        }
        $generated += '};'
        Set-Content -LiteralPath (Join-Path $sourceRoot 'LunaHook/engine32/PJADVLayout.generated.h') -Value $generated -Encoding UTF8
        foreach ($header in (Get-ChildItem -LiteralPath $engineScript -Filter '*.h' -File)) {
            Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $sourceRoot 'LunaHook/engine32') -Force
        }
        $hostBuild = Join-Path $engineRoot 'host-build'
        & cmake -S $engineScript -B $hostBuild -G 'Visual Studio 17 2022' -A Win32
        if ($LASTEXITCODE -ne 0) { throw 'PJADV Host configure failed' }
        & cmake --build $hostBuild --config Release --clean-first -- /m:2 /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw 'PJADV Host build failed' }
        foreach ($name in @('PJADVHost32.exe', 'PJADVInjector32.exe')) {
            Copy-Item -LiteralPath (Join-Path $hostBuild ('bin/Release/' + $name)) -Destination $releaseRoot
        }
    } elseif ($engine -ne 'yuris') {
        foreach ($hostPlatform in $configuration.Platforms) {
            $hostBits = if ($hostPlatform -eq 'Win32') { '32' } else { '64' }
            $hostBuild = Join-Path $engineRoot ('host-' + $hostPlatform)
            & cmake -S $engineScript -B $hostBuild -G 'Visual Studio 17 2022' -A $hostPlatform
            if ($LASTEXITCODE -ne 0) { throw "$engine Host configure failed" }
            & cmake --build $hostBuild --config Release -- /m:2 /verbosity:minimal
            if ($LASTEXITCODE -ne 0) { throw "$engine Host build failed" }
            foreach ($kind in @('Host', 'Injector')) {
                $name = $configuration.Prefix + $kind + $hostBits + '.exe'
                Copy-Item -LiteralPath (Join-Path $hostBuild ('bin/Release/' + $name)) -Destination $releaseRoot
            }
            if ($engine -eq 'kirikiri') {
                Copy-Item -LiteralPath (Join-Path $hostBuild 'Release/KiriKiriRuntime32.dll') -Destination $releaseRoot
            }
        }
    }
    $engineDependency = $dependency
    if ($engine -eq 'unity_scenario') {
        # 独立副本只扩展本引擎的合法rel32搜索范围，不改变其他组件的MinHook
        $engineDependency = Join-Path $engineRoot 'minhook-near'
        if (-not (Test-Path -LiteralPath $engineDependency)) {
            Copy-Item -LiteralPath $dependency -Destination $engineDependency -Recurse
        }
        $bufferPath = Join-Path $engineDependency 'src/buffer.c'
        $bufferSource = Get-Content -LiteralPath $bufferPath -Raw
        if ($bufferSource.Contains('#define MAX_MEMORY_RANGE 0x40000000')) {
            $bufferSource = $bufferSource.Replace('#define MAX_MEMORY_RANGE 0x40000000', '#define MAX_MEMORY_RANGE 0x7FFF0000')
            Set-Content -LiteralPath $bufferPath -Value $bufferSource -Encoding UTF8
        } elseif (-not $bufferSource.Contains('#define MAX_MEMORY_RANGE 0x7FFF0000')) {
            throw 'Unity Scenario MinHook分配布局不兼容，拒绝使用未知搜索范围'
        }
        # 扩大搜索时仍须核对原指令的RIP相对数据地址，禁止截断成错误的32位偏移
        $trampolinePath = Join-Path $engineDependency 'src/trampoline.c'
        $trampolineSource = (Get-Content -LiteralPath $trampolinePath -Raw).Replace("`r`n", "`n")
        $oldRelative = @'
            *pRelAddr
                = (UINT32)((pOldInst + hs.len + (INT32)hs.disp.disp32) - (pNewInst + hs.len));
'@
        $safeRelative = @'
            INT64 relative = (INT64)(pOldInst + hs.len + (INT32)hs.disp.disp32) - (INT64)(pNewInst + hs.len);
            if (relative < -2147483648LL || relative > 2147483647LL)
                return FALSE;
            *pRelAddr = (UINT32)(INT32)relative;
'@
        $oldRelative = $oldRelative.Replace("`r`n", "`n")
        $safeRelative = $safeRelative.Replace("`r`n", "`n")
        if ($trampolineSource.Contains($oldRelative)) {
            $trampolineSource = $trampolineSource.Replace($oldRelative, $safeRelative)
            Set-Content -LiteralPath $trampolinePath -Value $trampolineSource -Encoding UTF8
        } elseif (-not $trampolineSource.Contains($safeRelative)) {
            throw 'Unity Scenario MinHook重定位布局不兼容，拒绝截断未知相对地址'
        }
        # 复用同一MinHook的x64解码器，生成本引擎调用点校验所需声明
        $monoHeaders = Join-Path $sourceRoot 'LunaHook/engines/mono'
        Copy-Item -LiteralPath (Join-Path $engineScript 'unity_scenario.hpp') -Destination (Join-Path $monoHeaders 'unity_scenario.hpp') -Force
        Copy-Item -LiteralPath (Join-Path $engineDependency 'src/hde/hde64.h') -Destination (Join-Path $monoHeaders 'scenario_hde64.generated.h') -Force
        Copy-Item -LiteralPath (Join-Path $engineDependency 'src/hde/pstdint.h') -Destination (Join-Path $monoHeaders 'pstdint.h') -Force
    }
    foreach ($platform in $configuration.Platforms) {
        $buildDirectory = Join-Path $engineRoot ('build-' + $platform)
        $log = Join-Path $engineRoot ('build-' + $platform + '.log')
        & cmake -S $sourceRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A $platform -DBUILD_GUI=OFF -DWIN10ABOVE=ON "-DFETCHCONTENT_SOURCE_DIR_MINHOOK=$engineDependency" *> $log
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 25; throw "配置失败: $engine $platform" }
        $buildArguments = @('--build', $buildDirectory, '--config', 'Release', '--target', 'LunaHook', 'LunaHostDll')
        if ($engine -eq 'yuris' -or $engine -eq 'pjadv' -or $engine -eq 'ddworks' -or $engine -eq 'lcse' -or $engine -eq 'csystem' -or $engine -eq 'nscripter' -or ($engine -in @('dxcompiled', 'abel_ast', 'constructor_ssf', 'gsx')) -or $engine -eq 'adventure_creator' -or $engine -eq 'rpgmaker_unite' -or $engine -eq 'unity_scenario' -or $engine -eq 'nemk_novel' -or $engine -eq 'yarn_spinner' -or $engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') { $buildArguments += '--clean-first' }
        $buildStartedAt = Get-Date
        & cmake @buildArguments -- /m:2 /verbosity:minimal *>> $log
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 35; throw "构建失败: $engine $platform" }
        if ($engine -eq 'pjadv') {
            $pensilObject = Get-Item -LiteralPath (Join-Path $buildDirectory 'LunaHook/LunaHook.dir/Release/Pensil.obj')
            if ($pensilObject.LastWriteTime -lt $buildStartedAt) { throw 'PJADV 引擎目标文件未重新编译，拒绝发布旧组件' }
        }
        if ($engine -eq 'csystem') {
            $csystemObject = Get-Item -LiteralPath (Join-Path $buildDirectory 'LunaHook/LunaHook.dir/Release/CSystem.obj')
            if ($csystemObject.LastWriteTime -lt $buildStartedAt) { throw 'CSystem引擎目标文件未重新编译，拒绝发布旧组件' }
        }
        if ($engine -eq 'nscripter') {
            $nscripterObject = Get-Item -LiteralPath (Join-Path $buildDirectory 'LunaHook/LunaHook.dir/Release/NScripter.obj')
            if ($nscripterObject.LastWriteTime -lt $buildStartedAt) { throw 'NScripter引擎目标文件未重新编译，拒绝发布旧组件' }
        }
        if ($engine -eq 'dxcompiled') {
            $dxObject = Get-Item -LiteralPath (Join-Path $buildDirectory 'LunaHook/LunaHook.dir/Release/DxCompiled.obj')
            if ($dxObject.LastWriteTime -lt $buildStartedAt) { throw 'DxCompiled引擎目标文件未重新编译，拒绝发布旧组件' }
        }
        if ($engine -in @('abel_ast', 'constructor_ssf', 'gsx')) {
            $nativeEngineObject = Get-Item -LiteralPath (Join-Path $buildDirectory ('LunaHook/LunaHook.dir/Release/' + $configuration.Prefix + '.obj'))
            if ($nativeEngineObject.LastWriteTime -lt $buildStartedAt) { throw "$($configuration.Prefix)引擎目标文件未重新编译，拒绝发布旧组件" }
        }
        if ($engine -eq 'yuris') {
            $yurisObject = Get-Item -LiteralPath (Join-Path $buildDirectory 'LunaHook/LunaHook.dir/Release/Yuris.obj')
            if ($yurisObject.LastWriteTime -lt $buildStartedAt) { throw 'YU-RIS目标文件未重新编译，拒绝发布旧组件' }
        }
        if ($engine -eq 'adventure_creator' -or $engine -eq 'rpgmaker_unite' -or $engine -eq 'unity_scenario' -or $engine -eq 'nemk_novel' -or $engine -eq 'yarn_spinner' -or $engine -eq 'teamnimbus' -or $engine -eq 'unity_mono') {
            $monoObject = Get-Item -LiteralPath (Join-Path $buildDirectory 'LunaHook/LunaHook.dir/Release/mono.obj')
            if ($monoObject.LastWriteTime -lt $buildStartedAt) { throw "$($configuration.Prefix) 引擎目标文件未重新编译，拒绝发布旧组件" }
        }
        $bits = if ($platform -eq 'Win32') { '32' } else { '64' }
        foreach ($kind in @('Bridge', 'Hook')) {
            $fileName = $configuration.Prefix + $kind + $bits + '.dll'
            $builtFile = Join-Path $sourceRoot ('builds/Release_win10/' + $fileName)
            if ([Diagnostics.FileVersionInfo]::GetVersionInfo($builtFile).LegalCopyright -cne $upstreamCopyright) {
                throw "构建产物未保留上游版权声明，拒绝发布: $fileName"
            }
            Copy-Item -LiteralPath $builtFile -Destination $releaseRoot -Force
        }
        Write-Output "已构建: $engine $platform"
    }
}
if ($PrepareOnly) {
    Write-Output "源码准备完成: $BuildRoot"
} else {
    Write-Output "原生组件输出: $BuildRoot/release"
}
