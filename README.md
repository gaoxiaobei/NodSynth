# NodSynth

NodSynth 是一个开发中的节点式复音软件合成器。用户在画布上连接振荡器、包络、滤波器和输出节点，构建可由 MIDI 演奏、保存和重新打开的音色。

**当前阶段：Windows 上可以打开默认音色、演奏、编辑节点并保存工程。** 核心仍是不依赖界面的 C++20 库，可在无音频设备时离线渲染 WAV。

后续同时发展独立合成器和 AI 编曲套件。`nod_render --midi` 渲染单音色。`nod song` / `nod render` 导入多轨 MIDI，按显式映射用多个 NodSynth 实例或 FluidSynth 输出混音和分轨。NodSynth 可作为 VST3 乐器加载，套件也能离线托管第三方 VST3。Windows 上的 `nod_daw` 提供钢琴卷帘、混音条和播放。

## 项目文档

- [产品目标与架构](docs/PROJECT.md)：产品边界、核心原则和当前实现。
- [开发路线与近期计划](docs/ROADMAP.md)：里程碑、交付物和验收条件。
- [编曲工作台迭代](docs/ITERATION-COMPOSING.md)：依据 Agent 实际编曲体验，改善原生创作、编辑反馈、音色和试听效率。
- [编曲命令指南](docs/COMPOSING-COMMANDS.md)：资源收集、乐句、参数自动化和试听记录。
- [D0–D2 交付验收](docs/history/COMPOSING-2026-10-05.md)：公开命令复测、性能与人工试听状态。
- [Agent 编曲体感建议](docs/AGENT-COMPOSING-FEEDBACK.md)：trance 复测后的开发优先级（资源路径、diff、乐句原语、预设与试听）。
- [后 MVP 设计](docs/COMPOSITION-SUITE.md)：歌曲工程、命令行/AI 接口、多合成器渲染及插件路线。

## 当前能力

- 歌曲格式 v3：自包含预设资源、完整实体 diff、乐句展开与和弦辅助、采样精确 cutoff/level 自动化、trance/house 预设，以及独立 PCM16 试听和确认记录。
- 图模型、端口校验、确定性调度、撤销/重做，以及版本化 JSON 工程。
- 16 声部运行时：振荡器、ADSR、低通、增益、控制运算、反馈延迟、Voice Mix 和立体声输出。
- 离线渲染命令 `nod_render`：演示音符、soak，以及 `--midi` / `--patch` 的 SMF 渲染、尾音和 JSON 报告。Windows 独立应用 `nodsynth_app` 提供 WASAPI 输出、MIDI 输入、节点画布和屏幕键盘。

Windows 应用只在 `WIN32` 下构建，Linux 和 macOS 的核心测试目标保持不变。本机验收记录见 [MVP 历史](docs/history/MVP-2026-10-04.md)。

## 构建与测试

需要 C++20 编译器、CMake 3.27+ 和 Ninja。默认启用测试，首次配置会从 GitHub 获取 Catch2 v3.15.0，需要可用网络或预先提供依赖。

```sh
cmake --preset dev
cmake --build --preset dev -j2
ctest --preset dev
./build/dev/nod_graphcheck --scenario valid
./build/dev/nod_graphcheck --scenario type-error
./build/dev/nod_graphcheck --scenario cycle
```

Windows 下可使用 `./build/dev/nod_graphcheck.exe`；使用 MSVC 时在已初始化编译环境的开发者终端运行上述命令。

三个场景的预期输出分别是 `valid perVoice=5 global=2`、`rejected code=port-kind-mismatch`、`rejected code=cycle-detected`。非法图被按预期拒绝时，场景工具也返回 0；未知场景返回 64。

离线渲染一段 48 kHz、128 采样的立体声 WAV：

```sh
./build/dev/nod_render --output build/dev/smoke.wav --sample-rate 48000 --block-size 128
```

成功时打印 `wrote 96000 frames` 并返回 0。Release 性能测量不写文件：

```sh
./build/release/nod_render --soak --sample-rate 48000 --block-size 128 --seconds 600
```

在 Windows 上启动可演奏的应用：

```sh
./build/dev/nodsynth_app.exe
```

`nodsynth_app.exe --self-test` 会编译默认音色、写一份临时工程并打开默认输出设备，成功时返回 0。
