# 编曲命令指南

适用版本：Song v3，2026-10-05。v1/v2 工程可加载；旧自动化保持 normalized 值域。下列命令使用构建后的 `nod`，Windows 可指定 `build/release/nod.exe`。

## 创建、编辑与审阅

```sh
nod song create song.json --bpm 138 --meter 4/4 --bars 16 --ppq 480 --json
nod song apply song.json --commands commands.json --expect-revision 1 --json
nod song query song.json --view summary --json
nod song query song.json --view capabilities --json
nod song validate song.json --render-ready --json
```

命令批次是 `{"schemaVersion":1,"requestId":"arrange-v1","commands":[...]}`。一次 apply 是一个事务，失败不提交；同一 requestId 重放相同内容不会重复编辑，不同内容报冲突。dry-run 不提交歌曲或收集资源。

apply/undo/redo 默认返回 diff v2 摘要。`changedEntityCount` 是唯一实体数，`changeRecordCount` 是变化记录数，`legacyChangedEntityCount` 保留旧统计口径；新增或删除父轨也统计嵌套片段和音符。每轨 notesSummary 提供数量和时间范围，`--diff-detail notes|full` 可展开详情。轨道命令使用稳定 ID，名称可以重复。

## 资源路径与搬移

resource.path 和 `set-instrument` 的相对路径均以 Song 文件目录为基准；如从当前工作目录导入，应显式传 `"pathBase":"cwd"`，或使用绝对路径。

```json
{"op":"bind-preset","track":"lead","preset":"trance-lead"}
```

bind-preset 默认复制固定版本至 `assets/presets/`，保留 preset ID/version 和内容哈希。`"externalReference":true` 可显式保留外部引用，但工程不再自包含。对已有工程执行 `{"op":"collect-resources"}` 收集 Patch/预设；`"includeAssets":true` 同时收集 SoundFont/采样资产，调用者应确认分发权限。插件二进制不复制。

搬移时复制 Song 和 assets 目录；通过公开 apply 的 `--output` 另存到新目录时会复制已收集资产并修正历史中的引用。validate 区分 missing、unreadable、hash-mismatch，并给出解析路径和哈希；外部引用及 junction 会提示可搬移限制。

## 乐句与变换

以下 commands 数组可从空白工程生成四拍底鼓和反拍低音：

```json
[
  {"op":"create-track","id":"kick","name":"Kick"},
  {"op":"bind-preset","track":"kick","preset":"kick"},
  {"op":"add-pattern","track":"kick","id":"kick-phrase","startBar":1,"endBar":17,
   "grid":"1/4","duration":"1/8","pitches":[36],"velocities":[110]},
  {"op":"create-track","id":"bass","name":"Bass"},
  {"op":"bind-preset","track":"bass","preset":"trance-bass"},
  {"op":"add-pattern","track":"bass","id":"bass-phrase","startBar":1,"endBar":17,
   "grid":"1/4","duration":"1/8","offset":"1/8","pitches":[45],"velocities":[100]}
]
```

小节从 1 起算，endBar 为排他端点；1:17 是 16 小节。`1/4` 表示四分音符，grid 和 duration 独立；PPQ 无法精确表示的分数拒绝。pitches 序列循环，null/空数组是休止，数组是显式和弦；也可使用 `{"root":57,"quality":"minor","inversion":1,"voicing":[0,0,1]}`。quality 仅支持 major/minor；octave 按 MIDI C4=60 指定。

pattern 展开成普通片段和独立音符，默认拒绝末尾越界，`"truncate":true` 显式截断。跨拍号按小节图换算，拒绝小节内部换拍号。

```json
[
  {"op":"duplicate-clip","track":"bass","clip":"bass-phrase","id":"bass-copy","startBar":17},
  {"op":"transpose-notes","track":"bass","clip":"bass-copy","semitones":12},
  {"op":"scale-velocities","track":"bass","clip":"bass-copy","factor":0.8},
  {"op":"add-pump","track":"pad","startBar":1,"endBar":17,
   "period":"1/4","recovery":"1/8","depth":0.7}
]
```

复制到工程范围之外需先扩大 song range。移调/力度变换可选 startBar/endBar，按起音筛选；越出 MIDI 范围默认拒绝，`"clamp":true` 显式限制。pump 替换该轨 gain lane，只是周期音量自动化。

## 参数与预设

```sh
nod preset list --json
nod preset inspect trance-lead --json
nod preset audition trance-lead --output lead.preview.wav --json
```

新增 trance-lead/bass/pad 和 house-bass/chord，与既有鼓预设组合。trance-lead 使用三个失谐 saw 振荡器。inspect 展示真实参数、宏映射和依赖，绑定后也可 query capabilities 检查当前音色。

```json
[
  {"op":"set-parameter","track":"lead","parameter":"macro:level","value":0.18},
  {"op":"set-parameter-automation","track":"lead","parameter":"filter/cutoff",
   "valueDomain":"physical","interpolation":"linear",
   "points":[{"tick":0,"value":350},{"tick":15360,"value":6500}]}
]
```

NodSynth 物理地址为 nodeId/parameterId，cutoff 单位 Hz，level 使用节点实际范围；macro:brightness、macro:level 使用 normalized 0–1。level 宏映射声音源振荡器/噪声的 level。可用地址以实际图为准。

lane 支持 step/linear，首点前使用基础值，末点后保持，采样精确执行，离线不额外平滑。未知地址、范围错误及不支持的 lane 在 render-ready 检查时拒绝，参数或 lane 改动使对应轨 dry 缓存失效。

VST3 使用 `vst3:十进制ParamId`、normalized 0–1，目前仅支持 linear lane；唯一的旧参数名称保留兼容。仅接收插件声明可自动化的参数，跨块线性插值与上述时间约定一致。VST3 基础参数覆盖暂不支持。FluidSynth 参数 lane 不支持。每个后端最多 64 个自动化目标；多目标宏按展开后的目标计数。

## 导出与试听记录

```sh
nod render song.json --output mix.wav --preview-output mix.preview.wav --stems stems --report render.json --json
nod render song.json --output slice.wav --bars 9:13 --format pcm16 --json
nod play mix.preview.wav --report render.json --json
nod audition record mix.preview.wav --report render.json --heard --reviewer "Your name" --json
nod audition query mix.preview.wav --json
```

默认母带与缓存保持 float32；preview 和 preset audition 使用 PCM16。`--format pcm16` 可直接导出兼容 WAV。量化确定、无抖动；超范围默认拒绝，`--pcm-overflow attenuate` 显式衰减，报告编码、导出增益及文件哈希。

play 调用 Windows 默认播放器，成功只记录 playbackStarted，仍是 unheard。人工确认后才执行 audition record；heard 不等于 approved。记录默认写入 AUDIO.audition.json，可用 `--records` 指定位置，独立于 render 报告，绑定 renderId、区间和实际文件内容哈希。新音频不继承旧音频的听感结论。

## 重复验收

```powershell
./scripts/AcceptComposing.ps1
```

脚本只用公开命令创建 16/17 小节 trance、4 小节 house，导出预览/分轨/报告，验证搬移前后的文件哈希并记录冷/暖/裁片耗时。每次使用独立冷缓存，默认产物在 build/acceptance-d0-d2，保留 compositions 中既有作品。音乐听感仍需人工确认。
