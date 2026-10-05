param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-d0-d2"
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)

function Invoke-Nod([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($value in $Arguments) { $start.ArgumentList.Add($value) }
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(120000)) { $process.Kill(); throw 'nod timed out' }
    $text = $stdout.GetAwaiter().GetResult()
    $errors = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "nod failed: $text $errors" }
    $process.Dispose()
    return ($text | ConvertFrom-Json)
}
function Write-Json($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 40), $utf8)
}
function Pattern($Track, $Id, $StartBar, $EndBar, $Grid, $Duration, $Pitches, $Velocities, $Offset = '0/1') {
    return @{op='add-pattern'; track=$Track; id=$Id; startBar=$StartBar; endBar=$EndBar; grid=$Grid;
        duration=$Duration; pitches=@($Pitches); velocities=@($Velocities); offset=$Offset; truncate=$true}
}
function Make-Song([string]$Name, [int]$Bars, [double]$Bpm, [bool]$House) {
    $directory = Join-Path $OutputDirectory $Name
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $song = Join-Path $directory 'song.json'
    $cache = Join-Path $directory ('cache-' + [Guid]::NewGuid().ToString('N'))
    Invoke-Nod @('song','create',$song,'--bpm',"$Bpm",'--bars',"$Bars",'--json') | Out-Null
    $commands = [Collections.Generic.List[object]]::new()
    $roles = @('kick','clap','closed-hat','open-hat','bass','lead','pad')
    $presets = @('kick','snare-clap','closed-hat','open-hat','trance-bass','trance-lead','trance-pad')
    if ($House) { $presets[4]='house-bass'; $presets[5]='house-chord'; $presets[6]='house-chord' }
    $gains = @(0.8,0.5,0.3,0.22,0.65,0.55,0.35)
    for ($i=0; $i -lt $roles.Count; $i++) {
        $commands.Add(@{op='create-track'; id=$roles[$i]; name=$roles[$i]})
        $commands.Add(@{op='bind-preset'; track=$roles[$i]; preset=$presets[$i]})
        $commands.Add(@{op='set-gain'; track=$roles[$i]; gain=$gains[$i]})
    }
    $commands.Add((Pattern 'kick' 'kick-pattern' 1 ($Bars+1) '1/4' '1/16' @(36) @(112,106,110,106)))
    $commands.Add((Pattern 'clap' 'clap-pattern' 1 ($Bars+1) '1/2' '1/16' @(38) @(95) '1/4'))
    $commands.Add((Pattern 'closed-hat' 'hat-pattern' 1 ($Bars+1) '1/8' '1/32' @(42) @(75,95)))
    $commands.Add((Pattern 'open-hat' 'open-pattern' 1 ($Bars+1) '1/4' '1/16' @(46) @(68) '1/8'))
    $roots = @(57,53,60,55)
    for ($bar=1; $bar -le $Bars; $bar+=4) {
        $end = [Math]::Min($bar+4,$Bars+1)
        $root = $roots[[int](($bar-1)/4) % 4]
        $quality = 'major'; if ($root -eq 57) { $quality = 'minor' }
        $commands.Add((Pattern 'bass' "bass-$bar" $bar $end '1/4' '1/8' @($root-12) @(108,100) '1/8'))
        $melody = @(($root+12),($root+15),($root+19),($root+24),($root+19),($root+15),($root+12),($root+10))
        if ($quality -eq 'major') { $melody[1]++; $melody[5]++ }
        if ($House) {
            $commands.Add((Pattern 'lead' "chord-$bar" $bar $end '1/2' '1/8' @(@{root=$root;quality=$quality;inversion=1}) @(88) '1/8'))
        } else {
            $commands.Add((Pattern 'lead' "lead-$bar" $bar $end '1/8' '1/16' $melody @(100,92,98,105)))
        }
        $commands.Add((Pattern 'pad' "pad-$bar" $bar $end '1/1' '1/1' @(@{root=$root;quality=$quality}) @(65)))
    }
    $commands.Add(@{op='set-parameter';track='lead';parameter='macro:level';value=0.18})
    $commands.Add(@{op='set-parameter-automation';track='lead';parameter='filter/cutoff';valueDomain='physical';interpolation='linear';
        points=@(@{tick=0;value=350},@{tick=[int](1920*$Bars/2);value=6500},@{tick=1920*$Bars;value=9500})})
    $commands.Add(@{op='add-pump';track='pad';startBar=1;endBar=$Bars+1;period='1/4';recovery='1/8';depth=0.7})
    $batch = @{schemaVersion=1;requestId="$Name-arrange-v1";commands=@($commands.ToArray())}
    $batchPath = Join-Path $directory 'commands.json'; Write-Json $batchPath $batch
    $applied = Invoke-Nod @('song','apply',$song,'--commands',$batchPath,'--json')
    Write-Json (Join-Path $directory 'apply.json') $applied
    $summary = Invoke-Nod @('song','query',$song,'--view','summary','--json')
    Write-Json (Join-Path $directory 'summary.json') $summary
    if (($summary | ConvertTo-Json -Depth 20).Length -ge 8192) { throw 'summary exceeds 8 KiB' }
    $ready = Invoke-Nod @('song','validate',$song,'--render-ready','--json')
    Write-Json (Join-Path $directory 'validate.json') $ready
    $cold = Invoke-Nod @('render',$song,'--output',(Join-Path $directory 'mix.wav'),'--preview-output',(Join-Path $directory 'mix.preview.wav'),
        '--report',(Join-Path $directory 'render.json'),'--stems',(Join-Path $directory 'stems'),'--cache-dir',$cache,'--json')
    $warm = Invoke-Nod @('render',$song,'--output',(Join-Path $directory 'remix.wav'),'--cache-dir',$cache,'--json')
    Write-Json (Join-Path $directory 'warm.json') $warm
    $slice = Invoke-Nod @('render',$song,'--output',(Join-Path $directory 'slice.wav'),'--bars','2:4','--format','pcm16','--cache-dir',$cache,'--json')
    Write-Json (Join-Path $directory 'slice.json') $slice
    $metrics = Invoke-Nod @('analyze',(Join-Path $directory 'mix.wav'),'--json')
    Write-Json (Join-Path $directory 'metrics.json') $metrics
    $moved = Join-Path $directory 'moved'
    [IO.Directory]::CreateDirectory($moved) | Out-Null
    Copy-Item -LiteralPath $song -Destination (Join-Path $moved 'song.json') -Force
    Copy-Item -LiteralPath (Join-Path $directory 'assets') -Destination $moved -Recurse -Force
    $movedRender = Invoke-Nod @('render',(Join-Path $moved 'song.json'),'--output',(Join-Path $moved 'mix.wav'),'--no-cache','--json')
    if ($movedRender.fileHash -ne $cold.fileHash) { throw 'moved render differs from original' }
    $row = @{name=$Name;bars=$Bars;tracks=$summary.summary.tracks;notes=$summary.summary.notes;
        coldMs=$cold.milliseconds;warmMs=$warm.milliseconds;speedup=$cold.milliseconds/$warm.milliseconds;
        warmTarget=($warm.milliseconds -le 2000 -and $cold.milliseconds/$warm.milliseconds -ge 5);
        sliceMs=$slice.milliseconds;portable=$true;auditionStatus=$cold.auditionStatus;format=$cold.format;previewFormat='pcm16'}
    Write-Host "$Name cold=$($row.coldMs) ms warm=$($row.warmMs) ms notes=$($row.notes)"
    return $row
}
$results = @((Make-Song 'trance-16' 16 138 $false), (Make-Song 'trance-17' 17 138 $false), (Make-Song 'house-4' 4 124 $true))
$previews = Join-Path $OutputDirectory 'presets'; [IO.Directory]::CreateDirectory($previews) | Out-Null
foreach ($preset in @('trance-lead','trance-bass','trance-pad','house-bass','house-chord')) {
    Invoke-Nod @('preset','audition',$preset,'--output',(Join-Path $previews "$preset.wav"),'--json') | Out-Null
    Write-Json (Join-Path $previews "$preset.json") (Invoke-Nod @('preset','inspect',$preset,'--json'))
}
Write-Json (Join-Path $OutputDirectory 'acceptance.json') @{automated=$results;audition='pending human confirmation';notes='pump is gain automation; no audio-detected sidechain'}
$results | ConvertTo-Json -Depth 10
