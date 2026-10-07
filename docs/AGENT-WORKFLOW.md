# Agent 工作流与适配协议

[首页](../README.md) · [操作参考](COMMAND-REFERENCE.md) · [声音能力](SOUND-GUIDE.md)

## 默认工作流

1. query summary/workflow 获取修订、范围、轨道 ID 和角色；只在需要时查 notes，并分页。
2. 查询实际 capabilities，选择已有声音或搜索/试听 preset；先确认参数单位和后端支持。
3. 形成单一意图的命令批次，提供唯一 requestId 和当前 baseRevision。
4. dry-run 审阅差异，处理阻塞；正式 apply 后检查返回值与新修订。
5. 渲染指定章节/小节，检查报告，必要时生成匹配响度的版本试听。
6. 保存工程、音频、资源与报告；没有真实聆听时保持 unheard。

```powershell
nod song query song.json --view workflow --json
nod song query song.json --view roles --role lead --json
nod song query song.json --view capabilities --track lead --json
nod song query song.json --view notes --track lead --bars 1:5 --limit 32 --json
nod preset search "production" --role lead --json
```

角色优先使用显式 set-role，其次是绑定预设的角色，不从名字猜。筛选可能返回多轨，必须明确选择稳定 ID，不能修改“第一条匹配”。未知角色不凭空补齐。

workflow/roles 可返回 startingChainProposal。目前是保守高通起点，不自动添加母带响度、限幅或空间。未知角色可返回 null。应用 set-inserts 会替换原链，应合并仍需保留的效果，再审阅差异。

preset index 生成独立试听目录，列出逐项成功/失败、音频路径与哈希。新音频都是 unheard；它是检索索引，不是匹配响度的音质排名。

## 模型提案

```powershell
nod song propose song.json --adapter model-adapter.exe --instruction "保留旋律，减少 pad 对 lead 的掩蔽" --output proposal.json --json
nod song apply song.json --commands proposal.json --dry-run --json
nod song apply song.json --commands proposal.json --output next-version.json --json
```

模型由外部适配器提供，套件不内置模型服务、网络账户或密钥。提案不直接提交；正式应用时 baseRevision 防止覆盖提案产生后的编辑。

### 适配器调用约定

实际进程参数顺序：

```text
ADAPTER [每个 --adapter-arg 的值] QUERY_JSON_PATH INSTRUCTION_TEXT_PATH OUTPUT_BATCH_PATH
```

适配器应读取前两个文件，把 schemaVersion 1 的完整命令批次 JSON 写到第三个路径，并以 0 退出。stdout 不是命令文件。默认超时 30 秒；缺少程序、超时、非零退出、缺少输出或非法 JSON 都失败。每个调用使用独立临时目录，请求结束清理，不应把临时路径存入长期资源。

QUERY_JSON 是兼容完整 Song 查询，额外包括 capabilities、workflow、presets、proposalPolicy。适配器应以这些实际信息为依据，不猜命令、节点 ID 或参数值域。返回格式：

```json
{
  "schemaVersion":1,
  "requestId":"model-balance-001",
  "commands":[{"op":"set-gain","track":"lead","gain":0.3}]
}
```

示例依赖确有 lead 轨。套件在副本上预检，失败提案不会导出为可应用结果；成功结果包含差异和 baseRevision。暂存资源用于验证，预检不会执行资源收集写入。

变更音源或参数后，原生参数检查使用实际 Patch 和自动化内核，按 48000 Hz 检查地址、范围、内部覆盖和重叠映射。不支持 lane 的后端拒绝。VST3 动态参数返回 pendingChecks，需要实际 worker 检查；不能将静态预检当作全部插件、采样率、图编译和听感均通过。

## 章节与版本

用 set-section 定义半开区间，渲染用 `--section ID`。repeat-phrase 可复制章节内指定轨或所有轨的音符，目标为叠加，自动化与演奏控制不随之复制；超出范围时显式选择 extendSongRange。

```powershell
nod render song.json --section drop --output drop.wav --tail-seconds 8 --json
nod version audition before.json after.json --section drop --output new-ab --tail-seconds 8 --json
```

A/B 输出目录必须是新目录。每份 Song 使用自身资源目录、tempo/meter；共同章节应在两份工程中存在。输出原始 float 母带、报告、恒定增益匹配的 A.wav/B.wav、差异与 manifest。目标 -20 LUFS，必要时降低以保留 -1 dBTP 余量；匹配误差不超过 0.05 LU。静音、过短无法测量、截断尾音、渲染中来源变更会拒绝。A/B 标签不构成随机盲听。

峰值与 LUFS 能判断电平问题，不能判断旋律、音色与制作审美。playbackStarted、heard、approved 是不同结论，不得从一个推断另一个。

## 操作成本记录

```powershell
nod workflow metrics trace.json --json
```

trace 由调用者记录，不自动监控用户活动。最小示例：

```json
{
  "actor":"agent",
  "resourceSnapshotHash":"actual-fixed-resource-hash",
  "events":[
    {"kind":"tool-call","tool":"nod","arguments":["song","query","song.json","--view","summary","--json"],"exitCode":0},
    {"kind":"command-batch","intentId":"balance-lead","outcome":"ok","commands":[{"op":"set-gain","track":"lead","gain":0.3}]}
  ]
}
```

事件种类：tool-call（tool、arguments、整数 exitCode）、command-batch（commands、intentId、outcome:ok/rejected；失败带 code）、source-lookup、human-intervention。同 intentId 的后续批次计作重试。工具调用次数与编辑操作数量分开统计。

参数阻塞类失败中的 lane 数量表示尝试数量，不证明其中每条 lane 都失败。未记录的活动不能解释为零成本；Agent 与人工比较应使用同任务、固定资源的真实 trace，自动测试夹具不代替人工结果。
