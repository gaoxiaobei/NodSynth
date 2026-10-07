# 命令参考

[首页](../README.md) · [完整工作流](USER-GUIDE.md) · [声音配置](SOUND-GUIDE.md)

以下 `nod` 指构建出的 `build/release/nod.exe`。大写参数是需要替换的占位符，方括号表示可选项；不要原样输入。所有主要 `nod` 子命令支持 `--json`。不带参数会显示用法并以非零码退出，不依赖通用 `--help`。

## 歌曲与编辑

| 命令 | 选项与用途 |
|---|---|
| `song create FILE` | `--bpm 120 --meter 4/4 --bars N --ppq 480`；创建空白工程 |
| `song import-midi MIDI --output SONG` | 可重复 `--map SOURCE_TRACK:CHANNEL=PATCH`；channel 为 0–15 |
| `song export-midi FILE --output MIDI` | 导出 MIDI 事件 |
| `song validate FILE` | `--render-ready` 追加音源/自动化等准备检查 |
| `song apply FILE --commands BATCH` | `--expect-revision N --output FILE --dry-run --diff-detail summary\|notes\|full` |
| `song undo FILE` / `song redo FILE` | `--output FILE --diff-detail summary\|notes\|full` |
| `song query FILE` | 下表的视图与过滤参数 |
| `song propose FILE --adapter EXE --instruction TEXT --output BATCH` | 可重复 `--adapter-arg ARG`；返回提案，不自动提交，见 [Agent 协议](AGENT-WORKFLOW.md) |

| query 选项 | 含义 |
|---|---|
| `--view summary` | 轻量工程摘要 |
| `--view tracks` | 轨道元数据和音源 |
| `--view notes` | 音符查询；建议始终限制数量 |
| `--view capabilities` | 参数、宏、效果等实际能力与不可用原因 |
| `--view roles` / `workflow` | 角色选择、章节与可审阅起始链建议 |
| `--view sections` | 命名章节 |
| `--view legacy` | 兼容完整输出；也是省略 view 时的默认值 |
| `--track ID --role ROLE` | 按稳定轨道 ID / 角色筛选 |
| `--bars A:B` 或 `--start-tick A --end-tick B` | 时间范围；结束点排他 |
| `--limit N --cursor CURSOR` | 有界分页；使用上次结果的 cursor，不自行计算 |
| `--onset-only` | 按起音选择范围内音符 |
| `--expand-automation` | 展开自动化数据 |

## 预设、Patch、采样与插件发现

| 命令 | 选项与用途 |
|---|---|
| `preset list` | `--role ROLE` |
| `preset search TEXT` | `--role ROLE`；文本检索 |
| `preset inspect ID` | 音色信息、参数和依赖 |
| `preset audition ID --output WAV` | 渲染单音色试听 |
| `preset index --output NEWDIR` | `--role ROLE`；批量试听索引，目录应为新目录 |
| 以上 preset 命令 | 可用 `--presets ROOT` 指定音色库 |
| `patch inspect FILE` | 图与迁移/自动化诊断 |
| `patch create-stereo FILE` | 创建立体声 Patch |
| `patch create-unison FILE` | `--pad` 选择 pad 起点 |
| `patch migrate FILE --output NEWFILE --decisions JSON` | 显式迁移；目标不得已存在 |
| `sampler create-kit NEWDIR` | `--punch-kick` 使用 punch kick 版本；生成样本与 kit 清单 |
| `plugin inspect BINARY` | `--state FILE --class NAME`；隔离检查 VST3 乐器 |
| `effect inspect BINARY` | `--state FILE --class NAME`；隔离检查 VST3 效果 |

## 渲染

入口：`nod render SONG --output WAV`。

| 选项 | 默认/用途 |
|---|---|
| `--stems DIR --report JSON` | 分轨输出与持久报告 |
| `--sample-rate N --block-size N` | 默认 48000 Hz / 128 frames |
| `--bars A:B` | 小节范围，例如 9:13 为四小节 |
| `--start-tick A --end-tick B` | tick 范围 |
| `--section ID` | 命名章节；与其它范围选项互斥 |
| `--tail-seconds N` | 默认固定尾音 2 秒；长混响需加长 |
| `--max-tail-seconds N --tail-threshold N` | 接受这些参数，但当前 CLI 不切换自动尾音模式 |
| `--cache-dir DIR --no-cache` | 指定缓存位置 / 禁用缓存；默认输出目录下 `.nod-cache` |
| `--quality final\|draft` | 默认 final；draft 用于迭代，不代表交付质量 |
| `--freeze-external` | 显式允许冻结外部音源结果 |
| `--format float32\|pcm16\|pcm24` | 默认 float32；整数导出同时保留 float 母带 |
| `--preview-output WAV` | 生成 PCM16 试听副本 |
| `--pcm-overflow reject\|attenuate` | 整数超峰值时拒绝或整体衰减 |
| `--dither none\|tpdf --dither-seed N` | 显式量化抖动与 uint32 种子 |
| `--loose` | 放宽支持范围相关处理；不能替代资源修复或有效音源配置 |
| `--fluidsynth EXE --vst3-worker EXE` | 外部后端程序路径 |

渲染后检查报告中的缓存、依赖、非有限值及 `tailTruncated`。声音状态可能需要预跑，局部范围不承诺零预跑。

## 分析、试听、版本和成本

| 命令 | 选项与用途 |
|---|---|
| `analyze WAV` | 峰值、true peak、LUFS/LRA 等 |
| `compare A.wav B.wav` | `--match-loudness`；分析比较，不写匹配音频 |
| `play WAV --report JSON` | `--records JSON`；启动播放器并记录状态 |
| `audition record WAV --report JSON --heard --reviewer NAME` | `--records JSON`；仅在确实听过后使用 |
| `audition query WAV --records JSON` | 查看按音频哈希绑定的记录 |
| `version audition A.song B.song --output NEWDIR` | `--bars A:B` 或 `--section ID`；支持 tail-seconds、freeze-external、no-cache、fluidsynth、vst3-worker |
| `workflow metrics TRACE.json` | 统计调用、命令、重试、查源码与人工干预 |

## 编辑操作

所有操作放在 `commands` 中；下表是公开编辑接口，不是独立 CLI 子命令。`?` 表示可省略字段。

```json
{
  "schemaVersion":1,
  "requestId":"unique-intent-001",
  "baseRevision":1,
  "commands":[
    {"op":"create-track","id":"lead","name":"Lead"},
    {"op":"bind-preset","track":"lead","preset":"production-lead"}
  ]
}
```

批次原子提交，requestId 用于幂等重放；baseRevision / `--expect-revision` 用于拒绝过期编辑。固定 ID 建议由调用者明确提供，后续操作不依赖名字或轨序。diff v2 摘要中的 changedEntityCount 统计唯一实体，changeRecordCount 统计变化记录。

### 轨道、片段与音符

| op | 字段 |
|---|---|
| `create-track` | `id?`, `name?`, `order?` |
| `delete-track` | `track` |
| `rename-track` | `track`, `name` |
| `reorder-track` | `track`, `order`；只改顺序 |
| `create-clip` | `track`, `id?`, `startTick?`（0）, `length?`（4×PPQ） |
| `delete-clip` | `track`, `clip` |
| `duplicate-clip` | `track`, `clip`, `id?`, `startTick?` 或 `startBar?`；默认接在源片段后 |
| `move-clip` | `track`, `clip`, `startTick` |
| `add-note` | `track`, `clip?`（第一片段）, `id?`, `tick`, `duration`, `pitch`, `velocity`, `channel?` |
| `add-notes` | `track`, `clip?`, `notes`（上述音符对象数组） |
| `move-note` | `track`, `note`, `tick?`, `pitch?` |
| `update-note` | `track`, `note`, `tick?`, `duration?`, `pitch?`, `velocity?`；不支持修改 channel |
| `delete-note` | `track`, `note` |
| `delete-notes` | `track`, `notes`（ID 字符串或 `{ "id":"…" }` 数组） |
| `transpose-notes` | `track`, `semitones`, `clip?`, `startBar/endBar?`, `clamp?` |
| `scale-velocities` | `track`, `factor`, `clip?`, `startBar/endBar?`, `clamp?` |
| `add-pattern` | `track`, `id`, `startBar/endBar`, `grid`, `duration`, `offset?`, `pitches`, `velocities`, `truncate?` |

MIDI pitch 0–127，力度按 MIDI 范围校验，channel 0–15。音符操作的 tick 相对所属片段，实际起音为 clip.startTick + note.tick；片段位置、章节与自动化使用歌曲时间。pattern 的节奏值是整音符分数；音高序列可混合单音、休止、和弦，详见使用手册。

### 全曲、角色与章节

| op | 字段 |
|---|---|
| `set-tempo` | `bpm` 或 `points:[{tick,bpm}]`（也可 microsecondsPerQuarter）；替换速度图，从 tick 0 开始 |
| `set-time-signature` | `tick?`, `numerator`, `denominator`；tick 0 替换，非零添加变化 |
| `set-song-range` | `bars` 或 `endTick` |
| `set-role` | `track`, `role` |
| `set-section` | `id`, `name?`, `startTick`, `endTick`；按 ID 创建/更新 |
| `delete-section` | `id` |
| `repeat-phrase` | `id`, `track?`（所有轨）, `section` 或 `startTick/endTick`, `destinationTick`, `extendSongRange?` |

章节可重叠，只是范围元数据。repeat-phrase 仅复制音符，裁切跨界音符，生成新 ID；目标叠加，超出显式工程范围默认拒绝。

### 音源与资源

| op | 字段 |
|---|---|
| `bind-preset` | `track`, `preset`, `presetsRoot?`, `externalReference?` |
| `set-instrument` | `track`, `patch`, `pathBase?`（song-directory / cwd） |
| `collect-resources` | `includeAssets?`；不复制插件二进制 |
| `add-sample-resource` | `id?`, `path`, `license`, `source` |
| `set-sampler` | `track`, `name?`, `samples`；样本字段见声音手册 |
| `bind-sample-kit` | `track`, `kit`；相对路径以歌曲目录为准 |
| `add-plugin-resource` | `id?`, `path`, `license?`, `source?` |
| `add-plugin-state` | `id?`, `path`, `license?`, `source?` |
| `set-vst3-instrument` | `track`, `pluginResource`, `className`, `stateResource?`, `clearAutomation?` |

切换为 VST3 时 `clearAutomation:true` 显式清除原参数覆盖和 lane。FluidSynth 能渲染已有 external-cli 工程，但 CLI 尚无对应的绑定操作；不能用不存在的命令替代。

### 电平、参数与独立节奏增益

| op | 字段 |
|---|---|
| `set-gain` | `track`, `gain`（线性增益） |
| `set-pan` | `track`, `pan`（-1 左、0 中、1 右） |
| `set-gain-automation` | `track`, `points:[{tick,gain}]`；替换整条音量 lane |
| `set-mix-mode` | `track`, `gainMode?`（legacy / multiply）, `panMode?`（equal-power / balance） |
| `add-pump` | `track`, `startBar/endBar`, `period`, `recovery`, `depth`, `fadeMs?`, `skipBars?`, `skipIntervals?`, `mode?` |
| `clear-pump` | `track` |
| `set-audio-mute` | `track`, `startBar/endBar?` 和/或 `muteBars?`, `intervals?`, `fadeMs?`；空选择清除 |
| `set-parameter` | `track`, `parameter`, `value` |
| `set-parameter-automation` | `track`, `parameter`, `points:[{tick,value}]`, `valueDomain?`（normalized 默认 / physical）, `interpolation?`（linear 默认 / step） |

pump 是独立增益层，不替换 gainAutomation。depth 0–1，recovery 小于 period；已有 pump 时需 `mode:"replace"`。skipIntervals / intervals 使用 `{startTick,endTick}` 对象数组。skipBars / muteBars 使用小节编号数组。静音使用独立层，即使改变 pump 也保留过门静音。

pattern/pump 的主范围必须用 startBar/endBar；移调与力度的范围筛选也只支持这对小节字段。不要因为 query/render 接受 tick 范围，就假设所有编辑操作也接受；静音的 tick 区间应放在 intervals 中。

新建轨道默认 multiply：基础 gain 与 lane 相乘；旧 legacy 模式保持历史音量语义。`set-parameter` 的值采用目标原生域；宏使用 0–1。参数是否可写、可自动化以 capabilities 为准。

### 总线、路由与效果

| op | 字段 |
|---|---|
| `create-bus` | `id`, `name?`, `output?`（master）, `gain?`, `pan?`, `return?`, `mute?`, `inserts?`, `sends?` |
| `set-output` | `target`（轨道/总线）, `output`；空字符串断开直出 |
| `set-sends` | `target`, `sends:[{target,gain,position}]`；position 为 pre-fader / post-fader |
| `set-inserts` | `target`（轨道/总线/master）, `inserts` |
| `set-bus` | `target`, `gain?`, `pan?`, `mute?`, `name?` |
| `set-effect-automation` | `target`, `effect`, `parameter`, `points:[{tick,value}]`, `interpolation?` |

set-sends / set-inserts 替换整个列表；传空数组清空。不存在通用 delete-bus 或任意 remove-automation 操作，不要猜测命令。效果配置、参数表和路由示例见[声音与混音](SOUND-GUIDE.md)。

## 单音色渲染工具

`nod_render` 是独立工具，用于 Patch 调试和单音色 MIDI 渲染，不负责完整歌曲混音。

```powershell
nod_render --midi input.mid --patch patch.json --output instrument.wav --sample-rate 48000 --block-size 128 --tail-seconds 3 --json --report instrument.render.json
```

支持 `--track N`、`--channel 0..15`、`--merge-channels`、`--loose`、`--tail-threshold`、`--max-tail-seconds`、`--inspect`、`--validate`；不传 MIDI 的演示用 `--output` / `--seconds`（最大 30 秒），soak 模式上限 600 秒。多 MIDI 声道并不自动获得不同音色。

## 退出与诊断

`nod`：0 成功；1 输入/编辑/历史错误；2 依赖或准备错误；3 其它处理失败；4 I/O 错误；5 修订或 request 冲突。自动调用必须同时检查退出码、stdout 和 stderr；少数数值解析异常即使带 `--json` 也可能输出普通错误文本。

`nod_graphcheck` 是开发工具，其预期无效图场景也可返回 0，未知场景返回 64。不能把它的退出规则套用到 `nod`。
