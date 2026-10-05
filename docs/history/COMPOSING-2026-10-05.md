# D0–D2 编曲交付与验收

日期：2026-10-05。当前路线的 D0–D2 已实现，自动验收通过，人工曲风与音乐质量试听待确认。操作见 [编曲命令指南](../COMPOSING-COMMANDS.md)。

## 交付范围

- D0：统一工程目录路径契约、事务资源复制与显式收集、保存到新目录时搬移资产和历史引用、资源诊断、嵌套实体 diff v2 摘要、请求内容冲突与单调 revision。
- D1：Song v3 与 v1/v2 迁移、实际参数元数据、NodSynth 采样精确 cutoff/level 与宏自动化、缓存失效与每轨耗时、VST3 稳定参数 ID 和跨块插值、PCM16 预览、播放交接及独立哈希绑定试听记录。
- D2：精确分数 pattern、major/minor 和弦辅助、独立 ID 的片段复制、移调/力度变换、gain pump、五个 trance/house 预设，以及公开 CLI 验收脚本。

## 自动验证

Debug/Release 构建通过。最终 `ctest --preset dev`：136/136 通过，43.25 秒。VST3 自动化专项：8 个断言通过，涵盖中间 ramp 电平与旧名称兼容。测试覆盖资源事务/Unicode/CWD/保存历史、602 音符嵌套摘要、未知自动化拒绝、cutoff 音频变化、跨块及缓存一致性、部分轨失效、多目标宏、PCM16 与试听隔离、17 小节和拍号边界。MIDI、撤销、外部后端与联合时间线回归通过。

公开接口验收脚本：`scripts/AcceptComposing.ps1`；输出总表：`build/acceptance-d0-d2/acceptance.json`。未改写 compositions 中的用户作品。

| Release 场景 | 轨/音符 | 冷渲染 | 暖缓存重混 | 提速 | 成品裁片 |
|---|---|---|---|---|---|
| 138 BPM trance，16 小节 | 7 / 528 | 6806.95 ms | 215.60 ms | 31.57× | 34.77 ms |
| 138 BPM trance，17 小节 | 7 / 561 | 11677.02 ms | 298.39 ms | 39.13× | 32.93 ms |
| 124 BPM house，4 小节 | 7 / 124 | 1765.54 ms | 85.36 ms | 20.68× | 36.62 ms |

均满足暖缓存 ≤2 秒、较冷渲 ≥5×；summary <8 KiB。脚本暖渲复用已生成的 dry 缓存，gain/pan 修改重混和参数修改局部失效另有行为测试。三个工程复制 Song/assets 到新目录、无缓存渲染后与原音频哈希一致。以上是本机 Release 测量，不是其他机器的性能保证。

## 试听与限制

Windows 默认播放器成功打开 `build/acceptance-d0-d2/trance-16/mix.preview.wav`，记录 playbackStarted。所有产物仍为 unheard，approved=false；未代替人耳确认鼓瞬态、频谱掩蔽、trance/house 辨识或音乐质量。母带 float32 与试听 PCM16 分别标识。

试听路径为各场景目录下 mix.preview.wav；母带 mix.wav、分轨 stems、不可变 render.json、warm.json、slice.json 和 metrics.json 同目录。新预设代表性试听位于 build/acceptance-d0-d2/presets。

PCM16 无抖动，超范围默认拒绝或显式衰减，不提供限幅器。pump 为 gain 自动化。曲风包复用既有 DSP，未加入 unison 引擎、混响、chorus 或音频侧链检测。工程自包含承诺限于已收集资产，插件安装仍是外部依赖。按轨并行、registry 导入/模板、响度归一化及更多 DSP 按原计划后置。
