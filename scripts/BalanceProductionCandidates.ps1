param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$ProjectsDirectory = "$PSScriptRoot/../build/acceptance-production/projects",
    [string]$IterationDirectory = "$PSScriptRoot/../build/acceptance-production/sound-iteration-2"
)
$ErrorActionPreference='Stop'
$Executable=[IO.Path]::GetFullPath($Executable);$ProjectsDirectory=[IO.Path]::GetFullPath($ProjectsDirectory)
$IterationDirectory=[IO.Path]::GetFullPath($IterationDirectory);$ffmpeg=(Get-Command ffmpeg -ErrorAction Stop).Source
function Write-Json($Path,$Value) {[IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 90),[Text.UTF8Encoding]::new($false))}
function Invoke-Tool($Tool,[string[]]$Arguments) {
    $info=[Diagnostics.ProcessStartInfo]::new($Tool);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach ($argument in $Arguments) {$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($info);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(600000)) {$process.Kill($true);throw 'gain-staging comparison timed out'}
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();if ($process.ExitCode) {throw "$Tool failed: $text $errors"}
    $process.Dispose();return $text
}
function Invoke-Nod([string[]]$Arguments) {return (Invoke-Tool $Executable $Arguments | ConvertFrom-Json -AsHashtable)}
function Number($Value) {([double]$Value).ToString('G17',[Globalization.CultureInfo]::InvariantCulture)}
$externalLead=Invoke-Nod @('analyze',(Join-Path $ProjectsDirectory 'trance-40-external/stems/dry/lead.wav'),'--json')
$cases=@(
    @{id='native-level-only';song=(Join-Path $ProjectsDirectory 'trance-40-native/song.json');lead=(Join-Path $ProjectsDirectory 'trance-40-native/stems/dry/lead.wav');factor='Only native lead fader adjusted to match the external lead solo loudness; downstream sends and master react naturally'},
    @{id='native-balanced';song=(Join-Path $IterationDirectory 'native-combined/song.json');lead=(Join-Path $IterationDirectory 'native-combined/stems/dry/lead.wav');factor='Combined candidate plus measured lead gain staging and explicit pad/pluck/bass balance; not a single-factor comparison'}
)
$results=[Collections.Generic.List[object]]::new()
foreach ($case in $cases) {
    $directory=Join-Path $IterationDirectory $case.id
    if (Test-Path -LiteralPath $directory) {throw 'Candidate directory already exists; preserve it and choose a new iteration directory'}
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $nativeLead=Invoke-Nod @('analyze',$case.lead,'--json')
    if ($null -eq $nativeLead.loudnessLufs -or $null -eq $externalLead.loudnessLufs) {throw 'Lead loudness cannot be calibrated'}
    $gainDb=$externalLead.loudnessLufs-$nativeLead.loudnessLufs
    $sourceSong=Get-Content -LiteralPath $case.song -Raw | ConvertFrom-Json -AsHashtable
    $sourceTrack=@($sourceSong.tracks | Where-Object {$_.id -eq 'lead'})[0]
    $fader=$sourceTrack.gain*[Math]::Pow(10,$gainDb/20)
    $commands=[Collections.Generic.List[object]]::new();$commands.Add(@{op='set-gain';track='lead';gain=$fader})
    if ($case.id -eq 'native-balanced') {
        foreach ($setting in @(@{track='pad';gain=1},@{track='pluck';gain=1.2},@{track='bass';gain=1})) {
            $commands.Add(@{op='set-gain';track=$setting.track;gain=$setting.gain})
        }
    }
    $commands.Add(@{op='collect-resources';includeAssets=$true})
    $batch=Join-Path $directory 'commands.json';Write-Json $batch @{schemaVersion=1;commands=$commands.ToArray()}
    $song=Join-Path $directory 'song.json';Invoke-Nod @('song','apply',$case.song,'--commands',$batch,'--output',$song,'--json') | Out-Null
    $raw=Join-Path $directory 'raw.wav'
    $render=Invoke-Nod @('render',$song,'--output',$raw,'--bars','17:33','--tail-seconds','8','--stems',(Join-Path $directory 'stems'),
        '--report',(Join-Path $directory 'render.json'),'--cache-dir',(Join-Path $ProjectsDirectory 'trance-40-native/cache'),'--json')
    if ($render.tailTruncated) {throw 'Gain-staged candidate truncates effect tail'}
    $excerpt=Join-Path $directory 'excerpt.wav'
    Invoke-Tool $ffmpeg @('-hide_banner','-loglevel','error','-i',$raw,'-t','28','-c:a','pcm_f32le','-y',$excerpt) | Out-Null
    $before=Invoke-Nod @('analyze',$excerpt,'--json')
    $results.Add(@{id=$case.id;factor=$case.factor;song=$song;raw=$raw;excerpt=$excerpt;before=$before;renderId=$render.renderId;
        calibration=@{nativeLead=$nativeLead;externalLead=$externalLead;trimDb=$gainDb;faderBefore=$sourceTrack.gain;faderAfter=$fader};
        render=$render;auditionStatus='unheard'})
    Write-Output "Rendered calibrated candidate: $($case.id), lead trim $gainDb dB"
}
$target=-20.
foreach ($entry in $results) {$target=[Math]::Min($target,$entry.before.loudnessLufs-1.5-$entry.before.truePeakDbtp)}
foreach ($entry in $results) {
    $file=Join-Path ([IO.Path]::GetDirectoryName($entry.raw)) 'matched.wav';$gainDb=$target-$entry.before.loudnessLufs
    for ($attempt=0;$attempt -lt 8;++$attempt) {
        Invoke-Tool $ffmpeg @('-hide_banner','-loglevel','error','-i',$entry.excerpt,'-af',"volume=$(Number $gainDb)dB",'-c:a','pcm_f32le','-y',$file) | Out-Null
        $after=Invoke-Nod @('analyze',$file,'--json')
        if ([Math]::Abs($after.loudnessLufs-$target) -le 0.05) {break};$gainDb+=$target-$after.loudnessLufs
    }
    if ([Math]::Abs($after.loudnessLufs-$target) -gt 0.05 -or $after.truePeakDbtp -gt -0.99) {throw 'Calibrated candidate normalization failed'}
    $entry.file=$file;$entry.targetLufs=$target;$entry.after=$after;$entry.sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
}
Write-Json (Join-Path $IterationDirectory 'gain-staging.json') @{schemaVersion=1;results=$results.ToArray();targetLufs=$target;
    scope='Instrument output calibration and mix balance, not an attribution of subjective quality to DSP; all global excerpts matched with constant gain';
    referenceScope='mda output is a level reference here, not a commercial quality certification';auditionStatus='unheard'}
Write-Output 'Gain-staging candidates prepared'
