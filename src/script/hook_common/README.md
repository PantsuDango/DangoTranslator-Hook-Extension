# Hook 公共构建与通信补丁

公共构建入口为 `build_first_text.ps1`，由仓库根目录 `build.ps1` 调用。

- `RuntimeBranding.patch` 调整通信 API 标识。
- `FirstTextHandshake.patch` 在首次文本注册和译文等待之间建立确认，避免快速响应丢失；断连或超时继续原文路径。
- 引擎品牌补丁可以调整产品与组件名称，必须保留上游作者版权。构建脚本检查源码及 Bridge/Hook 产物的版权字段。

默认上游源码为 `third_party/LunaTranslator`，MinHook 来自 `third_party/minhook`。脚本在独立构建目录应用补丁、生成共享布局头，并从源码构建 Host、Injector、Bridge 和 Hook。托管桥通过对应引擎的专用构建入口生成后嵌入。

```powershell
./build.ps1 -Engines livemaker
./build.ps1 -Engines yuris -PrepareOnly
```

构建参数、各组件的引用依赖和外部显示资源配置见 [构建指南](../../../BUILDING.md)。修改补丁后使用新的构建目录；准备检查通过不表示完成编译或游戏兼容验证。组件不会在构建时安装或注入游戏。

许可证与来源见 [NOTICE.md](../../../NOTICE.md)。
