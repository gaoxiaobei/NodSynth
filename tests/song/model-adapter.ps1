param(
    [Parameter(Mandatory = $true)][string]$QueryPath,
    [Parameter(Mandatory = $true)][string]$InstructionPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$instruction = Get-Content -Raw -Encoding utf8 $InstructionPath
if ([string]::IsNullOrWhiteSpace($instruction)) { exit 2 }
$query = Get-Content -Raw -Encoding utf8 $QueryPath | ConvertFrom-Json
$utf8 = New-Object System.Text.UTF8Encoding $false
if ($instruction -match "harmony") {
    $ppq = 480
    if ($query.ppq) { $ppq = [int]$query.ppq }
    $bar = 4 * $ppq
    $from = 4 * $bar
    $lines = @()
    foreach ($note in @($query.notes)) {
        if ($note.track -ne "bass") { continue }
        $tick = [int]$note.tick
        if ($tick -lt $from) { continue }
        $index = [int]($tick / $bar)
        $lines += "{`"op`":`"delete-note`",`"track`":`"bass`",`"note`":`"$($note.id)`"}"
        $lines += "{`"op`":`"add-note`",`"track`":`"bass`",`"clip`":`"bass-clip`",`"id`":`"bass-new-$index`",`"tick`":$tick,`"duration`":$ppq,`"pitch`":43,`"velocity`":90,`"channel`":0}"
        $lines += "{`"op`":`"add-note`",`"track`":`"harmony`",`"clip`":`"harmony-clip`",`"id`":`"harmony-$index`",`"tick`":$tick,`"duration`":$($ppq * 2),`"pitch`":67,`"velocity`":80,`"channel`":0}"
    }
    $joined = $lines -join ","
    [System.IO.File]::WriteAllText($OutputPath, "{`"schemaVersion`":1,`"commands`":[$joined]}", $utf8)
    exit 0
}
$notes = @($query.notes)
if ($notes.Count -lt 1) {
    [System.IO.File]::WriteAllText($OutputPath, '{"schemaVersion":1,"commands":[]}')
    exit 0
}
$note = $notes[0]
$pitch = [Math]::Min(127, [int]$note.pitch + 2)
$json = @"
{"schemaVersion":1,"commands":[{"op":"move-note","track":"$($note.track)","note":"$($note.id)","tick":$([int]$note.tick),"pitch":$pitch}]}
"@
[System.IO.File]::WriteAllText($OutputPath, $json, $utf8)
exit 0
