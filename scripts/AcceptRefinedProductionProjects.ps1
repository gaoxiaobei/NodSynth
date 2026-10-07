param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$ProjectsDirectory = "$PSScriptRoot/../build/acceptance-production/projects",
    [string]$IterationDirectory = "$PSScriptRoot/../build/acceptance-production/sound-iteration-2",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production/refined-projects"
)
$ErrorActionPreference='Stop'
$Executable=[IO.Path]::GetFullPath($Executable);$ProjectsDirectory=[IO.Path]::GetFullPath($ProjectsDirectory)
$IterationDirectory=[IO.Path]::GetFullPath($IterationDirectory);$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {throw 'Refined output directory already exists; use a new directory to preserve versions'}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$worker=Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'nod_vst3_worker.exe'
$measurements=[Collections.Generic.List[object]]::new();$results=[Collections.Generic.List[object]]::new()
function Write-Json($Path,$Value) {[IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 90),[Text.UTF8Encoding]::new($false))}
function Invoke-Nod([string[]]$Arguments) {
    $info=[Diagnostics.ProcessStartInfo]::new($Executable);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach ($argument in $Arguments) {$info.ArgumentList.Add($argument)}
    $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($info)
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();[long]$peak=0
    while (!$process.WaitForExit(20)) {
        if ($watch.Elapsed.TotalSeconds -gt 600) {$process.Kill($true);throw 'refined project command timed out'}
        try {$process.Refresh();$peak=[Math]::Max($peak,$process.PeakWorkingSet64)} catch [InvalidOperationException] {}
    }
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();$watch.Stop()
    if ($process.ExitCode) {throw "nod failed: $text $errors"}
    $measurements.Add(@{arguments=$Arguments;elapsedMs=$watch.Elapsed.TotalMilliseconds;cpuMs=$process.TotalProcessorTime.TotalMilliseconds;
        parentPeakWorkingSetBytes=$peak;workerMemory='per-effect private bytes in render report; parent measurement excludes plugin instrument process'})
    $process.Dispose();return ($text | ConvertFrom-Json -AsHashtable)
}
function Same-Audio($First,$Second) {
    if ((Get-FileHash -LiteralPath $First -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $Second -Algorithm SHA256).Hash) {throw 'Refined reconstruction audio differs'}
}
function Apply($Source,$Commands,$Output,$BatchPath) {
    Write-Json $BatchPath @{schemaVersion=1;commands=@($Commands)}
    Invoke-Nod @('song','apply',$Source,'--commands',$BatchPath,'--output',$Output,'--json') | Out-Null
}
foreach ($style in @('trance','house')) {foreach ($bars in @(16,40)) {foreach ($variant in @('native','external')) {
    $id="$style-$bars-$variant";$directory=Join-Path $OutputDirectory $id
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $source=Join-Path $ProjectsDirectory "$id/song.json";$sourceSha=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $recipe=Get-Content (Join-Path $IterationDirectory "$variant-combined/commands.json") -Raw | ConvertFrom-Json -AsHashtable
    $commands=@($recipe.commands | Where-Object {$_.op -ne 'collect-resources'})
    if ($variant -eq 'native') {
        $balance=Get-Content (Join-Path $IterationDirectory 'native-balanced/commands.json') -Raw | ConvertFrom-Json -AsHashtable
        $commands+=@($balance.commands | Where-Object {$_.op -ne 'collect-resources'})
    }
    $commands+=@(@{op='collect-resources';includeAssets=$true})
    $song=Join-Path $directory 'song.json';Apply $source $commands $song (Join-Path $directory 'refine.commands.json')
    $before=Get-Content -LiteralPath $source -Raw | ConvertFrom-Json -AsHashtable
    $after=Get-Content -LiteralPath $song -Raw | ConvertFrom-Json -AsHashtable
    if (($before.tempo | ConvertTo-Json -Depth 10 -Compress) -ne ($after.tempo | ConvertTo-Json -Depth 10 -Compress)) {throw 'Refinement changed tempo'}
    foreach ($track in $after.tracks) {
        $prior=@($before.tracks | Where-Object {$_.id -eq $track.id})[0]
        if (($prior.clips | ConvertTo-Json -Depth 30 -Compress) -ne ($track.clips | ConvertTo-Json -Depth 30 -Compress)) {throw 'Refinement changed arrangement'}
    }
    $tools=@();if ($variant -eq 'external') {$tools=@('--vst3-worker',$worker,'--freeze-external')}
    Invoke-Nod @('song','validate',$song,'--render-ready','--json') | Out-Null
    Invoke-Nod @('song','export-midi',$song,'--output',(Join-Path $directory 'events.mid'),'--json') | Out-Null
    $cache=Join-Path $directory 'cache';$master=Join-Path $directory 'master.wav';$warmFile=Join-Path $directory 'warm.wav'
    $cold=Invoke-Nod (@('render',$song,'--output',$master,'--stems',(Join-Path $directory 'stems'),'--report',(Join-Path $directory 'render.json'),
        '--cache-dir',$cache,'--tail-seconds','8','--json')+$tools)
    if ($cold.tailTruncated) {throw "$id truncates effect tail"}
    $warm=Invoke-Nod (@('render',$song,'--output',$warmFile,'--cache-dir',$cache,'--tail-seconds','8','--json')+$tools)
    Same-Audio $master $warmFile
    $analysis=Invoke-Nod @('analyze',$master,'--json');Write-Json (Join-Path $directory 'analysis.json') $analysis
    if ($analysis.truePeakDbtp -gt -0.99) {throw 'Refined master exceeds ceiling'}
    $unmasteredSong=Join-Path $directory 'unmastered.song.json'
    Apply $song @(@{op='set-inserts';target='master';inserts=@()}) $unmasteredSong (Join-Path $directory 'unmastered.commands.json')
    $unmastered=Invoke-Nod (@('render',$unmasteredSong,'--output',(Join-Path $directory 'unmastered.wav'),'--cache-dir',$cache,'--tail-seconds','8','--json')+$tools)
    if ($unmastered.tailTruncated) {throw 'Refined unmastered tail truncated'}
    $moved=Join-Path $directory 'relocated';[IO.Directory]::CreateDirectory($moved) | Out-Null
    $movedSong=Join-Path $moved 'song.json'
    Apply $song @(@{op='collect-resources';includeAssets=$true}) $movedSong (Join-Path $moved 'collect.commands.json')
    $fragment=Join-Path $directory 'fragment.wav';$relocated=Join-Path $moved 'fragment.wav'
    Invoke-Nod (@('render',$song,'--output',$fragment,'--bars','1:3','--tail-seconds','8','--no-cache','--json')+$tools) | Out-Null
    Invoke-Nod (@('render',$movedSong,'--output',$relocated,'--bars','1:3','--tail-seconds','8','--no-cache','--json')+$tools) | Out-Null
    Same-Audio $fragment $relocated
    $matrix=[Collections.Generic.List[object]]::new()
    if ($variant -eq 'native' -and $bars -eq 16) {
        foreach ($rate in @(44100,48000,96000)) {
            $referenceHash=$null
            foreach ($block in @(64,128,512)) {
                $file=Join-Path $directory "matrix-$rate-$block.wav"
                $render=Invoke-Nod @('render',$song,'--output',$file,'--bars','1:3','--tail-seconds','8','--sample-rate',"$rate",'--block-size',"$block",'--no-cache','--json')
                if ($render.tailTruncated) {throw 'Refined matrix tail truncated'}
                $hash=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
                if ($referenceHash -and $referenceHash -ne $hash) {throw 'Refined native output depends on block size'};$referenceHash=$hash
                $matrix.Add(@{sampleRate=$rate;blockSize=$block;sha256=$hash;render=$render})
            }
        }
    }
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $sourceSha) {throw 'Source project was modified'}
    $results.Add(@{id=$id;style=$style;bars=$bars;variant=$variant;sourceSong=$source;sourceSha256=$sourceSha;render=$cold;warm=$warm;analysis=$analysis;
        unmasteredRender=$unmastered;matrix=$matrix.ToArray();durationSeconds=$cold.frames/$cold.sampleRate;masterSha256=(Get-FileHash -LiteralPath $master -Algorithm SHA256).Hash;
        technicalChecks=@('notes and tempo unchanged','cold/warm exact','public save relocation exact','master ceiling','full declared tail','dry/post/return stems');auditionStatus='unheard'})
    Write-Json (Join-Path $OutputDirectory 'manifest.json') @{schemaVersion=1;results=$results.ToArray();status='technical candidates; listening pending';
        sourceIteration=$IterationDirectory;gainCalibrationScope='Native lead trim calibrated on the trance drop; reused explicitly across style candidates, not a per-style listening result';
        referenceDawStatus='skipped at user request';hardware=$env:PROCESSOR_IDENTIFIER;executableSha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash}
    Write-Json (Join-Path $OutputDirectory 'measurements.json') @{schemaVersion=1;memorySampleIntervalMs=20;measurements=$measurements.ToArray()}
    Write-Output "Verified refined project: $id"
}}}
