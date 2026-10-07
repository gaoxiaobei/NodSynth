param(
    [Parameter(Mandatory=$true)][string]$Reference,
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$ProjectsDirectory = "$PSScriptRoot/../build/acceptance-production/projects",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production/sound-iteration-2"
)
$ErrorActionPreference = 'Stop'
$Executable=[IO.Path]::GetFullPath($Executable); $ProjectsDirectory=[IO.Path]::GetFullPath($ProjectsDirectory)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory); $Reference=[IO.Path]::GetFullPath($Reference)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Iteration directory already exists; use a new directory to preserve listening versions' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$ffmpeg=(Get-Command ffmpeg -ErrorAction Stop).Source
$worker=Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'nod_vst3_worker.exe'
$measurements=[Collections.Generic.List[object]]::new()
function Write-Json($Path,$Value) { [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 90),[Text.UTF8Encoding]::new($false)) }
function Invoke-Tool($Tool,[string[]]$Arguments) {
    $info=[Diagnostics.ProcessStartInfo]::new($Tool);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach ($argument in $Arguments) {$info.ArgumentList.Add($argument)}
    $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($info)
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();[long]$peak=0
    while (!$process.WaitForExit(20)) {
        if ($watch.Elapsed.TotalSeconds -gt 600) {$process.Kill($true);throw 'sound iteration command timed out'}
        try {$process.Refresh();$peak=[Math]::Max($peak,$process.PeakWorkingSet64)} catch [InvalidOperationException] {}
    }
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();$watch.Stop()
    if ($process.ExitCode) {throw "$Tool failed: $text $errors"}
    $measurements.Add(@{arguments=$Arguments;elapsedMs=$watch.Elapsed.TotalMilliseconds;cpuMs=$process.TotalProcessorTime.TotalMilliseconds;parentPeakWorkingSetBytes=$peak})
    $process.Dispose();return $text
}
function Invoke-Nod([string[]]$Arguments) { return (Invoke-Tool $Executable $Arguments | ConvertFrom-Json -AsHashtable) }
function Number($Value) { ([double]$Value).ToString('G17',[Globalization.CultureInfo]::InvariantCulture) }

# Recover the exact previously delivered masters from the retained content cache before making new candidates.
$priorManifest=Get-Content (Join-Path $ProjectsDirectory 'manifest.json') -Raw | ConvertFrom-Json -AsHashtable
$reviewedProjects=Join-Path $OutputDirectory 'reviewed-projects'
foreach ($variant in @('native','external')) {
    $id="trance-40-$variant";$project=@($priorManifest.results | Where-Object {$_.id -eq $id})[0]
    $prior=$project.priorTailRender
    if (!$prior) {throw "Missing previous render provenance for $id"}
    $match=$null
    foreach ($file in Get-ChildItem (Join-Path $ProjectsDirectory "$id/cache") -Filter mix.wav -Recurse) {
        $identity=Invoke-Nod @('audition','query',$file.FullName,'--json')
        if ($identity.fileHash -eq $prior.fileHash) {$match=$file.FullName;break}
    }
    if (!$match) {throw "Previously reviewed master is missing: $id"}
    $directory=Join-Path $reviewedProjects $id;[IO.Directory]::CreateDirectory($directory) | Out-Null
    [IO.File]::Copy($match,(Join-Path $directory 'master.wav'),$false)
    Write-Json (Join-Path $directory 'render.json') $prior
}
$reviewedListening=Join-Path $OutputDirectory 'reviewed-listening';[IO.Directory]::CreateDirectory($reviewedListening) | Out-Null
$listening=Join-Path ([IO.Directory]::GetParent($ProjectsDirectory).FullName) 'listening'
[IO.File]::Copy((Join-Path $listening 'mapping.json'),(Join-Path $reviewedListening 'mapping.json'),$false)
Invoke-Tool 'pwsh' @('-NoProfile','-File',(Join-Path $PSScriptRoot 'PrepareProductionListening.ps1'),'-Reference',$Reference,
    '-Executable',$Executable,'-ProjectsDirectory',$reviewedProjects,'-OutputDirectory',$reviewedListening) | Out-Null
$delivered=Get-Content (Join-Path $listening 'review.json') -Raw | ConvertFrom-Json -AsHashtable
$recovered=Get-Content (Join-Path $reviewedListening 'review.json') -Raw | ConvertFrom-Json -AsHashtable
foreach ($excerpt in $delivered.excerpts) {
    $actual=@($recovered.excerpts | Where-Object {$_.file -eq $excerpt.file})[0]
    if ($actual.sha256 -ne $excerpt.sha256) {throw "Recovered listening excerpt differs from delivery: $($excerpt.file)"}
}
Write-Output 'Recovered original A/B/C byte-for-byte'

$assets=Join-Path $OutputDirectory 'assets';[IO.Directory]::CreateDirectory($assets) | Out-Null
$kit=Join-Path $assets 'punch-kit';Invoke-Nod @('sampler','create-kit',$kit,'--punch-kick','--json') | Out-Null
$patch=Get-Content (Join-Path $PSScriptRoot '../presets/production-roles/production-lead.json') -Raw | ConvertFrom-Json -AsHashtable
foreach ($node in $patch.nodes) {
    if ($node.id -eq 'oscillator') { $node.parameters.voices=8;$node.parameters.detune=24;$node.parameters.spread=0.9;$node.parameters.blend=0.8 }
    if ($node.id -eq 'envelope') { $node.parameters.attack=0.003;$node.parameters.decay=0.14;$node.parameters.sustain=0.72;$node.parameters.release=0.36 }
    if ($node.id -eq 'filter') { $node.parameters.depth=1.3 }
}
$patch.production.version=2;$patch.production.auditionStatus='unheard'
$patch.production.revisionReason='Candidate responding to user report of toy-like native lead; wider detune, stronger contour and longer musical release; not approved'
$leadPatch=Join-Path $assets 'lead-v2.json';Write-Json $leadPatch $patch
$voice=@{op='set-instrument';track='lead';patch=$leadPatch}
$kick=@{op='bind-sample-kit';track='kick';kit=(Join-Path $kit 'kit.json')}
$space=@{op='set-sends';target='lead';sends=@(@{target='room';gain=0.27;position='post-fader'},@{target='echo';gain=0.16;position='post-fader'})}
$soften=@{op='set-inserts';target='lead';inserts=@(
    @{id='body-lowcut';type='eq';parameters=@{mode=1;frequency=130;q=0.707}},
    @{id='presence-control';type='eq';parameters=@{mode=0;frequency=3200;gainDb=-3;q=0.9}},
    @{id='top-control';type='eq';parameters=@{mode=4;frequency=4500;gainDb=-3;q=0.707}})}
$subDuck=@{op='set-inserts';target='sub';inserts=@(@{id='kick-space';type='compressor';sidechain='kick';parameters=@{thresholdDb=-24;ratio=5;attackMs=1;releaseMs=100;detectorHighpass=0}})}
$cases=@(
    @{id='kick-only';variant='native';commands=@($kick);factor='Only the kick sample edition changes; downstream detector consequences are retained'},
    @{id='voice-only';variant='native';commands=@($voice);factor='Only the lead patch changes; MIDI, mix and sends retained'},
    @{id='space-only';variant='native';commands=@($space);factor='Only lead room/echo send amounts change'},
    @{id='external-softened';variant='external';commands=@($soften);factor='Only lead EQ inserts change; external instrument and delay retained'},
    @{id='native-combined';variant='native';commands=@($kick,$voice,$space,$soften,$subDuck);factor='Combined candidate, not used for single-factor attribution'},
    @{id='external-combined';variant='external';commands=@($kick,$soften,$subDuck);factor='Combined candidate, not used for single-factor attribution'}
)
$results=[Collections.Generic.List[object]]::new()
foreach ($case in $cases) {
    $directory=Join-Path $OutputDirectory $case.id;[IO.Directory]::CreateDirectory($directory) | Out-Null
    $source=Join-Path $ProjectsDirectory "trance-40-$($case.variant)/song.json"
    $commands=@($case.commands)+@(@{op='collect-resources';includeAssets=$true})
    $batch=Join-Path $directory 'commands.json';Write-Json $batch @{schemaVersion=1;commands=$commands}
    $song=Join-Path $directory 'song.json';Invoke-Nod @('song','apply',$source,'--commands',$batch,'--output',$song,'--json') | Out-Null
    $tools=@();if ($case.variant -eq 'external') {$tools=@('--vst3-worker',$worker,'--freeze-external')}
    $raw=Join-Path $directory 'raw.wav'
    $render=Invoke-Nod (@('render',$song,'--output',$raw,'--bars','17:33','--tail-seconds','8','--stems',(Join-Path $directory 'stems'),
        '--report',(Join-Path $directory 'render.json'),'--cache-dir',(Join-Path $ProjectsDirectory "trance-40-$($case.variant)/cache"),'--json')+$tools)
    if ($render.tailTruncated) {throw "Candidate tail truncated: $($case.id)"}
    $excerpt=Join-Path $directory 'excerpt.wav'
    Invoke-Tool $ffmpeg @('-hide_banner','-loglevel','error','-i',$raw,'-t','28','-c:a','pcm_f32le','-y',$excerpt) | Out-Null
    $before=Invoke-Nod @('analyze',$excerpt,'--json')
    $results.Add(@{id=$case.id;variant=$case.variant;factor=$case.factor;song=$song;raw=$raw;excerpt=$excerpt;before=$before;render=$render;
        renderId=$render.renderId;sourceSha256=(Get-FileHash -LiteralPath $raw -Algorithm SHA256).Hash;auditionStatus='unheard'})
    Write-Output "Rendered candidate: $($case.id)"
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
    if ([Math]::Abs($after.loudnessLufs-$target) -gt 0.05 -or $after.truePeakDbtp -gt -0.99) {throw 'Candidate normalization failed'}
    $entry.file=$file;$entry.targetLufs=$target;$entry.gainDb=$gainDb;$entry.after=$after;$entry.sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
}
Write-Json (Join-Path $OutputDirectory 'manifest.json') @{schemaVersion=1;results=$results.ToArray();targetLufs=$target;auditionStatus='unheard';
    reviewedBaseline=$reviewedListening;sourceFeedback='A more spatial than B, B toy-like, reference C has stronger kick and less harshness; A is progress but not accepted';
    fixed=@('tempo','notes','velocity','drop range','sample rate','block size');externalScope='mda instrument and delay remain external resources; no claim of commercial acceptance'}
Write-Json (Join-Path $OutputDirectory 'measurements.json') @{schemaVersion=1;processor=$env:PROCESSOR_IDENTIFIER;measurements=$measurements.ToArray()}
Write-Json (Join-Path $OutputDirectory 'review.json') @{schemaVersion=1;status='pending';reviewer=$null;
    excerpts=@($results | ForEach-Object {@{id=$_.id;sha256=$_.sha256;renderId=$_.renderId;transient=$null;lowEnd=$null;widthMono=$null;masking=$null;space=$null;harshnessAliasing=$null;comments=$null}})}
Write-Output "Six matched sound candidates prepared: $OutputDirectory"
