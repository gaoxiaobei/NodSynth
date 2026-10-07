# 声音、音源与混音

[首页](../README.md) · [使用手册](USER-GUIDE.md) · [命令字段](COMMAND-REFERENCE.md)

本页中的批次均通过 `nod song apply SONG --commands FILE` 执行。示例中的 lead、bass、kick 必须已存在；可先运行[入门示例](examples/quickstart.json)。资源路径、插件类名和参数地址需要对应实际工程。

## 选择与绑定音色

```powershell
nod preset list --json
nod preset search "production" --role lead --json
nod preset inspect production-lead --json
nod preset audition production-lead --output lead.preview.wav --json
nod preset index --role lead --output new-lead-index --json
```

| 用途 | 内置 preset ID |
|---|---|
| 基础鼓与乐器 | kick、snare-clap、closed-hat、open-hat、bass、lead、pad |
| 曲风起点 | trance-lead、trance-bass、trance-pad、house-bass、house-chord |
| 立体声齐奏 | stereo-unison-lead、stereo-unison-pad |
| 制作角色包 | production-lead、production-pad、production-pluck、production-offbeat-bass、production-sub、production-riser、production-impact |

role 是精确筛选，例如 kick、clap、hat、bass、lead、pad、pluck、sub、riser、impact；`--role drums` 不等于搜索鼓组，可改用 `preset search drums`。inspect 可查看实际宏和建议音区等元数据。

`bind-preset` 默认复制固定版本并记录哈希；可用 `presetsRoot` 指定其他库。预设命令支持 `--presets`；环境变量 `NODSYNTH_PRESETS` 可设置库目录，否则使用构建时内置位置。搬移程序时也要提供音色库。

## Patch 与合成能力

Patch 用节点和显式连接定义声音。Audio、Control、Gate、Note 各有类型；每声部与全局处理之间需要明确边界。当前合成器是 16 音符声部，Unison 每音符最多 8 个子振荡器，不等于把复音数改为 8。

可使用振荡器、Noise、ADSR、滤波、增益、混合、控制运算、反馈延迟、立体声转换和全局效果。新版 Unison 是带限立体声源，支持声部数、失谐与展开；音符声部数、Unison 层数、效果尾音都会影响性能。

```powershell
nod patch create-stereo stereo.json --json
nod patch create-unison unison.json --json
nod patch create-unison pad.json --pad --json
nod patch inspect unison.json --json
```

用 `set-instrument` 绑定自定义 Patch，例如 `{"op":"set-instrument","track":"lead","patch":"unison.json","pathBase":"cwd"}`。编辑具体节点前先 inspect。旧 oscillator 的 waveform 1 是保留兼容行为的漏积分 saw，不应作为已校准的理想 triangle 使用。

## 参数发现、基础值与调制

```powershell
nod song query song.json --view capabilities --track lead --json
```

不要凭经验猜 `filter/cutoff`。实际地址由节点 ID 和参数 ID 组成；查询返回范围、单位、基础值、宏、可自动化性和阻塞原因。`control-override` 表示旧 replace 控制线占用了参数，诊断会列出连接；`unknown-address` / `unsupported-target` 分别提示未知或不支持的目标。准备期固定参数不能写入运行时 lane。

新版 lowpass-v2 / highpass-v2 分开基础 cutoff Hz 与内部 cutoff-mod：

`有效 cutoff = baseHz × 2^(depthOctaves × modulation)`，modulation 为 -1..1。

这样外部慢速扫频可控制 base，内部包络/LFO 控制 modulation。旧节点仍保留原来的绝对 Hz/replace 语义，不能直接把旧线当作归一化调制线。

以下片段假设 capabilities 确认了对应地址：

```json
{
  "schemaVersion":1,
  "requestId":"tone-001",
  "commands":[
    {"op":"set-parameter","track":"lead","parameter":"macro:level","value":0.18},
    {"op":"set-parameter-automation","track":"lead","parameter":"filter/cutoff","valueDomain":"physical","interpolation":"linear","points":[{"tick":0,"value":350},{"tick":7680,"value":6500}]}
  ]
}
```

NodSynth lane 支持 normalized / physical 和 step / linear，首点前使用基础值，末点后保持，采样精确执行且离线不另加平滑。`set-parameter` 本身采用目标原生域；宏为 0–1。快速跳变可能产生可听瞬态，应设计合适的插值。

Patch 顶层可添加如下宏定义（此处展示顶层字段，不是独立 Patch）：

```json
{
  "macros":[
    {"id":"tone","mappings":[{"node":"filter","parameter":"cutoff","minimum":200,"maximum":12800,"curve":"log"}]}
  ]
}
```

Song 使用 `macro:tone`，值域 0–1。映射支持 linear / log 和反向端点；log 两端必须大于 0。定义宏不隐式改变基础声音；保留的 brightness/level 不能覆盖。两个 lane 或宏命中同一目标会冲突。每轨最多 64 个展开后的参数自动化目标。

| 后端 | 参数支持 |
|---|---|
| NodSynth | 原生参数基础覆盖、宏、physical/normalized、step/linear |
| VST3 乐器 | 插件声明可自动化的 `vst3:十进制ParamId`；normalized、linear；暂不支持基础参数覆盖 |
| Sampler / FluidSynth | 当前不支持乐器参数 lane；仍可使用音量和混音处理 |

## 显式升级旧 Patch

先 inspect，再为每个迁移候选写决定文件。下面只适用于相同节点/连接 ID 的旧图：

```json
{
  "cutoff":{"filter":{"action":"disconnect","baseHz":800}},
  "gain":{"gain":{"base":1}},
  "mono":{"oscillator/audio->filter/audio-in":{"pan":0,"level":1.4142135623730951}}
}
```

```powershell
nod patch migrate old.json --output new.json --decisions choices.json --json
```

输出必须不存在，原文件保留。已有归一化调制源时，cutoff 决定可采用 `{"action":"normalized","sourceIsNormalized":true,"baseHz":800,"depthOctaves":4}`。这是调用者对源值域的明确声明，迁移器不会自动缩放旧绝对 Hz。缺少、多余或无法编译的决定会拒绝；迁移后重新验证和试听。

## 采样鼓与 one-shot

```powershell
nod sampler create-kit new-kit --punch-kick --json
```

生成含 license/source 的原创鼓样本与 kit 清单。默认不加 punch-kick 时保留原版本；输出使用新目录。也可绑定仓库的 `presets/electronic-one-shots/kit.json`，先查看清单中的键位，不猜 MIDI 鼓号。

```json
{
  "schemaVersion":1,
  "requestId":"kit-001",
  "commands":[
    {"op":"create-track","id":"drums","name":"Drum Kit"},
    {"op":"bind-sample-kit","track":"drums","kit":"../../presets/electronic-one-shots/kit.json"}
  ]
}
```

这个 kit 路径适用于 Song 位于仓库 `build/my-first-song/`；其他位置必须改路径或使用绝对路径。

自有样本先 `add-sample-resource`，再 `set-sampler`：

```json
{
  "schemaVersion":1,
  "requestId":"sample-001",
  "commands":[
    {"op":"add-sample-resource","id":"kick-sample","path":"samples/kick.wav","license":"CC0-1.0","source":"original recording"},
    {"op":"set-sampler","track":"kick","samples":[{"resource":"kick-sample","rootNote":36,"lowNote":36,"highNote":36,"lowVelocity":1,"highVelocity":127,"gain":1,"fadeInMs":0,"fadeOutMs":2}]}
  ]
}
```

license/source 应填写真实来源。每个 sample 还可设置 chokeGroup、tuneCents、startSeconds、endSeconds。准备阶段预加载并重采样，16 声部；同键域/力度域多个样本支持 round robin，相同 MIDI 声道内的 choke group 用于开闭镲互斥。它不是磁盘流式采样器，也不是通用音频剪辑轨。

## 音量、声像、pump 与静音

新轨默认 `gainMode:multiply`，基础推子与 gainAutomation 相乘；旧 `legacy` 行为不自动改变。立体声音色可选 balance，避免把 equal-power 声像误当作保持左右声道平衡的控制。

```json
{
  "schemaVersion":1,
  "requestId":"pump-001",
  "commands":[
    {"op":"set-mix-mode","track":"bass","gainMode":"multiply","panMode":"balance"},
    {"op":"add-pump","track":"bass","startBar":1,"endBar":5,"period":"1/4","recovery":"1/8","depth":0.6,"skipBars":[4],"fadeMs":2},
    {"op":"set-audio-mute","track":"bass","intervals":[{"startTick":7200,"endTick":7680}],"fadeMs":2}
  ]
}
```

pump 是按节拍生成的独立增益层，不覆盖人工 gain lane，不等于由底鼓音频触发的侧链压缩。skipBars/skipIntervals 跳过抽吸；set-audio-mute 控制实际局部静音。已有 pump 时显式 `mode:"replace"`，`clear-pump` 只清 pump；空 set-audio-mute 清除静音。

## 路由、共享空间与侧链

轨道默认输出 master。可建立 bus/return，再设置直出和 pre-fader/post-fader sends。图必须无环；音频侧链同样参与依赖和延迟补偿，不能用任意反馈路由形成回路。

```json
{
  "schemaVersion":1,
  "requestId":"routing-001",
  "commands":[
    {"op":"create-bus","id":"room","return":true,"inserts":[{"id":"space","type":"reverb","parameters":{"decay":2,"wet":1}}]},
    {"op":"set-sends","target":"lead","sends":[{"target":"room","gain":0.15,"position":"post-fader"}]},
    {"op":"set-inserts","target":"bass","inserts":[{"id":"duck","type":"compressor","sidechain":"kick","parameters":{"thresholdDb":-18,"ratio":4,"attackMs":5,"releaseMs":100}}]},
    {"op":"set-inserts","target":"master","inserts":[{"id":"ceiling","type":"limiter","quality":"high","parameters":{"ceilingDb":-1,"lookaheadMs":5,"releaseMs":100}}]}
  ]
}
```

set-inserts / set-sends 替换整条链/全部 sends，应用前 query 或 dry-run，保留仍需使用的项目。共享混响通常设 wet=1，通过 send 控制量。set-output 的空字符串断开直出。

压缩器 sidechain 读取指定轨道或总线的轨后音频；detectorHighpass=0 绕过检测滤波。原生 bypass 保留状态与延迟并淡化切换。轨后、bus/return 与 master 补偿处理延迟，dry stem 保持源时间；共享处理或非线性母带后，分轨不保证简单相加重建母带。

## 原生效果参数

参数格式为 `名称 范围（默认值）`；省略 parameters 中的值采用默认。效果对象有 id、type、version（当前 1）、parameters、bypass、quality（standard/high）及 automation；compressor 可加 sidechain。

| type | 参数及范围 |
|---|---|
| delay | timeMs 1–8000 (375)；syncBeats 0–16 (0，关闭同步，单位四分音符)；feedback 0–0.95 (0.35)；pingPong 0/1 (0)；lowCut 20–20000 Hz (80)；highCut 20–20000 Hz (12000)；wet 0–1 (0.25) |
| reverb | preDelayMs 0–250 (20)；decay 0.1–15 s RT60 (2)；damping 200–20000 Hz (6000)；lowCut 20–20000 Hz (100)；highCut 20–20000 Hz (16000)；wet 0–1 (0.2) |
| eq | mode 0–4 (0)；frequency 20–20000 Hz (1000)；gainDb -24–24 (0)；q 0.1–20 (约 0.707)；wet 0–1 (1) |
| compressor | thresholdDb -60–0 (-18)；ratio 1–20 (4)；attackMs 0.1–200 (10)；releaseMs 5–2000 (100)；kneeDb 0–24 (6)；makeupDb 0–24 (0)；detectorHighpass 0–2000 Hz (80)；wet 0–1 (1) |
| limiter | ceilingDb -24–0 dBTP (-1)；lookaheadMs 1–20 (5)；releaseMs 5–2000 (100) |
| saturation | driveDb 0–36 (6)；outputDb -24–6 (-6)；wet 0–1 (1) |

EQ mode：0 bell、1 highpass、2 lowpass、3 lowshelf、4 highshelf。mode 和 limiter lookaheadMs 为准备期固定参数，不能自动化。其余可自动化参数仍应从 capabilities 确认，使用原生单位：

```json
{
  "schemaVersion":1,
  "requestId":"space-001",
  "commands":[
    {"op":"set-effect-automation","target":"room","effect":"space","parameter":"wet","interpolation":"linear","points":[{"tick":0,"value":0.6},{"tick":7680,"value":1}]}
  ]
}
```

saturation standard/high 执行真实 2x/4x 过采样，固定延迟 32 个源采样。limiter high 的 8x 是峰值检测，不是全链 8x 过采样；其 true-peak 限制较保守，可能比商业母带算法损失更多响度/动态。它不是保证成品响度和听感的自动母带按钮。

## 第三方 VST3

需要可用的 64 位 Windows 插件模块和 `nod_vst3_worker.exe`。先 inspect 实际模块文件；某些 `.vst3` 是目录包，不能把目录名直接当作内部模块文件。

```powershell
nod plugin inspect C:/Plugins/Synth.vst3 --class "Synth" --json
nod effect inspect C:/Plugins/Delay.vst3 --class "Delay" --state C:/States/delay.bin --json
```

检查返回 class、vendor/version、稳定参数 ID 与自动化标记；以下名字和路径必须换成实际结果：

```json
{
  "schemaVersion":1,
  "requestId":"external-001",
  "commands":[
    {"op":"add-plugin-resource","id":"synth-plugin","path":"C:/Plugins/Synth.vst3"},
    {"op":"add-plugin-state","id":"synth-state","path":"states/synth.bin"},
    {"op":"set-vst3-instrument","track":"lead","pluginResource":"synth-plugin","className":"Synth","stateResource":"synth-state","clearAutomation":true},
    {"op":"add-plugin-resource","id":"delay-plugin","path":"C:/Plugins/Delay.vst3"},
    {"op":"set-inserts","target":"room","inserts":[{"id":"echo","type":"vst3","pluginResource":"delay-plugin","className":"Delay","declaredTailSeconds":4}]}
  ]
}
```

```powershell
nod render song.json --output mix.wav --vst3-worker build/release/nod_vst3_worker.exe --tail-seconds 8 --json
```

状态在 activate 前恢复，单个状态文件最大 16 MiB；可以省略 state 使用插件默认状态。collect-resources 收集状态但不打包插件二进制。效果对象还可配置 workerTimeoutMs。VST3 效果当前支持 stereo 主输入、参数、BPM，不支持外部 sidechain。宿主是隔离进程中的离线同步处理；崩溃、超时或运行时延迟改变会取消导出并报告。冻结外部结果需显式选择，不把未冻结的外部状态当作可重复缓存。

## FluidSynth / SoundFont

渲染器支持 Song 中已有的 external-cli / FluidSynth 音源，渲染时通过 `--fluidsynth EXE` 指定程序，并提供对应 SoundFont 资源。它可与 NodSynth/VST3 轨联合渲染，但当前公开 CLI 没有创建该绑定的编辑操作，也不能通过 import-midi 的 Patch 映射直接绑定 `.sf2`。

已有此类工程可以继续使用；程序集成可查看 [SongDocument 接口](../include/nodsynth/song/SongDocument.h) 的 bindExternal。不要手工伪造资源哈希来绕过验证。需要完整 CLI 新建流程时，当前优先使用原生音源、Sampler 或 VST3。
