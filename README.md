# DangoTranslator Hook

基于 [LunaTranslator / LunaHook](https://github.com/HIllya51/LunaTranslator) 的游戏文本 Hook 组件。

本项目提供 33 组引擎适配的原生源码、补丁、Host/Injector 和托管桥，面向组件开发与集成。构建所需的 LunaHook、MinHook 源码及共享布局数据随仓库提供。

## 构建

支持 Windows，使用 Visual Studio 2022 C++ 工具链、Windows SDK、CMake 和 Git。部分组件还需要 Python 或 Unity 托管编译引用，详见 [构建指南](BUILDING.md)。

```powershell
# 检查全部引擎的源码准备及补丁应用
$engines = (Get-Content ./SOURCE_MANIFEST.json -Raw | ConvertFrom-Json).engines
./build.ps1 -Engines $engines -PrepareOnly

# 构建 LiveMaker Hook 组件
./build.ps1 -Engines livemaker
```

默认在临时目录构建并打印输出路径，可用 `-BuildRoot` 指定目录。运行文件生成于 `<BuildRoot>/release/<engine>`。

## 源码结构

```text
src/script/                  引擎 Hook、Host/Injector、托管桥及补丁
src/game/format/             Hook 构建使用的共享布局和原生辅助代码
src/utils/hook/              Hook 构建使用的共享布局
third_party/LunaTranslator/ 上游 Hook 源码及许可
third_party/minhook/        MinHook 源码及许可
config/                    Hook 显示使用的字体及许可
```

引擎目录说明各组件的源码与构建入口。适配范围由各实现的结构检查决定，不能将存在一个适配目录理解为兼容该引擎的全部游戏或版本。

此项目仅提供 Hook 组件及其显示字体，不包含桌面主程序、独立全文提取器或游戏资源构建器。显示注入所需的快照和字形数据由集成应用提供，详见构建指南。

## 许可证与来源

团子 Hook 适配代码采用 [GNU General Public License v3.0](LICENSE)。

上游 LunaTranslator / LunaHook 作者为 **HIllya51**，保留版权声明：

```text
Copyright (C) 2026 HIllya51
```

第三方代码保留原有版权及许可，详见 [NOTICE.md](NOTICE.md) 和 [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt)。源码文件摘要及修改信息见 [SOURCE_MANIFEST.json](SOURCE_MANIFEST.json)，版本变更见 [CHANGES.md](CHANGES.md)。
