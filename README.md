# NodSynth

NodSynth 是节点式复音合成器和可脚本化的音乐制作套件。它既能作为独立乐器或 VST3 插件演奏，也能让人或 Agent 从空白工程编排、多合成器联合渲染、混音和导出。

项目的两个目标是：提供可设计声音的合成器；提供 CLI、Agent 和图形界面共用的音乐制作核心。当前重点平台为 Windows，核心使用 C++20，离线制作无需音频设备。

## 现在能做什么

| 范围 | 功能 |
|---|---|
| 编曲 | Song v7、MIDI 导入/导出、轨道/片段/章节/角色、pattern/和弦/复制/移调/力度、事务编辑和撤销 |
| 音源 | 节点式合成、16 声部、立体声 Unison、Noise、ADSR、滤波与调制、one-shot 采样鼓、曲风/角色预设 |
| 自动化 | 实际参数与宏发现、采样精确 lane、版本化 base+modulation、独立 pump 和局部静音 |
| 混音 | insert、bus、send/return、EQ、压缩与侧链、Delay、Reverb、过采样 Saturation、前视 Limiter、延迟补偿 |
| 互通 | NodSynth VST3、隔离 VST3 乐器/效果器、FluidSynth/SoundFont、插件状态与资源收集 |
| 交付 | float32/PCM16/PCM24、显式抖动、混音/分轨、缓存重混、响度/true peak 分析、区间和版本 A/B 试听 |
| Agent | 有界查询、角色选择、预设搜索与试听索引、外部模型适配、提案预检、修订冲突与操作成本记录 |
| 界面 | Windows 合成器画布与屏幕键盘；基础 DAW 时间线、钢琴卷帘、混音条和播放 |

套件不是完整商业 DAW：不含录音、通用音频片段/时间伸缩、插件商店或发布级跨平台 UI。外部 VST3 效果链用于离线制作，不承诺低延迟实时宿主。技术验证不等于商业音质认证。

## 从这里开始

1. [构建与开发](docs/DEVELOPMENT.md)：工具链、依赖、运行入口和验证。
2. [使用手册](docs/USER-GUIDE.md)：运行附带示例，从建歌到导出；MIDI、资源、版本与常见问题。
3. [命令参考](docs/COMMAND-REFERENCE.md)：CLI 选项与全部公开编辑操作。
4. [声音与混音](docs/SOUND-GUIDE.md)：预设、采样、调制、路由、效果和外部插件。
5. [Agent 工作流](docs/AGENT-WORKFLOW.md)：发现 → 提案 → 审阅 → 应用 → 试听。

已构建后，在仓库根目录的 PowerShell 中执行（使用新的输出目录）：

```powershell
New-Item -ItemType Directory -Path build/my-first-song
./build/release/nod.exe song create build/my-first-song/song.json --bpm 128 --bars 4 --ppq 480 --json
./build/release/nod.exe song apply build/my-first-song/song.json --commands docs/examples/quickstart.json --expect-revision 1 --json
./build/release/nod.exe render build/my-first-song/song.json --output build/my-first-song/mix.wav --preview-output build/my-first-song/mix.preview.wav --report build/my-first-song/render.json --tail-seconds 3 --json
```

这是演示工作流的 4 小节工程，不是音质标杆。后续试听与修改见使用手册。

## 兼容与状态

当前 Song 格式 v7，可加载 v1–v6；旧 Patch 的声音语义通过显式版本/迁移保留。资源相对路径以 Song 所在目录为准，插件安装仍是外部依赖。Windows 的合成器与 VST3 路径已有技术验证；其他平台不等同于已完成发行。

文档依据当前实现整理；附带入门示例已验证建歌、编排、混音/分轨导出与分析。构建、验证入口和工程约束见[开发说明](docs/DEVELOPMENT.md)。
