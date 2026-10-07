param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production"
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$measurements = [Collections.Generic.List[object]]::new()

function Write-Json($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 60), [Text.UTF8Encoding]::new($false))
}
function Invoke-Nod([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    [long]$peakMemory = 0
    while (!$process.WaitForExit(20)) {
        if ($watch.ElapsedMilliseconds -gt 180000) { $process.Kill(); throw 'production acceptance command timed out' }
        try {
            $process.Refresh()
            $peakMemory = [Math]::Max($peakMemory, $process.PeakWorkingSet64)
        } catch [InvalidOperationException] { }
    }
    $text = $stdout.GetAwaiter().GetResult()
    $errors = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "nod failed: $text $errors" }
    $watch.Stop()
    $measurements.Add(@{arguments=$Arguments;elapsedMs=$watch.Elapsed.TotalMilliseconds;
        cpuMs=$process.TotalProcessorTime.TotalMilliseconds;peakWorkingSetBytes=$peakMemory;
        memoryStatus=$(if ($peakMemory -gt 0) {'observed'} else {'process-exited-before-sample'})})
    $value = $text | ConvertFrom-Json
    $process.Dispose()
    return $value
}
function Pattern($Track, $Id, $Grid, $Duration, $Pitches, $Velocities, $Offset = '0/1') {
    return @{op='add-pattern'; track=$Track; id=$Id; startBar=1; endBar=17; grid=$Grid;
        duration=$Duration; pitches=@($Pitches); velocities=@($Velocities); offset=$Offset; truncate=$true}
}

$results = [Collections.Generic.List[object]]::new()
foreach ($style in @('trance', 'house')) {
    $directory = Join-Path $OutputDirectory $style
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $patch = Join-Path $directory 'stereo-filter.json'
    if (!(Test-Path -LiteralPath $patch)) { Invoke-Nod @('patch','create-stereo',$patch,'--json') | Out-Null }
    $inspection = Invoke-Nod @('patch','inspect',$patch,'--json')
    Write-Json (Join-Path $directory 'patch-inspect.json') $inspection
    $bpm = 138; $bass = 'trance-bass'; $lead = 'trance-lead'
    if ($style -eq 'house') { $bpm = 124; $bass = 'house-bass'; $lead = 'house-chord' }
    foreach ($variant in @('legacy', 'stereo')) {
        $song = Join-Path $directory "$variant.song.json"
        Invoke-Nod @('song','create',$song,'--bpm',"$bpm",'--bars','16','--json') | Out-Null
        $commands = [Collections.Generic.List[object]]::new()
        foreach ($role in @('kick','hat','bass','lead')) { $commands.Add(@{op='create-track';id=$role;name=$role}) }
        $commands.Add(@{op='bind-preset';track='kick';preset='kick'})
        $commands.Add(@{op='bind-preset';track='hat';preset='closed-hat'})
        $commands.Add(@{op='bind-preset';track='bass';preset=$bass})
        if ($variant -eq 'stereo') {
            $commands.Add(@{op='set-instrument';track='lead';patch='stereo-filter.json'})
            $commands.Add(@{op='set-mix-mode';track='lead';panMode='balance'})
        } else { $commands.Add(@{op='bind-preset';track='lead';preset=$lead}) }
        $commands.Add((Pattern 'kick' 'kick-notes' '1/4' '1/16' @(36) @(110,105)))
        $commands.Add((Pattern 'hat' 'hat-notes' '1/8' '1/32' @(42) @(68,88)))
        $commands.Add((Pattern 'bass' 'bass-notes' '1/4' '1/8' @(45,45,48,43) @(100) '1/8'))
        $commands.Add((Pattern 'lead' 'lead-notes' '1/8' '1/16' @(69,72,76,81,76,72,67,72) @(95,85)))
        $commands.Add(@{op='set-gain';track='lead';gain=0.65})
        $commands.Add(@{op='set-gain-automation';track='lead';points=@(@{tick=0;gain=0.8},@{tick=30720;gain=1})})
        $commands.Add(@{op='add-pump';track='lead';startBar=1;endBar=17;period='1/4';recovery='1/8';depth=0.6;skipBars=@(4,12)})
        $commands.Add(@{op='set-audio-mute';track='lead';muteBars=@(8,16);fadeMs=2})
        $commands.Add(@{op='set-parameter-automation';track='lead';parameter='filter/cutoff';valueDomain='physical';
            points=@(@{tick=0;value=300},@{tick=15360;value=5000},@{tick=30720;value=10000})})
        $batch = Join-Path $directory "$variant.commands.json"
        Write-Json $batch @{schemaVersion=1;commands=$commands.ToArray()}
        Invoke-Nod @('song','apply',$song,'--commands',$batch,'--json') | Out-Null
        Invoke-Nod @('song','export-midi',$song,'--output',(Join-Path $directory "$variant.mid"),'--json') | Out-Null
        $output = Join-Path $directory "$variant.wav"
        $report = Join-Path $directory "$variant.report.json"
        $cache = Join-Path $directory ("$variant.cache-" + [Guid]::NewGuid().ToString('N'))
        $cold = Invoke-Nod @('render',$song,'--output',$output,'--stems',(Join-Path $directory "$variant.stems"),
            '--report',$report,'--cache-dir',$cache,'--tail-seconds','1','--json')
        $analysis = Invoke-Nod @('analyze',$output,'--json')
        Write-Json (Join-Path $directory "$variant.analysis.json") $analysis
        $warm = Invoke-Nod @('render',$song,'--output',(Join-Path $directory "$variant.warm.wav"),'--cache-dir',$cache,'--tail-seconds','1','--json')
        $hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
        $warmHash = (Get-FileHash -LiteralPath (Join-Path $directory "$variant.warm.wav") -Algorithm SHA256).Hash
        if ($hash -ne $warmHash) { throw "$style $variant warm cache differs from cold render" }
        if ($variant -eq 'stereo') {
            foreach ($rate in @(44100,48000,96000)) {
                $referenceHash = $null
                foreach ($block in @(64,128,512)) {
                    $matrixFile = Join-Path $directory "matrix-$rate-$block.wav"
                    Invoke-Nod @('render',$song,'--output',$matrixFile,'--sample-rate',"$rate",'--block-size',"$block",
                        '--bars','1:3','--tail-seconds','0.1','--no-cache','--json') | Out-Null
                    $matrixHash = (Get-FileHash -LiteralPath $matrixFile -Algorithm SHA256).Hash
                    if ($referenceHash -and $matrixHash -ne $referenceHash) { throw "$style rate $rate depends on block size" }
                    $referenceHash = $matrixHash
                }
            }
        }
        $results.Add(@{style=$style;variant=$variant;renderId=$hash;report=$report;analysis=$analysis;
            cold=$cold;warm=$warm;auditionStatus='unheard';referenceRecording=$null;dawComparison='pending';reviewer=$null})
    }
}
Write-Json (Join-Path $OutputDirectory 'manifest.json') @{schemaVersion=1;results=$results.ToArray();
    scope='Q0/Q1 technical fixtures; no claim of commercial or perceptual acceptance';
    assets='Generated MIDI and native patches; external references and listening results pending'}
Write-Json (Join-Path $OutputDirectory 'measurements.json') @{processor=$env:PROCESSOR_IDENTIFIER;
    memorySamplingIntervalMs=20;
    executable=$Executable;executableHash=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;
    measurements=$measurements.ToArray()}
Write-Output "Production quality fixtures verified: $OutputDirectory"
