param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production/legacy-performance",
    [string]$LegacyProjects = "$PSScriptRoot/../build/acceptance-d0-d2",
    [int]$Repetitions = 9
)
$ErrorActionPreference = 'Stop'
if ($Repetitions -lt 5) { throw 'At least five interleaved repetitions are required' }
$repository = [IO.Path]::GetFullPath("$PSScriptRoot/..")
$Executable = [IO.Path]::GetFullPath($Executable)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$LegacyProjects = [IO.Path]::GetFullPath($LegacyProjects)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
function Invoke-Process($Tool, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($Tool)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true; $info.WorkingDirectory = $repository
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $watch = [Diagnostics.Stopwatch]::StartNew(); $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    [long]$peak = 0
    while (!$process.WaitForExit(20)) {
        if ($watch.Elapsed.TotalSeconds -gt 600) { $process.Kill($true); throw 'regression command timed out' }
        try { $process.Refresh(); $peak = [Math]::Max($peak,$process.PeakWorkingSet64) } catch [InvalidOperationException] {}
    }
    $text = $stdout.GetAwaiter().GetResult(); $errors = $stderr.GetAwaiter().GetResult(); $watch.Stop()
    if ($process.ExitCode) { throw "$Tool failed: $text $errors" }
    $result = @{text=$text;elapsedMs=$watch.Elapsed.TotalMilliseconds;cpuMs=$process.TotalProcessorTime.TotalMilliseconds;parentPeakWorkingSetBytes=$peak}
    $process.Dispose(); return $result
}
function Median($Values) { $ordered = @($Values | Sort-Object); return $ordered[[int][Math]::Floor($ordered.Count/2)] }
function Write-Json($Path, $Value) { [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 80),[Text.UTF8Encoding]::new($false)) }
$run = Join-Path $OutputDirectory ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($run) | Out-Null
$archive = Join-Path $run 'eb20d0f.zip'; $source = Join-Path $run 'source'; $baselineBuild = Join-Path $run 'baseline-build'
Invoke-Process 'git' @('archive','--format=zip',"--output=$archive",'eb20d0f') | Out-Null
Expand-Archive -LiteralPath $archive -DestinationPath $source
$cache = Get-Content (Join-Path $repository 'build/release/CMakeCache.txt')
$compiler = (($cache | Where-Object {$_ -match '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)='} | Select-Object -First 1) -split '=',2)[1]
$ninja = (($cache | Where-Object {$_ -match '^CMAKE_MAKE_PROGRAM:FILEPATH='} | Select-Object -First 1) -split '=',2)[1]
Invoke-Process 'cmake' @('-S',$source,'-B',$baselineBuild,'-G','Ninja',"-DCMAKE_CXX_COMPILER=$compiler", "-DCMAKE_MAKE_PROGRAM=$ninja",
    '-DCMAKE_BUILD_TYPE=Release','-DNODSYNTH_BUILD_TESTS=OFF','-DNODSYNTH_BUILD_VST3=OFF','-DNODSYNTH_BUILD_GRAPHCHECK=OFF') | Out-Null
Invoke-Process 'cmake' @('--build',$baselineBuild,'--target','nod','-j','2') | Out-Null
$programs = @{baseline=(Join-Path $baselineBuild 'nod.exe');current=$Executable}
$results = [Collections.Generic.List[object]]::new()
foreach ($name in @('trance-16','trance-17','house-4')) {
    $song = Join-Path $LegacyProjects "$name/song.json"
    $samples = @{baseline=[Collections.Generic.List[object]]::new();current=[Collections.Generic.List[object]]::new()}
    foreach ($version in @('baseline','current')) {
        $output = Join-Path $run "$name-$version.wav"; $renderCache = Join-Path $run "$name-$version-cache"
        Invoke-Process $programs[$version] @('render',$song,'--output',$output,'--cache-dir',$renderCache,'--json') | Out-Null
    }
    $baselineHash = (Get-FileHash -LiteralPath (Join-Path $run "$name-baseline.wav") -Algorithm SHA256).Hash
    $currentHash = (Get-FileHash -LiteralPath (Join-Path $run "$name-current.wav") -Algorithm SHA256).Hash
    if ($baselineHash -ne $currentHash) { throw "Legacy complete render changed: $name" }
    for ($iteration=0; $iteration -lt $Repetitions; ++$iteration) {
        $order = @('baseline','current'); if ($iteration%2) { $order = @('current','baseline') }
        foreach ($version in $order) {
            $summary = Invoke-Process $programs[$version] @('song','query',$song,'--view','summary','--json')
            $bytes = [Text.Encoding]::UTF8.GetByteCount($summary.text)
            if ($bytes -ge 8192) { throw "Summary exceeds 8 KiB: $name $version" }
            $render = Invoke-Process $programs[$version] @('render',$song,'--output',(Join-Path $run "$name-$version.wav"),
                '--cache-dir',(Join-Path $run "$name-$version-cache"),'--json')
            $report = $render.text | ConvertFrom-Json -AsHashtable
            $samples[$version].Add(@{summaryWallMs=$summary.elapsedMs;summaryCpuMs=$summary.cpuMs;summaryBytes=$bytes;
                renderWallMs=$render.elapsedMs;renderCpuMs=$render.cpuMs;renderInternalMs=$report.milliseconds;
                peakWorkingSetBytes=$render.parentPeakWorkingSetBytes;cache=$report.cache})
        }
    }
    $comparison = @{}
    foreach ($metric in @('summaryWallMs','renderInternalMs')) {
        $before = Median @($samples.baseline | ForEach-Object {$_[$metric]})
        $after = Median @($samples.current | ForEach-Object {$_[$metric]})
        $limit = $before*1.05+5
        $comparison[$metric] = @{baselineMedianMs=$before;currentMedianMs=$after;limitMs=$limit;passed=($after -le $limit)}
    }
    $results.Add(@{project=$name;songSha256=(Get-FileHash -LiteralPath $song -Algorithm SHA256).Hash;audioSha256=$currentHash;
        samples=$samples;comparison=$comparison})
    Write-Output "Measured legacy regression: $name"
}
$passed = @($results | Where-Object {!$_.comparison.summaryWallMs.passed -or !$_.comparison.renderInternalMs.passed}).Count -eq 0
Write-Json (Join-Path $OutputDirectory 'comparison.json') @{schemaVersion=1;passed=$passed;results=$results.ToArray();runDirectory=$run;
    processor=$env:PROCESSOR_IDENTIFIER;baselineRef='eb20d0f';baselineArchiveSha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash;
    baselineExecutableSha256=(Get-FileHash -LiteralPath $programs.baseline -Algorithm SHA256).Hash;
    currentExecutableSha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;
    scope='Same existing native v3 projects; separate primed caches; interleaved process runs; full audio byte equality; medians';
    tolerance='5 percent plus 5 ms for timing noise; query wall time includes process launch, render uses internal timer; CPU and sampled peak memory retained'}
if (!$passed) { throw 'Legacy summary or warm render regression exceeds recorded tolerance' }
Write-Output 'Legacy full renders, summary and warm-render performance passed'
