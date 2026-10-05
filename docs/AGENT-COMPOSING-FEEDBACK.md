# Agent 编曲体感 → 开发者建议

日期：2026-10-05。证据来源：公开 `nod` 接口从空白制作约 30s uplifting trance drop（`compositions/trance-drop/`），含库存预设版与自定义 patch 高质感版。机器：Windows · Release `nod.exe`。听感验收状态：产物已导出为可播放 pcm16；报告仍为 `auditionStatus=unheard`。

本文是**创作闭环复测后的产品建议**，不是架构重写提案。已实现的 A/B/C（见 [编曲工作台迭代](ITERATION-COMPOSING.md)）视为底座；下面按「还卡创作」排序。

## 1. 结论（给排期用）

接口闭环已经能完成「建歌 → 编曲 → 改混音 → 快试听 → 导出」。当前短板不在“能不能渲”，而在：

1. **资源路径与工程自包含**（易失败、浪费排查时间）
2. **编排语义太细**（Agent 被迫写 tick 工厂，而不是写乐句）
3. **音色天花板与可调性**（库存预设偏通用；NodSynth 参数自动化未真正驱动渲染）
4. **试听交接**（float32 WAV、未试听状态、人耳评价入口弱）

建议下一迭代以「真实曲风验收（trance/house 各一首）」为门槛，而不是再加一条仅存在于帮助里的命令。

## 2. 已验证可用（请保持）

| 能力 | 体感 | 证据 |
|---|---|---|
| `song create` / `apply` / `query` / `render` / `analyze` | 可独立完成创作，无需中间 SMF | trance 工程 9 轨 / 602 音符 |
| `bind-preset` | 选音色快，鼓噪声预设可用 | hat / open-hat 可直接上曲 |
| `set-instrument` + 本地 patch | 可在不改仓库预设的前提下做曲风音色 | `patches/*-trance.json` |
| gain/pan 的 apply diff | 混音修改可审阅 | `tone-v2` 前后 before/after 清晰 |
| 成品裁片 + dry 重混音 | 改混音几乎不痛 | 4 小节切片 ~6 ms；gain/pan 暖重混 ~56–237 ms；9 轨冷渲 ~12 s |
| summary / tracks 查询 | 体量可控，适合循环确认 | summary 只含计数与范围 |

这些是 Agent 愿意反复迭代的原因；回归测试应锁住「暖重混 ≫ 冷渲」和「summary 不随音符数爆炸」。

## 3. 高优先级摩擦与建议

### P0 — 工程资源解析（当天可修）

**现象：** patch 路径相对 **song 文件目录** 解析；`set-instrument` 哈希却相对 **进程 CWD**。库存预设需在工程旁手工 junction `presets/`，否则 `resource-hash` 失败。自定义 patch 必须从工程目录 apply/render，否则路径/哈希错位。

**建议：**

1. 统一规则并写进 CLI 帮助：`资源路径相对 song 文件；apply 时用同一 base 计算 hash`。
2. `bind-preset` 默认把预设**复制或链接进工程**（`songDir/presets/<id>-vN.json`），歌曲自包含可搬移。
3. `set-instrument` 接受相对 song 的路径；若传入相对 CWD 的路径，先规范化再入库。
4. `nod song validate` / `render` 在失败时打印：期望路径、实际路径、期望 hash、实际 hash（本次只有 `presets/kick.json` 级别提示，排查成本高）。

**验收：** 从仓库根目录对 `compositions/foo/song.json` 执行 create→bind-preset→render 一次成功；拷贝整个 `foo/` 目录到别处仍能 render。

### P0 — 大批量 apply 的 diff 可信度

**现象：** 一次 `arrange` 写入数百音符、多轨、预设绑定后，diff 往往只亮出「新增 track」，`changedEntityCount` 远小于真实语义变化。Agent 只能再 `query` 才能确认落盘。

**建议：**

1. diff 至少汇总：`tracks+/clips+/notes+ / instruments changed / automation lanes` 计数。
2. 默认不dump全部音符；提供 `diff.notesSummary`（每轨新增数、tick 范围）与可选 `--diff-notes`。
3. `invalidateRange` 与真实音符范围一致（本次可用，请保持）。

**验收：** 对 500+ 音符的 arrange，不看 query 也能从 apply JSON 判断「哪些轨有音符、大约多少」。

### P1 — 编排要有「乐句层」而不是只有 note 数组

**现象：** trance 的 4-on-floor、offbeat bass、2&4 clap、和弦 pad、侧链泵，全部靠外部脚本生成 tick。公开命令允许这么做，但心智模型仍是 MIDI 表格，不是编曲。

**建议（按收益顺序）：**

1. **Pattern / groove 原语**（可先做非破坏糖）：  
   `add-pattern` 支持 `every`（1/4、1/8、1/16）、`offset`、`lengthBars`、`pitches`/`degree`、`velocity` 曲线。底层仍落成普通 notes。
2. **Clip 循环与 duplicate-clip 的 bar 语义**已有部分能力；补 `duplicate-clip --to-bars`、`transpose-notes`、`scale-velocities`。
3. **和弦辅助**：`add-chord`（根音 + 性质 + 时值 + 转位），避免手写四音。
4. 文档明确：允许 Agent 用脚本生成 **commands JSON**；不要鼓励写 SMF 或私有 API。

**验收：** 不写逐 tick 列表，也能用公开命令表达「17 小节 4-on-floor + offbeat bass + Am–F–C–G pad」。

### P1 — 试听交接（人听得见、状态可标）

**现象：** 默认输出 float32 WAV，Windows 常见播放器打不开；`auditionStatus` 长期 `unheard`；Agent 只能报峰值/LUFS，不能声称听感合格。

**建议：**

1. `nod render --format float32|pcm16`（默认 pcm16 或同时写 `.preview.wav`）。
2. `nod play <wav|song> [--bars N:N]` 或文档化「打开系统播放器」的稳定钩子；成功播放后可 `nod audition mark --heard --render-id …`。
3. 报告区分：`metrics`（机器）/ `auditionStatus`（人）/ `agentHypothesis`（禁止把峰值当掩蔽结论——已有文案，请在 CLI 示例里重复）。

**验收：** 一次 render 后，默认产物可被本机双击播放；标记 heard 后 query/report 可见。

### P1 — 音色：曲风预设 + 真正可自动化

**现象：** 七个起步预设能搭骨架；要 uplifting trance 必须手写节点图。`set-parameter-automation` 对 NodSynth 轨在渲染中基本不生效（仅 VST3 路径使用），无法做 cutoff 扫频等电子舞曲核心动作。

**建议：**

1. **曲风/角色预设包**：`trance.lead-supersaw`、`trance.bass-offbeat`、`trance.pad-bloom`、`trance.kick` 等；带 tags 与短 audition。
2. **宏参数**（cutoff、delay mix、env amount）映射到稳定 parameter id，并在 `preset inspect` 列出。
3. **NodSynth 渲染读取 track parameter automation**（至少 cutoff / level / delay mix）；与 VST3 共用同一 lane 语义。
4. DSP 缺口（按电子舞曲优先级）：chorus/unison 或 detune 组、简易算法混响、侧链压缩（或保留 gain 自动化但提供 `add-sidechain-pump` 糖）。

**验收：** 只 `bind-preset`、不手写 patch JSON，能做出可辨认的 trance drop；一条 cutoff 自动化从闭到开可在 WAV 频谱/听感上确认。

## 4. 中优先级（体验抛光）

| 问题 | 建议 |
|---|---|
| 冷渲随轨数上升（6 轨 ~6 s → 9 轨 ~12 s） | 保持 dry 缓存；考虑按轨并行（仅确定性后端）；报告每轨 dspMs |
| 首包 diff 与 query 双步才能放心 | apply 返回可选 `snapshot.summary`（轨列表 + 音符计数） |
| 自定义 patch 与仓库 presets 两套入口 | `nod preset import patches/foo.json --id trance.lead` 纳入 registry，或 `bind-preset` 支持 `--file` |
| 工程目录样板缺失 | `nod song init my-track --template trance-drop` 生成目录、junction/复制 presets、示例 arrange |
| Agent 仍需本地 Python 生成 commands | 官方提供无依赖的 `nod song gen-pattern` 或示例 schema，减少私有脚本分叉 |
| 响度偏保守（本曲约 −18 LUFS） | 可选 `render --normalize loudness=-14`（显式、可关）；不要默契抬增益导致削波 |

## 5. 不建议现在做的事

- 为“更好听”先上商店、新插件格式、完整混音器 UI 重做。
- 用 LUFS/峰值自动打分替代人工试听。
- 在未修资源路径前推广「任意目录渲染」。
- 宣称参数自动化已支持 NodSynth（当前易误导 Agent 写无效 lane）。

## 6. 建议的下一迭代验收剧本

固定任务（可与现有 twinkle 验收并行）：

1. 从仓库根目录：`create` 138 BPM × 16–17 小节 × ≥7 轨 trance drop。
2. 仅用 registry 预设（含新 trance 包），禁止手写节点图。
3. 一次 arrange apply 的 diff 能汇总音符规模；`query summary` < 8 KiB。
4. 冷全曲有基线；改 lead gain + pad pan 后暖试听 ≤2 s 且 ≥5×。
5. 默认试听 WAV 可播放；人工听后标记 heard。
6. 输出：mix、可选 stems、render 报告、简短 NOTES（含未解决听感问题）。

未通过则优先修 P0/P1，不开启新后端。

## 7. 对文档/路线图的落点

- 本文可作为 [ITERATION-COMPOSING.md](ITERATION-COMPOSING.md) 的**复测附录**；A/B/C 已完成不等于曲风创作体验完成。
- [ROADMAP.md](ROADMAP.md)「下一项」建议改为：资源路径自包含 + apply diff 摘要 + 试听格式/状态 + trance 预设包与 NodSynth 参数自动化（见上文 P0/P1）。
- 保留 `compositions/trance-drop/` 作回归素材（含 `NOTES-HQ.md` 与 `mix-hq-pcm16.wav`），不要在未记录听感的情况下当“音质达标”。

---

**一句话给开发者：** 工作台已经能编；下一步让工程可搬、diff 可信、乐句好写、预设能跳舞、渲染能听——按这个顺序做，Agent 编曲体感会明显上台阶。
