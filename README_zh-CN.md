[English](README.md) | [Русский](README_RU.md) | [简体中文](README_zh-CN.md)

## 模组新时代由此开启

**你好，同志！**

我很高兴向大家展示我为《WRSR》游戏开发的自定义加载器。它通过代码注入实现对游戏的深度修改——可添加全新资源，包括地图上的新矿脉，并支持对这些资源的完整交互。我坚信，这个加载器将成为大规模、真正意义上的模组开发的开端。从长远来看，它为玩家期盼已久的各类游戏优化铺平了道路：

- 自定义基础设施、人行道、围栏
- 调整经济公式，修改寻路逻辑与算法
- 更改出行范围、公民工作时长
- 修改道路铺设算法
- 乃至添加全新机制，以及更多更多可能

该模组需手动安装：请将文件夹复制到游戏根目录，与media_soviet文件夹并列。然后打开该文件夹并运行启动器——tesmiolauncher.exe。同时请确保已启动Steam，该启动器会调用原版的SOVIET64.exe，并注入我的tesmioloader.dll库文件，其中包含了补丁代码。
该加载器基于钩子（Hook）原理实现——在游戏加载过程中对可执行文件进行补丁注入。若通过 Steam 正常启动游戏，运行的则是未修改的原版程序。

默认包含加载器本身及两个用于测试铜生产链的插件（可选）——即资源与矿脉插件（resources.dll 和 deposits.dll），以及与之配套的配置文件。

这是一项艰难的工作，您可以在 Boosty 上支持我 — https://boosty.to/tesmio/donate

该加载器使用 GNU GPL v3 许可证发布。完整许可内容见 → https://choosealicense.com/licenses/gpl-3.0/

此外，我还在开发一款名为 Socialist Union 的新游戏。更多详情，请查看我YouTube频道上的开发日志 — https://www.youtube.com/@tesmio
