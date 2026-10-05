# 编曲工作台迭代：从能渲染到顺畅创作

日期：2026-10-05。源码基线：实现批次。状态：A/B/C 已落地。A/B/C 之后的真实曲风复测与开发建议见 [Agent 编曲体感建议](AGENT-COMPOSING-FEEDBACK.md)。C 含成品 mix 裁片、轨道 dry 缓存与仅改 gain/pan 重混音、耗时拆分、可解释静音、`nod compare`（可选响度匹配）以及 render 报告中的 quality/renderId/auditionStatus（默认 unheard）。公开命令复测（128 BPM、16 小节、6 轨）：Dev 冷全曲约 34 s、裁片约 16 ms、暖重混音约 181 ms；同一台机器 Release 冷全曲约 5.76 s、裁片约 5 ms、暖重混音约 50 ms（相对冷全曲约 115×，满足 ≤2 s 且 ≥5×）。联合渲染由测试 `NodSynth and FluidSynth share one timeline` 覆盖（已通过）。人工鼓听感仍需人耳验收，峰值/LUFS 不代替。

## 当前与历史范围

A/B/C 的实现与既有测量保留如下。后文 I1–I5、首次反馈表及 A/B/C 验收剧本是第一轮设计基线，不表示这些问题今天仍全部存在。**当前排期以文末“第二轮：选择性纳入 trance 复测反馈”为准**；原始体验报告保留为证据，不直接作为全部必须实现的需求。

## 第一轮目标（A/B/C）

以 Agent 制作《小星星 DJ 版》的实际体验为依据，将产品推进到：**从空白工程建轨、选音色、编排、检查修改、快速试听和导出，全程使用公开接口，不需要生成中间 SMF 或手写节点图 JSON。**

现有 MIDI、歌曲编辑、混音、外部合成器、VST3 与 DAW 界面已经提供底座。本轮优先改善创作闭环，不再以增加一种插件格式或 UI 面板作为主要进度。

## 证据与反馈解读

| 体验问题 | 核对结果 | 应解决的问题 |
|---|---|---|
| 必须先生成 MIDI | `applyOne` 没有建轨、命名和建片段命令 | 原生歌曲创作入口缺失 |
| 轨序与 MIDI 轨号不一致 | `MidiSong.cpp` 按首次 note-on 建立 stream | 来源身份、显示顺序和稳定 ID 未清晰呈现 |
| 混音 diff 空 | `applyCommands` 的 added/removed/changed 只比较音符 | 结果反馈不完整，Agent 无法可信审阅 |
| query 过大 | `querySong` 展开全部音符，只支持时间范围 | 查询缺少按任务分层和有界输出 |
| 鼓音色不合适 | 现有流程需要手写 Patch，缺少噪声源和可发现鼓预设 | 声音素材与选音色工作流缺失 |
| 试听几乎与全曲同样慢 | 区间渲染从零推进；现有报告全曲耗时约 27.99 秒 | 缺少耗时拆分、重用及便捷 A/B |
| 首尾静音常为 true | `AudioAnalysis.cpp` 用 leading > 0 / trailing > 0 判断 | 指标未对应音乐意义 |
| 无法听到结果 | Agent 提供的是数值及渲染产物，无听感证据 | 需要可靠播放交接与评价状态 |

`compositions/twinkle-dj/render.json` 记录 48 kHz、1509188 帧、峰值约 0.979、渲染约 28 秒，可作为基线材料。试听约 24 秒和 -15.1 LUFS 来自体验反馈，本次未复测或试听。`compositions/` 当前为未跟踪创作资产，本轮不改写或移动这些文件。

峰值低不能证明“被底鼓压住”：掩蔽还受频谱、时序、持续时间和音色影响。峰值 0.98 也只能说明采样峰值，不能证明没有 inter-sample peak。Agent 应说“电平差异值得试听”，不能仅凭数字声称完成听感修复。

## I1：可信且简洁的编辑接口（第一优先级）

### 完整语义 diff

为音轨、片段、音符、乐器绑定、增益、声像、自动化、tempo/拍号建立统一 diff。每条变化包含 entityType、稳定 ID、field、before、after；新增/删除包含恢复所需对象数据。长自动化数组默认给出点数、时间范围和内容哈希变化，并允许展开完整内容，不能只给空 changed。

`apply`、dry-run、undo、redo 共用同一差异生成器。返回 baseRevision、revision、requestId、changedEntityCount、影响音轨及渲染失效范围。区分“请求重放”“命令执行但无语义变化”“实际修改”。真正 no-op 不制造新的编辑历史；requestId 的幂等记录仍需持久化。

所有新编辑进入同一事务：先在副本上校验，再原子提交。失败不改变歌曲、历史或修订；同一 requestId 携不同内容应明确冲突。撤销/重做需检查修订是否仍能唯一识别状态，不能恢复成会被旧缓存误认的修订号；缓存最终以内容哈希判定。

### 按需查询

新增拟议查询方式：

```sh
nod song query song.json --view summary --json
nod song query song.json --view tracks --json
nod song query song.json --view capabilities --json
nod song query song.json --view notes --track bass --bars 9:13 --limit 100 --json
```

- summary：修订、tempo/拍号概览、歌曲范围、轨道/片段/音符计数及诊断，不含音符列表。
- tracks：ID、名称、显示次序、来源轨/通道、乐器、gain/pan、片段概要、自动化概要。
- notes/automation：按稳定 ID、轨道、片段和时间查询，支持 limit/cursor、total、hasMore 和 nextCursor；游标绑定工程修订，编辑后旧游标报失效。
- 时间查询明确区间为左闭右开，默认包含与区间相交的持续音，而非只返回在区间内起音的音符；可选 onset-only。
- capabilities 从实际命令注册表及后端生成，列出参数单位、范围、自动化能力和版本，避免帮助文本与实现分叉。

兼容策略：首先增加显式 view；旧 query 的输出保持一个兼容周期，Agent 适配器切到 summary，再在明确的 API 版本变更中调整默认行为。不能无告知破坏现有脚本。

### 轨道身份与命名

命令一律使用稳定 track ID；name 可以重复，不作为隐式唯一选择器。query 同时展示 id/name/order/sourceTrack/sourceChannel，帮助中明确 Song 轨 ID 与 `--map` 的源轨/通道不同。

新 MIDI 导入按 sourceTrack、sourceChannel 排列，再分配 ID；保留 MIDI Track Name 元信息，无名字才回退来源标签。首次起音变化不改变导入排序。既有工程不重新编号，不改原映射；重排只改变展示次序，不改变绑定和 ID。

落点：`src/song/SongDocument.cpp`、`src/song/MidiSong.cpp`、`apps/nod/Main.cpp`、模型适配器及 DAW 调用层。

**验收：** 单独修改 gain、pan、自动化均有准确 before/after；undo/redo 给出反向/正向差异；重复请求不重复修改。用源轨后进入的 MIDI 验证排序与映射。对 300 音符和 10000 音符工程，summary 大小随轨数而非音符数增长；6 轨示例目标不超过 8 KiB，分页无重漏。

## I2：从空白工程原生编曲

### 最小创建命令集

增加 `song create`，显式指定 BPM、拍号、PPQ 和歌曲范围；空工程及未绑定乐器的草稿可保存、查询、编辑。文档校验与 render-ready 校验分开，渲染时清晰指出缺失音色。不要让“最后一个音符的位置”成为唯一歌曲终点，空小节和结束留白也要保留。

通过 apply 增加 create/delete/rename/reorder-track、create/delete/duplicate-clip、批量 add/update/delete-notes、set-tempo、set-time-signature、set-song-range；尽量复用已有 move-note/move-clip。一个事务可创建轨道和片段并立刻引用新 ID，返回生成 ID 映射。

第一版允许调用者提供稳定 ID，并检查冲突。clip 具有明确 length，note 使用 clip 内位置；首版不引入隐式循环。复制片段产生新 clip/note ID，独立编辑，不共享可变内容；跨界音符默认拒绝，用户可显式扩展片段。删除轨道一并删除其片段/自动化，但不随意删除共享音色资源；未引用资源另行整理。

小节输入以 1 为起点，拍号按 tempo/拍号图换算；`--bars 9:13` 表示第 9 小节起点至第 13 小节起点，共 4 小节。机器模型继续使用确定的 tick，避免模型反复心算。重复片段、整体移调和力度缩放可作为后续批量命令，不把完整乐理生成器塞进首轮。

拟议流程：

```sh
nod song create twinkle.json --bpm 128 --meter 4/4 --bars 16 --json
nod song apply twinkle.json --commands arrange.json --expect-revision 1 --json
nod song query twinkle.json --view tracks --json
```

`arrange.json` 是公开编辑命令批次，不是直接重写内部工程结构。所有能力也供 GUI 使用，UI 不维护第二份编辑逻辑。

需要新增 clip length、歌曲范围等字段时，提升文件格式版本并提供旧版迁移；旧 clip 由内容末尾推断长度，空片段使用明确默认值并报告。不能把未来旧程序无法理解的新必需语义伪装成原格式。

**验收：** 不生成中间 MIDI、不手改工程 JSON，从空白建立 6 条命名音轨和 16 小节编排；复制后只改副本；删除/撤销恢复音符、乐器与自动化；保存重开 ID 稳定；MIDI 导出仍可用。失败批次完全回滚。

## I3：可发现的音色与真正可用的鼓

### 先提供预设入口

新增可版本化 preset registry，提供 list/inspect/audition/bind。元数据包含稳定 preset ID、版本、角色（kick/hat/clap/bass/lead/pad）、标签、宏参数、依赖和资源哈希。选择预设时将确定版本复制或锁定进工程，后续更新预设库不能让旧歌悄悄变声。

拟议 `nod preset list --role drums --json`、`nod preset audition ID --output preview.wav`。宏参数映射节点稳定参数 ID，提供单位、范围和自动化支持。常见音色修改不再要求 Agent 构造节点图；高级用户仍能打开图编辑。

### 最小原生声音集合

1. 新增确定性 Noise 节点，白噪声优先，明确项目 seed、每实例/声部状态和 reset 行为；随机序列不能依赖线程执行顺序。
2. 将现有滤波能力扩展为适合镲/拍手的高通或带通，并提供短打击包络；需要音高下滑的 kick 使用明确的音高包络映射。
3. 提供 kick、snare/clap、closed hat、open hat、bass、lead、pad 七类起步音色，统一输出余量、力度响应和宏参数。
4. 首轮用独立音轨承载不同鼓件，不先造复杂鼓架。鼓组映射、开闭镲 choke、采样器随后独立迭代；Noise 不等价于已实现可信鼓组。

可同时利用已接入的 SoundFont/VST3 作为选音色来源，依赖和授权需在工程中说明；默认入门路径使用随仓库可分发的自有预设，不要求商业插件。

**验收：** 不手写图 JSON 即可选择并渲染各预设；相同输入/seed/配置输出可重复；正常工作范围无 NaN/Inf、严重 DC 或失控峰值。鼓样例必须人工试听，检查瞬态、衰减、高频刺耳和配合底鼓的可用性；仅“噪声非零”不算声音验收。

## I4：让试听迭代快起来

### 先测量，再缓存

第一步报告 build 类型、硬件、prepare、预跑、DSP、外部进程、混音、文件写入、分析的耗时，以及 renderedFrames/emittedFrames、实时倍率。用同一 Release 环境复测完整 16 小节与第 9–13 小节。不能仅凭当前 24/28 秒推断瓶颈是 DSP；编译配置和外部启动开销尚需确认。

优先顺序：

1. **已有成品裁片。** 工程声音依赖和渲染配置未改变时，从已完成的 mix 裁出准确区间，不重跑合成器。
2. **轨道音频缓存。** 缓存乐器/插入效果输出、轨道 gain/pan 前的音频；只改 gain/pan/轨道增益自动化时重混音。改变音符、音色或乐器参数自动化只重渲受影响轨道，初版重渲整轨以保留状态正确性。
3. **更细预跑优化。** 仅当后端明确能精确恢复运行状态，才增加检查点；保存 Patch/插件 preset 不等于保存振荡器相位、包络和延迟线。无法恢复时继续从零预跑，不能默认从区间第一拍硬起。

缓存键包含音符/控制器/自动化、音色及依赖内容哈希、tempo/拍号、后端和版本、采样率、块长、seed、尾音和音频路由。仅改名称或显示顺序不失效声音缓存；用全歌曲 revision 作为唯一键会丧失局部复用能力。下游总线/效果变更沿依赖传播失效，反馈路由暂不纳入增量渲染。

首版只为声明确定性的内嵌后端启用自动复用。外部非确定性后端可显式 freeze（沿用已有音频）或 fresh render，并在报告标识；不暗示冻结输出等于重新演奏。缓存有配额、清理、校验、临时写入和原子提交；损坏缓存自动重算。

区分 quality=final 与显式 draft，报告不可混用缓存；不通过悄悄降低采样率或截断状态来声称提速。

**验收目标（固定机器 Release，以复测记录为准）：** 相同成品裁 4 小节目标 ≤1 秒；仅改 gain/pan 的暖缓存试听 ≤2 秒且比冷渲染至少快 5 倍；冷渲染先记录基线，不提前承诺倍数。各缓存路径与 fresh render 在定义容差内一致。报告命中/失效原因；变速、尾音、随机种子变化不误命中。

## I5：分析要可解释，试听要可交接

将 leading/trailing silence 布尔值替换或补充为首个/最后有效活动位置、leading/trailing duration、有效活动比例、阈值和窗口长度。检测采用多通道短时 RMS 窗口与最短持续时间，建议起始默认窗口 10 ms、门限 -60 dBFS、最短静音 100 ms，均可配置；阈值不是“音乐上应该静音”的判断。

独立 WAV 只陈述测量；结合 Song 时区分预期编排留白、ADSR 起音、释放尾音和整轨异常静音。全静音文件单独标识，活动起止为 null，避免双重计算。旧布尔字段保留一个兼容周期，并声明其旧语义。

新增 sample peak dBFS、RMS、区段短时响度、轨道活动区间，以及测量是否受支持。LUFS 对照参考实现和 mono/stereo 测试；现有分析对 mono 能量累加与多声道处理需专门核验，不能未经校准作为标准测量。true peak、LRA 在实现并验证后才报告；采样峰值不代替 true peak。

试听产物提供稳定路径、工程修订、渲染 ID、区间、质量模式和状态；GUI/CLI 可打开播放器，AI 客户端可展示音频附件或链接。没有设备/音频理解能力时状态是“未试听”，不是失败也不是质量合格。A/B 对比锁定同一区间，并可显式做响度匹配，避免把“更响”误判成“更好”。

**验收：** 1 sample 的低电平起音不产生有意义静音告警；已知 500 ms 留白报告时长准确到一个检测窗口；整轨无声和正常 release 可区分；不支持的响度格式明确返回 unavailable。Agent 的报告能区分测量事实、听感假设与已完成的人类试听。

## 交付顺序与发布门槛

| 批次 | 范围 | 退出条件 |
|---|---|---|
| A 接口可信 | I1，附 I5 静音时长修正和耗时拆分 | 混音有完整 diff，query 可控，来源/ID 清晰，诊断不再因单采样误导 |
| B 原生创作 | I2 + I3 预设入口和首批鼓音色 | Agent 从空白完成歌曲，不绕行 SMF、不手写 Patch |
| C 快速试听 | I4 + I5 试听交接及分析校准 | 暖缓存试听达标，失效正确，A/B 和听感状态可追溯 |

每批都有独立行为测试和真实 Agent 复测，既有 MIDI 导入、外部合成器、VST3、保存/撤销流程不回归。优先增加针对边界的测试，而不是只验证新命令被识别。

复测固定任务：128 BPM、16 小节、6 条命名音轨；第 3 小节进镲、第 5 小节进拍手/低音、第 9 小节进入旋律，后半段高八度变化，pad 做节拍音量闪避。这里的音量自动化不宣称是侧链压缩器。

保留现有作品作参考，另建验收工程；Agent 只能使用公开命令和预设，不读取源码寻找接口、不调用私有脚本制造 MIDI/音色。允许输出公开 apply 命令批次。记录完成所需工具调用数、query 字节量、重试次数、冷/暖试听耗时、用户干预和人工试听结果。

最终通过条件：从零创建、命名、复制编排；每次实际修改可审阅；查询只取所需数据；能够选用合适鼓预设；改混音无需全曲重合成；导出 mix/stems/报告并明确是否试听。另做一次两种合成器联合渲染回归，不能只在纯 NodSynth 示范中通过。

本轮暂缓新插件格式、商店、录音/拉伸、复杂鼓架、通用 DSP 快照、自动“审美打分”和大规模 UI 改版。先让已有能力成为可靠、容易操作的编曲工作台。

## 第二轮：选择性纳入 trance 复测反馈

设计日期：2026-10-05。依据 [Agent 复测反馈](AGENT-COMPOSING-FEEDBACK.md)，并检查资源绑定、语义 diff 与渲染代码。以下设计增量已实现，自动验收与性能复测已完成，人工音乐质量试听待确认；既有创作产物保留。交付证据见 [D0–D2 验收记录](history/COMPOSING-2026-10-05.md)，操作见 [命令指南](COMPOSING-COMMANDS.md)。

### 取舍与优先级

| 建议 | 决定 | 理由与边界 |
|---|---|---|
| 资源路径统一、工程可搬移 | D0 优先纳入 | 是正确性问题；默认复制资源，不使用 junction 作为交付方案 |
| 大批量 diff 摘要 | D0 优先纳入 | 统计嵌套对象，不通过完整 dump 数百音符解决 |
| NodSynth 参数自动化 | D0 先禁止静默忽略，D1 完成执行 | 接受命令却不改变声音，比少一个预设更容易误导 Agent |
| PCM16 与试听记录 | D1 调整后纳入 | 保留 float32 母带默认值，增加便捷 PCM16 试听产物；播放启动不等于已听 |
| pattern、移调、力度、和弦 | D2 缩小后纳入 | 先实现确定、可展开的编辑糖，不增加长期运行的生成器或完整乐理系统 |
| 曲风预设及宏参数 | D2 纳入 | 宏绑定与自动化先可靠，再提供精简 trance 包；house 用作泛化复测 |
| 按轨并行与更多 DSP | 后置，按测量/试听选择 | 暖重混已远超目标；不同时建设混响、压缩器和新调度器 |
| preset import、工程模板 | 后置 | 先统一资源绑定与可搬移，再扩展 registry 管理与模板，模板不得依赖 junction |
| 响度归一化 | 后置 | -18 LUFS 不是错误；需先有校准测量、true-peak/限幅策略及输出报告 |

不采纳“当天可修”的工期判断；资源事务和兼容测试完成后再估计。也不将 float32 WAV 普遍判定为不可播放，只记录本次播放器的兼容问题。

### D0：修正资源与编辑反馈的可靠性

**统一路径契约。** 工程中 resource.path 的相对路径一律相对 Song 文件所在目录。apply/validate/render/GUI 使用同一资源解析服务，显式传入工程目录；不能修改进程 CWD 代替上下文。`set-instrument` 的相对路径也按此规则解释；如需从 CWD 导入，提供显式 pathBase/sourceBase 选项，或先传绝对源路径。禁止“Song 目录找不到就试 CWD”的隐式回退。

源码中 `set-instrument` 目前直接拿命令路径计算 hash，渲染却使用 `baseDirectory`；`bindPreset` 计算 registry 文件 hash 后存入另一条工程路径。这与反馈一致。

**默认自包含。** `bind-preset` 将固定版本及所需依赖复制到工程的 `assets/presets/`，使用版本和内容哈希避免同名覆盖，存相对路径和原始 preset ID/version/hash。自定义 Patch 也提供同一 collect/copy 路径。显式外部引用可以保留，但 validate 标记 non-portable；插件二进制不自动复制，SoundFont/采样等资产按明确的收集选项及分发权限处理，不能把所有工程都宣称完全自包含。

资源复制先暂存和校验，再提交引用；dry-run 不创建正式资源，失败批次不留下指向半成品的工程，重复 requestId 不重复复制。通过内容寻址与原子文件提交保证中断后旧工程仍有效，未引用暂存文件可清理。撤销先移除引用，不删除其他修订或工程可能使用的资源。

诊断区分 missing/unreadable/hash-mismatch，包含 resourceId、原始路径、pathBase、resolvedPath、expectedHash 和 actualHash；无可读文件时 actualHash 为 null，不能假装是哈希不匹配。已有 junction 工程提供显式 collect/migrate 操作，不静默重写用户资源。

**准确但紧凑的 diff。** 当前新增轨道分支直接 continue，跳过其子片段/音符；`changedEntityCount` 实际近似差异条目数，而非唯一实体数。增加 schemaVersion、changeRecordCount、按 entity 分类的 added/removed/modified 计数，以及每轨 notesSummary（数量、起止、删除/变化数量）。父轨被新增也须统计子实体；每个实体 ID 在同一分类只计一次。明确 changedEntityCount 新语义并提供兼容字段，避免旧消费者误读。

默认摘要不展开音符；显式 diff detail/notes 选项返回完整数据或分页引用。可选 `snapshot.summary` 复用 query summary，并绑定提交后的 revision。invalidateRange 是保守的声音影响范围：删除/移动同时覆盖旧、新位置；有状态 DSP、尾音和 tempo 变化可能要求扩展到后续，不强求等于音符包围盒。检查全量 tempo/拍号点的变化，不能只比较首点与数组长度。

**自动化先诚实声明。** 目前 lane 被整理后主要送入 VST3 工作进程，内嵌 NodSynth 尚无对应消费路径。D1 完成前 capabilities 不声明该后端已支持；render-ready 校验明确拒绝无法执行的 lane。编辑草稿可保留 lane 并显示诊断，不能把它静默渲染成静态音色。

**D0 验收：** 从仓库根、工程目录和其他目录操作同一 Song 结果一致；复制已收集的工程目录后不依赖原 registry/junction 仍可渲染；覆盖带空格/中文路径、同名不同内容、缺失/变更资产、失败事务和请求重放。一次新增多轨及 602 音符的 apply 摘要计数可核对，删除父轨统计完整；同长度 tempo map 中间点变化可见。未知或未支持参数自动化明确失败。

### D1：参数确实改变声音，试听确实可交接

**统一 lane 语义，保留后端单位。** 为 NodSynth 参数定义稳定 nodeId/parameterId 地址，预设宏使用稳定 macroId 映射；VST3 参数继续保留原稳定 ID。参数元数据声明单位、范围、值域（physical/normalized）、可自动化性和 step/linear 插值。不能将 cutoff Hz 当作 VST3 的 0–1 值，也不能对旧 lane 静默改单位；必要时版本化迁移。

离线调度在准确 sample offset 应用事件，明确同刻事件顺序、首点之前使用基础值、末点后保持及平滑规则。事件容量预检，不在音频处理路径临时分配；不可只在块边界调用 UI 调参接口。宏的多个目标按声明映射同步更新，重绑不兼容 Patch 时给出失效地址，而不是忽略。

首批只承诺现有 DSP 真正暴露的 cutoff 和 level；delay mix 等先核对节点是否有相应参数，没有就不在 capabilities 或预设宏中宣称。自动化属于乐器输出依赖，必须使该轨 dry 缓存失效；轨道 gain/pan 仍只触发重混音。局部试听继续正确预跑。

**D1 自动化验收：** 一个静态输入音色的 cutoff 扫频在频谱上可测，且有/无 lane 的 WAV 明确不同；参数落点测试覆盖跨块及 64/128/512 块长。fresh/cached、全曲裁切/局部预跑在容差内一致；只改 lane 会重渲正确轨道；与 VST3 的插值/时间约定一致，不要求两种乐器波形相同。

**输出与试听状态。** 保持现有 float32 渲染默认值和缓存精度，新增显式 `--format pcm16`，并提供一次渲染同时生成 `.preview.wav` 的选项。面向试听的命令默认 PCM16，母带输出不降精度。量化仅发生在导出阶段，报告编码、抖动方案/seed、超范围采样处理；默认拒绝静默硬削波，用户显式选择衰减/限幅策略。导出格式变化可重用 float 缓存。

提供打开已有试听文件的稳定入口（优先复用现有播放能力），如新增 `nod play`，失败必须报设备/播放器问题。audition 记录独立于不可变 render manifest，引用 renderId、文件内容哈希、区间、评价者与时间。打开成功仅标记 playbackStarted；只有人明确确认才记录 heard，heard 也不等于 approved。换了音频产物默认 unheard；旧产物的听感记录保留，不冒充新版本评价。

**D1 试听验收：** 本机默认播放器能打开 PCM16 试听产物；float 母带和兼容预览分别标识；无人确认时保持 unheard；明确确认后能查询记录，重渲不同音频不继承 heard。报告持续区分 metrics、agentHypothesis 和 audition。

### D2：用乐句表达编排，用宏调整曲风

首批纳入 add-pattern、transpose-notes、scale-velocities，以及按小节复制片段。pattern 仅支持显式拍长/网格、offset、范围、音高和力度序列；音符时值与网格间隔分别指定。底层一次展开为普通 clip/notes，拥有独立稳定 ID，可撤销、可 diff、幂等重试，不引入可变共享循环引用。

节拍与时值用精确分数，定义 `1/4` 为四分音符而不是四分之一拍；小节遵循拍号图，跨拍号变化逐段展开。无法在工程 PPQ 精确表示时明确报错或要求显式量化。默认不隐式 humanize；以后加入随机变化必须有 seed。

和弦辅助先支持根 MIDI 音高、major/minor、转位、显式 octave/voicing，其他和弦品质按实际实现扩展；暂缓 degree 调式推导、自动和声与 groove 模型。`pump` 可生成普通 gain lane，不能称为基于音频检测的侧链压缩。首批可用生成命令批次完成，不必同时增加另一套 gen-pattern CLI。允许 Agent 用脚本生成公开 commands JSON；禁止的是绕开接口改私有工程或先造 SMF 作为必要入口。

曲风包先提供精简的 trance lead/bass/pad，加现有可用鼓音色，必要时增加 house 对应角色。每个预设附版本、tags、宏映射、代表性试听、音量余量和依赖；`supersaw` 等名称必须符合实际 DSP，不用标签掩盖缺失能力。优先复用现有振荡器组合；只有试听证实不足才引入 detune/unison。混响、chorus、侧链压缩器不作为整包同时开工项。

**D2 验收：** 不列逐 tick 音符即可表达 16 小节四拍底鼓、反拍低音和 Am–F–C–G 和弦；17 小节场景额外验证尾部非整循环边界。复制与移调后原片段不变，范围/越界/力度限制有明确行为。只使用 registry 预设及公开宏完成 trance 主验收，再做一个小型 house 对照，避免只为单首示范硬编码。曲风辨识和质量由人试听，尚无试听则标记待验收。

### 第二轮发布与保持项

顺序为 **D0 资源/反馈正确性 → D1 自动化与试听 → D2 乐句和曲风**，每批独立交付。C 批暖缓存已显著达标，先保持，不把按轨并行或外部冻结作为此轮额外门槛；每轨 dspMs 可随诊断改进纳入，实际瓶颈明确后再排并行。

复测从仓库根目录开始，138 BPM、16 小节、至少 7 轨 trance；补 17 小节边界测试与短 house 对照。检查工程搬移、批量摘要、cutoff 自动化、可播放预览与人工确认。保持 summary <8 KiB 的既定规模门槛和暖重混 ≤2 秒且较冷渲 ≥5×，并回归外部合成器、MIDI 导入、撤销和既有工程加载。

原始反馈与 `compositions/trance-drop/` 只作为证据保留，另建验收产物，不覆写创作文件。验收记录分别列 automated passed、试听待办/结果和已知限制；“已渲染”不再同时代表“可搬移”“自动化生效”或“听感达标”。
