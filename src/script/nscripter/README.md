# NScripter Hook

该目录提供 NScripter 的 Hook 适配源码和构建配置，目标架构为 x86。

## 构建

在仓库根目录运行：

```powershell
./build.ps1 -Engines nscripter
```

工具链、输出目录和公共构建参数见 [构建指南](../../../BUILDING.md)。公共脚本按配置复制上游源码、应用补丁并检查上游版权声明。

## 源码与输出

本目录直接参与构建的文件包括 `.gitattributes`、`CMakeLists.txt`、`NScripter.cpp`、`NScripter.h`、`NScripterControl.h`、`NScripterDisplay.h`、`NScripterSnapshot.h`。引擎补丁位于 `patches/`，公共 Host/Injector 实现和握手补丁由其他引擎目录及 `hook_common/` 共享。

原生输出：

- `NScripterHost32.exe`、`NScripterInjector32.exe`、`NScripterBridge32.dll`、`NScripterHook32.dll`。

构建读取 `src/game/format/nscripter/` 的布局与原生辅助代码，生成共享头并编译显示协议。辅助解码器的原始许可位于该目录的 `native/LICENSE.txt`。

## 集成范围

运行时按本目录实现的入口、编码及布局检查决定能力，不承诺覆盖该引擎的全部版本或自定义扩展。显示快照、字形及控制数据按组件协议由集成应用提供；独立全文提取与资源生成工具不属于本项目。

上游来源及第三方许可见 [NOTICE.md](../../../NOTICE.md)。
