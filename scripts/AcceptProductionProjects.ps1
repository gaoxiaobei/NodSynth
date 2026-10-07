param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production/projects",
    [string]$ExternalPlugin = "$PSScriptRoot/../build/release/VST3/Release/mda-vst3.vst3/Contents/x86_64-win/mda-vst3.vst3",
    [switch]$NativeOnly
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$ExternalPlugin = [IO.Path]::GetFullPath($ExternalPlugin)
$worker = Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'nod_vst3_worker.exe'
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$measurements = [Collections.Generic.List[object]]::new()
$results = [Collections.Generic.List[object]]::new()
function Write-Json($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 80), [Text.UTF8Encoding]::new($false))
}
function Invoke-Nod([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    foreach ($argument in $Arguments) {$info.ArgumentList.Add($argument)}
    $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($info)
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    [long]$peak=0
    while (!$process.WaitForExit(20)) {
        if ($watch.Elapsed.TotalSeconds -gt 600) {$process.Kill($true);throw 'project acceptance command timed out'}
        try {$process.Refresh();$peak=[Math]::Max($peak,$process.PeakWorkingSet64)} catch [InvalidOperationException] {}
    }
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();$watch.Stop()
    if ($process.ExitCode) {throw "nod failed: $text $errors"}
    $measurements.Add(@{arguments=$Arguments;elapsedMs=$watch.Elapsed.TotalMilliseconds;cpuMs=$process.TotalProcessorTime.TotalMilliseconds;
        parentPeakWorkingSetBytes=$peak;workerMemory='per-effect private bytes in render report; parent process measurement excludes child instruments'})
    $process.Dispose();return ($text | ConvertFrom-Json -AsHashtable)
}
function Apply($Song, $Commands, $Name) {
    $path=Join-Path ([IO.Path]::GetDirectoryName($Song)) "$Name.commands.json"
    Write-Json $path @{schemaVersion=1;commands=@($Commands)}
    Invoke-Nod @('song','apply',$Song,'--commands',$path,'--json') | Out-Null
}
function Pattern($Track,$Id,$Start,$End,$Grid,$Duration,$Pitches,$Velocities,$Offset='0/1') {
    @{op='add-pattern';track=$Track;id=$Id;startBar=$Start;endBar=$End;grid=$Grid;duration=$Duration;
        pitches=@($Pitches);velocities=@($Velocities);offset=$Offset;truncate=$true}
}
function Same-Audio($A,$B) {
    if ((Get-FileHash -LiteralPath $A -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $B -Algorithm SHA256).Hash) {
        throw "audio differs: $A $B"
    }
}
$presets=[IO.Path]::GetFullPath("$PSScriptRoot/../presets")
$variants=@('native');if (!$NativeOnly) {
    if (!(Test-Path -LiteralPath $ExternalPlugin)) {throw 'build the licensed mda-vst3 Release target before external acceptance'}
    $variants+= 'external'
}
foreach ($style in @('trance','house')) {foreach ($bars in @(16,40)) {foreach ($variant in $variants) {
    $id="$style-$bars-$variant";$directory=Join-Path $OutputDirectory $id
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $song=Join-Path $directory 'song.json';$bpm=if ($style -eq 'trance') {138} else {124}
    Invoke-Nod @('song','create',$song,'--bpm',"$bpm",'--bars',"$bars",'--json') | Out-Null
    $commands=[Collections.Generic.List[object]]::new()
    $roles=@('kick','clap','hats','percussion','sub','bass','lead','pluck','pad','riser','impact')
    foreach ($role in $roles) {
        $commands.Add(@{op='create-track';id=$role;name=$role})
        $commands.Add(@{op='set-mix-mode';track=$role;panMode='balance';gainMode='multiply'})
    }
    foreach ($role in @('kick','clap','hats','percussion')) {
        $commands.Add(@{op='bind-sample-kit';track=$role;kit=(Join-Path $presets 'electronic-one-shots/kit.json')})
    }
    foreach ($role in @('sub','bass','lead','pluck','pad','riser','impact')) {
        $preset=if ($role -eq 'bass') {'production-offbeat-bass'} else {"production-$role"}
        $commands.Add(@{op='bind-preset';track=$role;preset=$preset})
    }
    $end=$bars+1;$drop=if ($bars -eq 40) {17} else {1};$drumStart=if ($bars -eq 40) {9} else {1}
    $commands.Add((Pattern 'kick' 'four-on-floor' $drumStart $end '1/4' '1/16' @(36) @(112,106,109,106)))
    $commands.Add((Pattern 'clap' 'backbeat' $drumStart $end '1/2' '1/16' @(39) @(89,96) '1/4'))
    $commands.Add((Pattern 'hats' 'offbeat-hats' 1 $end '1/4' '1/32' @(42,42,46,42) @(65,79,73,84) '1/8'))
    $commands.Add((Pattern 'percussion' 'syncopation' $drumStart $end '1/8' '1/32' @(49) @(48,61,45,70) '1/16'))
    $commands.Add((Pattern 'bass' 'offbeat-bass' $drumStart $end '1/4' '1/8' @(45,45,48,43,45,45,41,43) @(100,91,96,93) '1/8'))
    $commands.Add((Pattern 'sub' 'sub-foundation' $drop $end '1/1' '3/4' @(33,33,36,31,33,33,29,31) @(82)))
    $leadPitches=if ($style -eq 'trance') {@(69,72,76,81,76,72,67,72)} else {@(69,72,76,72,67,71,74,71)}
    $commands.Add((Pattern 'lead' 'lead-motif' $drop $end '1/8' '1/16' $leadPitches @(91,75,82,88)))
    $commands.Add((Pattern 'pluck' 'answer' 1 $end '1/4' '1/8' @(57,64,60,64,55,62,59,62) @(65,80,60,77) '1/8'))
    $commands.Add((Pattern 'pad' 'harmony' 1 $end '2/1' '7/4' @(@(57,60,64),@(53,57,60),@(55,59,62),@(52,55,59)) @(63)))
    $riseStart=if ($bars -eq 40) {13} else {5};$riseEnd=$riseStart+4
    $commands.Add((Pattern 'riser' 'lift' $riseStart $riseEnd '4/1' '4/1' @(72) @(78)))
    $commands.Add((Pattern 'impact' 'downbeat-impact' $drop $end '8/1' '1/4' @(33) @(100)))
    $dropouts=if ($bars -eq 40) {@(16,32)} else {@(8,16)}
    foreach ($role in @('kick','clap','bass','sub','lead')) {
        $commands.Add(@{op='set-audio-mute';track=$role;muteBars=$dropouts;fadeMs=5})
    }
    foreach ($role in @('pad','pluck','lead')) {
        $commands.Add(@{op='add-pump';track=$role;startBar=1;endBar=$end;period='1/4';recovery='1/8';depth=0.45;skipBars=$dropouts})
    }
    $gain=@{kick=0.65;clap=0.3;hats=0.22;percussion=0.12;sub=0.5;bass=0.45;lead=0.48;pluck=0.3;pad=0.25;riser=0.2;impact=0.3}
    foreach ($role in $roles) {$commands.Add(@{op='set-gain';track=$role;gain=$gain[$role]})}
    $commands.Add(@{op='set-parameter-automation';track='riser';parameter='macro:tone';valueDomain='normalized';
        points=@(@{tick=($riseStart-1)*1920;value=0},@{tick=($riseEnd-1)*1920-1;value=1})})
    $commands.Add(@{op='set-parameter-automation';track='lead';parameter='macro:tone';valueDomain='normalized';
        points=@(@{tick=0;value=0.35},@{tick=($drop-1)*1920;value=0.65},@{tick=$bars*1920;value=0.5}) | Sort-Object tick -Unique})
    $commands.Add(@{op='create-bus';id='room';return=$true;inserts=@(@{id='space';type='reverb';parameters=@{wet=1;decay=1.2;preDelayMs=18;lowCut=250;damping=5500}})})
    $commands.Add(@{op='create-bus';id='echo';return=$true;inserts=@(@{id='echo';type='delay';parameters=@{wet=1;syncBeats=0.75;feedback=0.18;lowCut=300;highCut=7500;pingPong=1}})})
    foreach ($role in @('clap','lead','pluck','pad','riser','impact')) {
        $commands.Add(@{op='set-sends';target=$role;sends=@(@{target='room';gain=0.12;position='post-fader'},@{target='echo';gain=0.08;position='post-fader'})})
    }
    $commands.Add(@{op='set-inserts';target='bass';inserts=@(@{id='duck';type='compressor';sidechain='kick';parameters=@{thresholdDb=-24;ratio=4;attackMs=2;releaseMs=120;detectorHighpass=0}})})
    $commands.Add(@{op='set-inserts';target='riser';inserts=@(@{id='lowcut';type='eq';parameters=@{mode=1;frequency=300;q=0.707}})})
    $commands.Add(@{op='set-inserts';target='master';inserts=@(@{id='ceiling';type='limiter';quality='high';parameters=@{ceilingDb=-1;lookaheadMs=5;releaseMs=100}})})
    if ($variant -eq 'external') {
        $commands.Add(@{op='add-plugin-resource';id='mda';path=$ExternalPlugin;source='Steinberg VST3 SDK mda example';license='MIT'})
        $commands.Add(@{op='set-vst3-instrument';track='lead';pluginResource='mda';className='mda JX10';clearAutomation=$true})
        $commands.Add(@{op='set-inserts';target='echo';inserts=@(@{id='mda-delay';type='vst3';pluginResource='mda';className='mda Delay';declaredTailSeconds=4;
            parameters=@{'vst3:0'=0.18;'vst3:1'=0.4;'vst3:2'=0.12;'vst3:4'=1;'vst3:5'=0.5}})})
    }
    $commands.Add(@{op='collect-resources';includeAssets=$true})
    Apply $song $commands.ToArray() 'build'
    Invoke-Nod @('song','export-midi',$song,'--output',(Join-Path $directory 'events.mid'),'--json') | Out-Null
    $tools=if ($variant -eq 'external') {@('--vst3-worker',$worker,'--freeze-external')} else {@()}
    $mix=Join-Path $directory 'master.wav';$cache=Join-Path $directory 'cache'
    $cold=Invoke-Nod (@('render',$song,'--output',$mix,'--stems',(Join-Path $directory 'stems'),'--report',(Join-Path $directory 'render.json'),
        '--cache-dir',$cache,'--tail-seconds','8','--json')+$tools)
    if ($cold.tailTruncated) {throw "$id truncates the declared effect tail"}
    $warm=Join-Path $directory 'warm.wav'
    $warmReport=Invoke-Nod (@('render',$song,'--output',$warm,'--cache-dir',$cache,'--tail-seconds','8','--json')+$tools)
    Same-Audio $mix $warm
    $analysis=Invoke-Nod @('analyze',$mix,'--json');Write-Json (Join-Path $directory 'analysis.json') $analysis
    if ($analysis.truePeakDbtp -gt -0.99) {throw "$id exceeds -1 dBTP ceiling"}
    $unmastered=Join-Path $directory 'unmastered.song.json';[IO.File]::Copy($song,$unmastered,$true)
    Apply $unmastered @(@{op='set-inserts';target='master';inserts=@()}) 'unmastered'
    $unmasteredReport=Invoke-Nod (@('render',$unmastered,'--output',(Join-Path $directory 'unmastered.wav'),'--cache-dir',$cache,'--tail-seconds','8','--json')+$tools)
    if ($unmasteredReport.tailTruncated) {throw "$id truncates the unmastered effect tail"}
    $moved=Join-Path $directory 'relocated';[IO.Directory]::CreateDirectory($moved) | Out-Null
    [IO.File]::Copy($song,(Join-Path $moved 'song.json'),$true)
    Copy-Item -LiteralPath (Join-Path $directory 'assets') -Destination $moved -Recurse -Force
    $reference=Join-Path $directory 'fragment.wav';$relocated=Join-Path $moved 'fragment.wav'
    Invoke-Nod (@('render',$song,'--output',$reference,'--bars','1:3','--tail-seconds','1','--no-cache','--json')+$tools) | Out-Null
    Invoke-Nod (@('render',(Join-Path $moved 'song.json'),'--output',$relocated,'--bars','1:3','--tail-seconds','1','--no-cache','--json')+$tools) | Out-Null
    Same-Audio $reference $relocated
    if ($variant -eq 'native' -and $bars -eq 16) {
        foreach ($rate in @(44100,48000,96000)) {
            $referenceHash=$null
            foreach ($block in @(64,128,512)) {
                $file=Join-Path $directory "matrix-$rate-$block.wav"
                Invoke-Nod @('render',$song,'--output',$file,'--bars','1:3','--sample-rate',"$rate",'--block-size',"$block",'--tail-seconds','1','--no-cache','--json') | Out-Null
                $hash=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
                if ($referenceHash -and $hash -ne $referenceHash) {throw "$id depends on block size at $rate"};$referenceHash=$hash
            }
        }
    }
    $results.Add(@{id=$id;style=$style;bars=$bars;variant=$variant;durationSeconds=$cold.frames/$cold.sampleRate;render=$cold;warm=$warmReport;
        analysis=$analysis;masterSha256=(Get-FileHash -LiteralPath $mix -Algorithm SHA256).Hash;auditionStatus='unheard';
        technicalChecks=@('cold/warm exact','relocation fragment exact','master ceiling','dry/post/return stems');
        referenceRecording=$null;referenceDaw=$null;skilledListener=$null;listeningStatus='pending';
        externalScope=$(if ($variant -eq 'external') {'Licensed mda JX10/Delay demonstrate external paths; not a reference commercial instrument judgment'} else {'native-only'})})
    Write-Json (Join-Path $OutputDirectory 'manifest.json') @{schemaVersion=1;results=$results.ToArray();commercialAcceptance='pending listening';referenceDawStatus='not performed';
        hardware=$env:PROCESSOR_IDENTIFIER;executableSha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash}
    Write-Json (Join-Path $OutputDirectory 'measurements.json') @{schemaVersion=1;measurements=$measurements.ToArray();memorySampleIntervalMs=20}
    Write-Output "Verified $id"
}}}
