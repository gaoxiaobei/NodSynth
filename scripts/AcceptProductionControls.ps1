param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$SourceSong = "$PSScriptRoot/../build/acceptance-production/projects/trance-16-native/song.json",
    [string]$ExternalPlugin = "$PSScriptRoot/../build/release/VST3/Release/mda-vst3.vst3/Contents/x86_64-win/mda-vst3.vst3"
)
$ErrorActionPreference='Stop'
$Executable=[IO.Path]::GetFullPath($Executable);$SourceSong=[IO.Path]::GetFullPath($SourceSong)
$ExternalPlugin=[IO.Path]::GetFullPath($ExternalPlugin)
$directory=[IO.Path]::GetDirectoryName($SourceSong);$worker=Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'nod_vst3_worker.exe'
function Write-Json($Path,$Value) {[IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 80),[Text.UTF8Encoding]::new($false))}
function Invoke-Nod([string[]]$Arguments) {
    $info=[Diagnostics.ProcessStartInfo]::new($Executable);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach ($argument in $Arguments) {$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($info);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(120000)) {$process.Kill($true);throw 'controlled comparison timed out'}
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();if ($process.ExitCode) {throw "nod failed: $text $errors"}
    $process.Dispose();return ($text | ConvertFrom-Json -AsHashtable)
}
$baseline=[IO.File]::ReadAllText($SourceSong) | ConvertFrom-Json -AsHashtable
$lead=@($baseline.tracks | Where-Object {$_.id -eq 'lead'})[0]
$lead.parameterAutomation=@();$lead.parameterValues=@{}
$baseline.undoStack=@();$baseline.redoStack=@()
$results=[Collections.Generic.List[object]]::new()
foreach ($case in @('baseline','instrument-only','effect-only','reverb-only')) {
    $song=Join-Path $directory "control-$case.song.json";Write-Json $song $baseline
    $commands=[Collections.Generic.List[object]]::new()
    if ($case -ne 'baseline') {$commands.Add(@{op='add-plugin-resource';id='control-mda';path=$ExternalPlugin;license='MIT';source='Steinberg mda VST3 example'})}
    if ($case -eq 'instrument-only') {$commands.Add(@{op='set-vst3-instrument';track='lead';pluginResource='control-mda';className='mda JX10';clearAutomation=$true})}
    if ($case -eq 'effect-only') {$commands.Add(@{op='set-inserts';target='echo';inserts=@(@{id='spatial-reference';type='vst3';pluginResource='control-mda';className='mda Delay';declaredTailSeconds=4;
        parameters=@{'vst3:0'=0.18;'vst3:1'=0.4;'vst3:2'=0.12;'vst3:4'=1;'vst3:5'=0.5}})})}
    if ($case -eq 'reverb-only') {$commands.Add(@{op='set-inserts';target='room';inserts=@(@{id='reverb-reference';type='vst3';pluginResource='control-mda';className='mda Ambience';declaredTailSeconds=4})})}
    if ($commands.Count) {
        $batch=Join-Path $directory "control-$case.commands.json";Write-Json $batch @{schemaVersion=1;commands=$commands.ToArray()}
        Invoke-Nod @('song','apply',$song,'--commands',$batch,'--json') | Out-Null
    }
    $wav=Join-Path $directory "control-$case.wav"
    $report=Invoke-Nod @('render',$song,'--output',$wav,'--bars','1:3','--tail-seconds','8','--no-cache','--vst3-worker',$worker,'--json')
    if ($report.tailTruncated) {throw "$case truncates the declared effect tail"}
    $analysis=Invoke-Nod @('analyze',$wav,'--json')
    $results.Add(@{case=$case;song=$song;wav=$wav;render=$report;analysis=$analysis;auditionStatus='unheard';
        sha256=(Get-FileHash -LiteralPath $wav -Algorithm SHA256).Hash})
}
Write-Json (Join-Path $directory 'controlled-comparison.json') @{schemaVersion=1;results=$results.ToArray();
    fixed=@('notes','tempo','velocity','musical range','48000 Hz','128 frames','mix routing except selected insert','track faders','all other instrument patches');
    change=@{baseline='production-lead fixed patch base; no lead lane';'instrument-only'='lead replaced with default mda JX10; native effects retained';'effect-only'='native lead retained; echo insert replaced with mda Delay';'reverb-only'='native lead and delay retained; room insert replaced with default mda Ambience'};
    listeningStatus='pending user comparison';dawComparison='skipped at user request';scope='Numerical repeatability is checked elsewhere; these comparisons isolate instrument versus spatial-chain choice, not aesthetic quality'}
Write-Output 'Controlled instrument-only, delay-only and reverb-only comparisons prepared'
