# 构建、架构与验证

[首页](../README.md) · [使用手册](USER-GUIDE.md)

## 构建环境

需要 CMake 3.27+、Ninja、C++20/C 编译器。当前 Windows 构建使用 LLVM-MinGW（clang/clang++）与 Ninja；Windows 界面目标带 MinGW 风格链接选项，不把 MSVC 当成已经验证的直接替代。把编译器和 Ninja 的 bin 目录加入 PATH；运行时也需能找到相应工具链运行库。

仓库 `third_party/libebur128` 用于响度分析。构建 NodSynth VST3 和第三方插件 worker 需要 `third_party/vst3sdk/CMakeLists.txt` 及 SDK 内容。缺少 SDK 时条件目标不会生成；只需核心 CLI 时可显式关闭 VST3。依赖许可证保留在各自目录，仓库当前没有统一的项目 LICENSE，不应从技术可用推断已授予再分发许可。

在仓库根目录：

```powershell
cmake --preset dev -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset dev
ctest --preset dev
cmake --preset release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset release
```

纯核心配置可加 `-DNODSYNTH_BUILD_VST3=OFF`。`dev` 默认 Debug 并构建测试，`release` 默认关闭测试；不要用 Release 目录没有测试来判断测试通过。已有配置切换编译器应使用新的构建目录。

核心与设备 UI 分离，不依赖 JUCE。Windows WASAPI/winmm 外壳及 VST3 路径是当前实际重点；Linux/macOS 核心可移植目标不代表这些平台的插件/UI 已经可发行。

## 运行目标

| 目标 | 用途 |
|---|---|
| nod | Song、编曲命令、多音源混音、分析与 Agent 接口 |
| nod_render | 单 Patch MIDI 渲染、演示和 soak |
| nodsynth_app | Windows 独立合成器、节点画布、MIDI/屏幕键盘 |
| nod_daw | 基础 Song 时间线、钢琴卷帘、混音与播放 |
| nodsynth_vst3 | 可被其它 DAW 加载的 NodSynth 乐器 |
| nod_vst3_worker | 第三方 VST3 的隔离离线宿主进程 |
| nod_graphcheck | 图编译与诊断场景 |
| nod_q2_bench / nod_effect_bench / nod_dsp_calibration | 性能与 DSP 校准工具 |

构建输出位于 build/dev 或 build/release；VST3 的具体包路径以构建产物为准。应用运行入口与示例见使用手册。离线 CLI 不需要打开 GUI 或连接音频设备。

## 架构与修改位置

| 目录/模块 | 职责 |
|---|---|
| src/model、src/compiler | 图文档、schema、编辑、连接类型/作用域验证、调度与缓冲规划 |
| src/runtime | 声部分配、DSP 计划执行与切换 |
| src/nodes | 原生振荡器、Unison、滤波、控制/音频节点与 Patch 工厂 |
| src/effects | 原生效果配置、参数 schema 和 DSP |
| src/persist | JSON 与版本化 Patch 持久化 |
| src/midi、src/render | SMF、音乐时间转换与单音色离线渲染 |
| src/song | Song 文档/查询/事务、资源、预设、自动化、采样、混音、渲染、分析、提案和版本试听 |
| src/daw、apps/daw | 共享时间线逻辑与 Windows DAW 外壳 |
| apps/win32、apps/vst3 | 独立合成器和插件入口 |
| include/nodsynth | 公共 C++ 接口 |
| presets、scripts、tests | 声音资产、场景验证、自动测试 |

CLI 与 GUI 使用共享歌曲/编辑层。Agent 产生声明式命令，不直接操纵音频线程。新增功能应同时更新 schema/查询能力、事务/diff、序列化兼容、渲染准备和公开文档，不只增加 CLI 分支。

## 持续保持的约束

- 稳定 ID 承担对象身份，名字和顺序只是展示。Song 当前 v7，可读取 v1–v6；Patch 改变声音语义应显式版本化。
- Audio/Control/Gate/Note 不隐式互转；PerVoice/Global 边界明确；反馈必须经过真实延迟状态。
- 实时处理不承担文档 I/O、动态分配与旧计划回收；结构编译失败保留旧计划。
- 歌曲编辑原子提交、可撤销、幂等且检查修订冲突。副本预检不产生资产写入。
- 音源和效果参数通过真实能力发现；后端不支持的 lane 明确拒绝。物理单位、宏映射和准备期固定参数不能混用。
- 音频状态相关修改使对应缓存失效；混音修改尽可能复用 dry stem。无法准确恢复状态时保留预跑。
- 路由 DAG 和侧链共同参与延迟补偿；禁止无定义的环、运行中外部延迟变化和悄然错位。
- float 母带与整数交付分开；量化、抖动、溢出策略明确。播放、聆听记录和主观质量结论分别报告。

## 验证入口

改代码时按影响范围运行测试，再运行需要的公开接口场景。脚本通常写入 build 下的验证目录，部分拒绝覆盖；先阅读其 param 参数，选新的输出目录，避免破坏既有证据。

| 脚本 | 用途/依赖 |
|---|---|
| AcceptComposing.ps1 | 基础公开编曲流程 |
| AcceptProductionQuality.ps1 | 原生制作能力和性能场景 |
| BuildProductionRolePack.ps1 | 重建制作角色预设，不是普通用户安装步骤 |
| AcceptProductionProjects.ps1 | 完整 native/external 工程、混音和分轨；外部场景需 worker/插件 |
| AcceptProductionControls.ps1 | 单因素替换音源或空间效果对照，依赖生成的工程 |
| VerifyLoudness.ps1 | 响度分析对照，依赖脚本指定的参考工具 |
| VerifyLegacyDsp.ps1 / VerifyProductionRegression.ps1 | 固定基线兼容与性能回归；不是用户入门步骤 |
| AcceptAgentWorkflow.ps1 | 查询、角色、提案、版本试听与 trace；用 SourceSong 指定已有工程 |
| PrepareProductionListening.ps1 / PrepareProductionDetailListening.ps1 | 参考与片段听评材料；前者需要 Reference |
| IterateProductionSound.ps1 / BalanceProductionCandidates.ps1 | 生成独立声音迭代/电平对照版本 |
| CompleteProductionTails.ps1 / AcceptRefinedProductionProjects.ps1 | 尾音补全和细化工程验证 |

技术检查包括信号有限性、峰值/true peak、响度、声道、尾音、延迟、资源与缓存正确性。听感比较使用匹配响度的独立版本；不要以“LUFS 达标”代替商业声音评价，也不以播放器启动代替有人聆听。

## 文档维护

README 只陈述目标、现有功能和入口；使用手册负责工作流；命令参考负责接口；声音手册负责 DSP/混音语义；Agent 文档负责适配协议。实现变化时更新对应页面与例子，避免把里程碑、临时反馈和长篇施工日志继续堆入用户手册。历史实现记录可从版本控制追溯，当前行为以代码及可复现验证为依据。

文档示例于 2026-10-06 使用本地 Release 程序核验：quickstart 建歌、apply、render-ready、混音/分轨/试听副本导出与分析，以及混音修改、乐句扩展、pump、路由、效果自动化、采样鼓绑定、MIDI 导入/导出和 workflow 查询。另检查本地链接、JSON 示例语法及全部 47 个公开编辑操作的文档覆盖。本轮未重新运行代码全量回归；后续实现变动应重新验证受影响的例子。
