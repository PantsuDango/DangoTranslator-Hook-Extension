# Windows Hook 组件构建

## 工具链

- Visual Studio 2022：Desktop development with C++，含 x86/x64 MSVC 和 Windows SDK。
- CMake、Git，加入 PATH。
- Python 3.10 或更新版本：YU-RIS 共享布局头生成器。
- TeamNimbus、Unity Mono 和 Yarn Spinner：系统 .NET Framework C# 编译器及目标框架对应的托管编译引用。

构建默认使用 `third_party/LunaTranslator` 与 `third_party/minhook`，在仓库外生成中间文件。修改补丁或布局后，应使用新的构建目录。

## 检查补丁

```powershell
$engines = (Get-Content ./SOURCE_MANIFEST.json -Raw | ConvertFrom-Json).engines
./build.ps1 -Engines $engines -PrepareOnly
```

该操作复制上游源码、应用补丁并检查版权声明，不编译、运行或注入游戏。它不验证后续布局头生成、托管引用或机器码兼容性。

## 原生组件

```powershell
./build.ps1 -Engines livemaker
./build.ps1 -Engines gsx -BuildRoot ./build/gsx
./build.ps1 -Engines kirikiri -BuildRoot ./build/kirikiri
```

通常生成四个文件：`<Prefix>Host<bits>.exe`、`<Prefix>Injector<bits>.exe`、`<Prefix>Bridge<bits>.dll` 和 `<Prefix>Hook<bits>.dll`。KiriKiri 额外生成 `KiriKiriRuntime32.dll`。Utage 同时构建 x86/x64 组件。

输出目录为 `<BuildRoot>/release/<engine>`；未指定 `-BuildRoot` 时使用系统临时目录。构建不会执行组件安装或游戏注入。

## 托管桥

```powershell
./build.ps1 -Engines unity_mono -ManagedReferenceRoot ./references/unity-managed
./build.ps1 -Engines teamnimbus -ManagedReferenceRoot ./references/teamnimbus-managed
./build.ps1 -Engines yarn_spinner -ManagedReferenceRoot ./references/yarn-managed
```

引用目录需包含目标运行时对应的 .NET、Unity 及相关 UI/TMP 程序集，具体文件由各引擎 `build.ps1` 列出。引用程序集不随本仓库提供。C# 桥编译后作为字节数组嵌入 Hook DLL，无需单独发布生成的读取器 DLL。

这些脚本构建 Hook 原生组件和托管桥，不构建独立资源处理程序。Yarn Spinner 和 Nemk Novel 同时复制运行时字体及许可证。

## 显示注入集成

普通文本监听与显示注入是独立能力。快照、字形数据、激活及租约等输入遵循对应引擎的 Hook 协议，由集成应用提供；本仓库不包含生成这些输入的独立资源工具。

### 显示字体

`config/SourceHanSansSC-Regular.otf` 为 Hook 显示提供中文字体，采用 SIL Open Font License 1.1，许可见同目录 `SourceHanSansSC-LICENSE.txt`。

PJADV、VALKYRIA 等组件按现有集成目录布局读取共享字体：`<应用根目录>/hook/<engine>/` 放置组件，`<应用根目录>/config/` 放置字体。Nemk Novel 和 Yarn Spinner 的专用脚本将字体及许可复制到其组件输出目录。其他组件通过显示协议接收预构建字形；数据由集成应用提供。

分发字体时须携带其许可证。字体、字位映射和快照加载失败仍遵循对应组件的校验和回退，不得跳过检查强行启用。

## 源码与版本

`SOURCE_MANIFEST.json` 记录源码来源、导入及当前摘要。上游快照没有可核实的提交号，以文件摘要标识。分发构建产物时应提供该版本对应的源码、构建说明及许可证。
