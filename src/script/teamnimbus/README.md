# Unity TeamNimbus Dialogue Hook

该目录提供 Unity TeamNimbus Dialogue 的 Hook 适配源码和构建配置，目标架构为 x64。

## 构建

在仓库根目录运行：

```powershell
./build.ps1 -Engines teamnimbus -ManagedReferenceRoot ./references/managed
```

工具链、输出目录和托管引用要求见 [构建指南](../../../BUILDING.md)。公共脚本按配置复制上游源码、应用补丁并检查上游版权声明。

## 源码与输出

本目录直接参与构建的文件包括 `CMakeLists.txt`、`Lease.h`、`teamnimbus.hpp`。引擎补丁位于 `patches/`，公共 Host/Injector 实现和握手补丁由其他引擎目录及 `hook_common/` 共享。

原生输出：

- `TeamNimbusHost64.exe`、`TeamNimbusInjector64.exe`、`TeamNimbusBridge64.dll`、`TeamNimbusHook64.dll`。

`bridge/Reader.cs` 编译后嵌入 Hook，`teamnimbus.hpp` 与 `Lease.h` 处理运行时入口和进程租约。需要目标运行时对应的 Unity/TMP 编译引用。

## 集成范围

运行时按本目录实现的入口、编码及布局检查决定能力，不承诺覆盖该引擎的全部版本或自定义扩展。显示快照、字形及控制数据按组件协议由集成应用提供；独立全文提取与资源生成工具不属于本项目。

上游来源及第三方许可见 [NOTICE.md](../../../NOTICE.md)。
