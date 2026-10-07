# 套件使用手册

[返回首页](../README.md) · [命令参考](COMMAND-REFERENCE.md) · [声音与混音](SOUND-GUIDE.md)

## 准备与文件概念

先按[开发说明](DEVELOPMENT.md)构建。以下示例在仓库根目录的 PowerShell 执行；把 `build/release` 加入 PATH 后即可使用后文的 `nod` 简写。离线编曲、渲染与分析不需要音频设备。

| 文件 | 用途 |
|---|---|
| Patch JSON | 单个 NodSynth 音色的节点图、连接、基础参数和宏 |
| Song JSON | 歌曲的轨道、片段、音符、资源、自动化、路由、章节和编辑历史 |
| commands JSON | 一次事务的编辑请求；交给 `song apply`，不是 Song 本身 |
| render report JSON | 本次渲染的配置、依赖、缓存与音频诊断 |
| WAV / stems | 母带、试听文件与分轨；不能替代可编辑 Song |

当前 Song v7 兼容读取 v1–v6。轨道、片段、音符使用稳定 ID；轨道名字和显示顺序不是 ID。小节从 1 开始，tick 从 0 开始；区间末端不包含在内。480 PPQ、4/4 时一小节为 1920 ticks，`--bars 1:5` 是前四小节。节奏字符串 `1/4` 表示四分音符，不是四分之一拍。

## 第一首可渲染的歌曲

使用新目录，避免混入旧工程和缓存：

```powershell
New-Item -ItemType Directory -Path build/my-first-song
./build/release/nod.exe song create build/my-first-song/song.json --bpm 128 --meter 4/4 --bars 4 --ppq 480 --json
./build/release/nod.exe song apply build/my-first-song/song.json --commands docs/examples/quickstart.json --expect-revision 1 --json
./build/release/nod.exe song query build/my-first-song/song.json --view tracks --json
./build/release/nod.exe song validate build/my-first-song/song.json --render-ready --json
./build/release/nod.exe render build/my-first-song/song.json --output build/my-first-song/mix.wav --preview-output build/my-first-song/mix.preview.wav --report build/my-first-song/render.json --stems build/my-first-song/stems --tail-seconds 3 --json
./build/release/nod.exe analyze build/my-first-song/mix.wav --json
./build/release/nod.exe play build/my-first-song/mix.preview.wav --report build/my-first-song/render.json --json
```

[quickstart.json](examples/quickstart.json) 是完整批次：创建 kick、bass、lead，绑定内置音色，写入四小节乐句并建立 `loop` 章节。它演示工作流，不是成品质感模板。`play` 调用系统播放器；`playbackStarted` 只代表启动成功，不代表有人听过。

## 修改、预检和撤销

把下面内容保存为工程目录中的 `mix-edit.json`：

```json
{
  "schemaVersion": 1,
  "requestId": "mix-edit-001",
  "commands": [
    {"op":"rename-track","track":"lead","name":"Main Lead"},
    {"op":"set-gain","track":"lead","gain":0.25},
    {"op":"set-pan","track":"lead","pan":0.1}
  ]
}
```

```powershell
nod song query build/my-first-song/song.json --view summary --json
nod song apply build/my-first-song/song.json --commands build/my-first-song/mix-edit.json --dry-run --json
nod song apply build/my-first-song/song.json --commands build/my-first-song/mix-edit.json --expect-revision 2 --json
nod song undo build/my-first-song/song.json --json
nod song redo build/my-first-song/song.json --json
```

这里 revision 2 仅适用于刚完成入门批次的工程；日常操作使用刚查询到的修订号。批次全成功才提交；`--dry-run` 不写歌曲或收集资产。相同 requestId 和相同内容重放不会重复编辑；修改内容时换新 requestId。空操作不产生编辑历史。`--output next.json` 可另存一个版本，保留原工程。

默认 diff 是摘要，音量、声像、自动化等也有变化记录。要审阅音符用 `--diff-detail notes`，要全部信息用 `full`。唯一实体数和变化记录数不是同一个指标。

## 编写与扩展乐句

操作 JSON 放入批次的 `commands` 数组。`add-pattern` 生成普通片段和音符：

```json
{
  "schemaVersion":1,
  "requestId":"extend-001",
  "commands":[
    {"op":"set-song-range","bars":8},
    {"op":"duplicate-clip","track":"lead","clip":"lead-phrase","id":"lead-copy","startBar":5},
    {"op":"transpose-notes","track":"lead","clip":"lead-copy","semitones":12},
    {"op":"scale-velocities","track":"lead","clip":"lead-copy","factor":0.85},
    {"op":"set-section","id":"variation","name":"Variation","startTick":7680,"endTick":15360}
  ]
}
```

先扩大范围再追加内容。`pitches` 序列循环；`null` 或空数组表示休止，音高数组表示和弦。也可写 `{"root":57,"quality":"minor","inversion":1,"voicing":[0,0,1]}`，quality 支持 major/minor。grid、duration、offset 各自独立，PPQ 必须精确表示分数。末尾越界默认拒绝，`truncate:true` 明确截断。移调和力度变换按起音筛选，越出 MIDI 范围默认拒绝，可用 `clamp:true` 限制。

`repeat-phrase` 可按章节或 tick 区间复制多个轨道的音符。它不会复制效果、自动化、pump、mute、速度或演奏控制事件；目标已有音符时叠加而非替换。完整字段见[编辑操作表](COMMAND-REFERENCE.md#编辑操作)。

## 从 MIDI 开始

```powershell
nod song import-midi input.mid --output imported.json --json
nod song query imported.json --view tracks --json
nod song query imported.json --view notes --track TRACK_ID --limit 32 --json
```

然后用 `bind-preset`、`set-instrument`、`set-sampler` 或 `set-vst3-instrument` 分配每轨音源，再执行 render-ready 校验。导入不等于自动完成 GM 配器。

也可在导入时使用 `--map 3:0=C:/sounds/lead.json`，键是**源 MIDI 轨号:0–15 声道**，值是 NodSynth Patch。建议传绝对路径；不要把导入映射的相对路径和 Song 资源路径混为一谈。歌曲轨序与源 MIDI 轨序不保证一致，编辑前查询真实 ID。SoundFont 不能当 Patch 传给 `--map`。

```powershell
nod song export-midi imported.json --output edited.mid --json
```

导出的 MIDI 用于交换音乐事件，不携带本套件的音源、混音路由、音频效果或可听到的最终声音；保存 Song 和音频才是完整交付。

## 音源与混音的推荐顺序

1. 用 `preset search` / `preset audition` 选声音，绑定后查询 capabilities。
2. 编排音符，设置基础音量；立体声音源按需要选择 balance 声像模式。
3. 添加音色参数 lane、独立 pump 与过门静音。
4. 设置 insert、总线、空间 send/return 和音频侧链。
5. 检查 render-ready，渲染片段，再导出全曲与分轨。

具体 JSON、采样鼓、参数组合、插件配置见[声音与混音](SOUND-GUIDE.md)。新增节点并不自动让编曲达到商业水准；需要对照音色、编排、动态和空间逐项判断。

## 局部试听、缓存与版本比较

```powershell
nod render build/my-first-song/song.json --section loop --output build/my-first-song/loop.wav --tail-seconds 3 --json
nod render build/my-first-song/song.json --bars 2:4 --output build/my-first-song/middle.wav --json
nod compare before.wav after.wav --match-loudness --json
nod version audition before.json after.json --section loop --output new-ab --tail-seconds 8 --json
```

章节和小节/tick 区间不要混用。效果器、包络和外部音源可能需要从前方预跑恢复状态；局部渲染不保证按长度等比例加速。默认缓存位于输出目录的 `.nod-cache`；只改 gain/pan 可重混，音符/音色参数改变会使相应声音缓存失效。外部音源缓存冻结需显式 `--freeze-external`；用 `--no-cache` 检查真实重渲结果。

`compare` 只输出分析，不生成新的匹配音频。`version audition` 会渲染并生成匹配响度的 A/B WAV 和 manifest；目录必须不存在且父目录存在。两版本分别使用自己的速度图和资源；section 必须都存在。它不随机化盲听。无法测量的短音频、静音或被截断的尾音会拒绝，应该修正范围/尾音再比较。

## 导出与报告

```powershell
nod render song.json --output delivery.wav --format pcm24 --dither tpdf --dither-seed 42 --pcm-overflow reject --preview-output delivery.preview.wav --stems stems --report delivery.render.json --tail-seconds 8 --json
nod analyze delivery.wav --json
```

母带和缓存默认 float32。PCM16/24 导出保留 `.float.wav` 母带；试听副本是 PCM16。整数导出显式选择抖动和峰值溢出策略：`reject` 拒绝，`attenuate` 整体降增益，不隐式削波。内部 float 可以超过 0 dBFS，应检查 true peak 并按交付要求处理。

报告和分析用于检查峰值、true peak、LUFS、LRA、非有限值、尾音与资源状态；积分响度至少需 400 ms，短时最大响度需 3 s，LRA 需 6 s，不足时返回 null。静音标记是阈值诊断，不能单独判定编曲错误。分轨有不同处理阶段和 return；有非线性总线/母带处理时，不能假设全部文件简单相加就等于混音。

当前 CLI 使用固定 `--tail-seconds`。虽然接受 `--tail-threshold` 和 `--max-tail-seconds`，它们不会自行切换到自动尾音模式。发现 `tailTruncated` 时增加固定尾音，尤其是 delay/reverb。

确实听过后才记录：

```powershell
nod audition record delivery.preview.wav --report delivery.render.json --heard --reviewer "listener" --records listening.json --json
nod audition query delivery.preview.wav --records listening.json --json
```

记录绑定音频哈希；新渲染的文件不继承旧文件的听感结论。heard 是听过，不是批准发行。

## 资源、搬移和备份

资源相对路径以 Song 文件所在目录为基准。`set-instrument` 可用 `pathBase:"cwd"` 显式选择当前目录。`bind-preset` 默认把固定版本复制到 `assets/presets/`，`externalReference:true` 才保留外部引用。

`collect-resources` 收集 Patch/预设；`includeAssets:true` 同时收集 SoundFont/采样。复制 Song 和 assets 一起搬移；公开 apply 的 `--output` 另存会处理已收集资源和历史引用。插件二进制不会随之打包，接收机器仍需相应安装。采样应保留 license/source 信息。`validate` 区分 missing、unreadable、hash-mismatch，先修复来源再渲染。

## 图形界面入口

`nodsynth_app.exe` 打开独立合成器，可从菜单加载示例 Patch、打开/保存音色，在节点画布编辑并使用屏幕键盘或 MIDI 输入。它编辑的是 Patch，不是 Song。`nod_daw.exe` 的 Open song 打开 Song，显示基础时间线、钢琴卷帘和混音，空格控制播放。复杂音源/路由的制作与能力查询以 CLI 为主，不假设 GUI 覆盖全部命令。

合成器画布中从输出端拖到输入端连线，右键点连线断开，拖动节点和参数滑块调整音色；Delete 删除选中节点/连接，Ctrl+Z / Ctrl+Y 撤销/重做，Ctrl+S 保存。无效连接会给出诊断，结构编译失败不会替换当前可运行的声音图。

基础 DAW 可拖动音符/推子/声像，通过底部文本区提交完整 commands 批次，并从菜单撤销。当前没有 Song 保存菜单，界面修改留在内存；需要可持久化制作时，通过 CLI apply 保存同样的批次。

构建的 NodSynth VST3 可由其他 DAW 扫描为乐器；这与本套件通过 worker 宿主第三方插件是两个独立入口。

## 常见问题

| 现象 | 检查与处理 |
|---|---|
| query 太大 | 指定 `--view tracks` / `summary`；音符查询按轨并限制条数 |
| revision/request 冲突 | 重新 query，审阅最新状态；新意图用新 requestId，不盲目重试覆盖 |
| 自动化地址无效 | 查 capabilities 的原因、单位、连接和后端支持；不要猜节点名 |
| pump 掩盖过门 | 使用独立 `set-audio-mute`，或 pump 的 skipBars/skipIntervals |
| 有 MIDI 却没有声音 | 检查绑定、资源、song range、mute、输出路由与实际音符范围 |
| 外部插件失败 | 检查模块路径、位数、class/state、worker 和进程错误，见声音手册 |
| 局部试听慢 | 看缓存命中和预跑范围；不要把预跑误认为无效渲染 |
| 数值良好但声音简陋 | 比较匹配响度的参考与分轨；数值不能诊断旋律、配器与听感品质 |
