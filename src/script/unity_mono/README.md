# Unity Mono Hook

该版本同步 DangoTranslator 7.0.4 的 Unity Mono Hook 运行时。`Reader.cs` 与
`LiveReader.cs` 编译为托管读取器并嵌入 Hook，前者负责全量显示协议，后者负责
经过租约和页面校验的实时显示协议；`TimelineReader.cs` 按 IL 语义识别 Timeline
选项的显示契约。

该目录提供 Unity Mono 的 Hook 适配源码和构建配置，目标架构为 x86 / x64。

## 构建

在仓库根目录运行：

```powershell
./build.ps1 -Engines unity_mono -ManagedReferenceRoot ./references/managed
```

工具链、输出目录和托管引用要求见 [构建指南](../../../BUILDING.md)。公共脚本按配置复制上游源码、应用补丁并检查上游版权声明。

## 源码与输出

本目录直接参与构建的文件包括 `CMakeLists.txt`、`Reader.cs`、`LiveReader.cs`、`TimelineReader.cs`、`teamnimbus.hpp`。引擎补丁位于 `patches/`，公共 Host/Injector 实现和握手补丁由其他引擎目录及 `hook_common/` 共享。

原生输出：

- `UnityMonoHost32.exe`、`UnityMonoInjector32.exe`、`UnityMonoBridge32.dll`、`UnityMonoHook32.dll`。
- `UnityMonoHost64.exe`、`UnityMonoInjector64.exe`、`UnityMonoBridge64.dll`、`UnityMonoHook64.dll`。

`Reader.cs`、`LiveReader.cs` 和 `TimelineReader.cs` 使用托管引用编译为同一个读取器并嵌入 Hook；旧版 Mono 的读取器引用可通过 `-ReaderManagedReferenceRoot` 单独指定。读取器只能引用单一的旧版完整 `UnityEngine.dll`；没有该程序集时，可用 `csc /nostdlib /target:library` 把 `UnityEngineReference.cs` 编为 `UnityEngine.dll`，与 .NET 3.5 的 `mscorlib.dll`、`System.dll`、`System.Core.dll` 放入同一目录作为 `-ReaderManagedReferenceRoot`。该引用桩只用于编译，不随组件发布。构建同时生成 x86/x64 原生组件。共享运行时协议由 `teamnimbus.hpp` 实现。

## 集成范围

运行时按本目录实现的入口、编码及布局检查决定能力，不承诺覆盖该引擎的全部版本或自定义扩展。显示快照、字形及控制数据按组件协议由集成应用提供；独立全文提取与资源生成工具不属于本项目。

上游来源及第三方许可见 [NOTICE.md](../../../NOTICE.md)。
