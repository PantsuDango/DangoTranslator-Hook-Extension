# Yarn Spinner Hook

该目录提供 Yarn Spinner 的 Hook 适配源码和构建配置，目标架构为 x64。

## 构建

在仓库根目录运行：

```powershell
./build.ps1 -Engines yarn_spinner -ManagedReferenceRoot ./references/managed
```

工具链、输出目录和托管引用要求见 [构建指南](../../../BUILDING.md)。公共脚本按配置复制上游源码、应用补丁并检查上游版权声明。

## 源码与输出

本目录直接参与构建的文件包括 `CMakeLists.txt`、`yarn_spinner.hpp`、`YarnControl.h`。引擎补丁位于 `patches/`，公共 Host/Injector 实现和握手补丁由其他引擎目录及 `hook_common/` 共享。

原生输出：

- `YarnSpinnerHost64.exe`、`YarnSpinnerInjector64.exe`、`YarnSpinnerBridge64.dll`、`YarnSpinnerHook64.dll`。

`bridge/` 包含读取、正文、界面和选项桥。专用构建脚本从 `src/game/format/yarnspinner/` 生成契约并将托管桥嵌入原生 Hook。

## 集成范围

运行时按本目录实现的入口、编码及布局检查决定能力，不承诺覆盖该引擎的全部版本或自定义扩展。显示快照、字形及控制数据按组件协议由集成应用提供；独立全文提取与资源生成工具不属于本项目。

上游来源及第三方许可见 [NOTICE.md](../../../NOTICE.md)。
