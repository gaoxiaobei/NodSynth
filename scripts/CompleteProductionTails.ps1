param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$ProjectsDirectory = "$PSScriptRoot/../build/acceptance-production/projects",
    [double]$TailSeconds = 8
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable); $ProjectsDirectory = [IO.Path]::GetFullPath($ProjectsDirectory)
$worker = Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'nod_vst3_worker.exe'
$manifestPath = Join-Path $ProjectsDirectory 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
$measurementsPath = Join-Path $ProjectsDirectory 'measurements.json'
$measurements = Get-Content -LiteralPath $measurementsPath -Raw | ConvertFrom-Json -AsHashtable
function Write-Json($Path,$Value) { [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 90),[Text.UTF8Encoding]::new($false)) }
function Invoke-Nod([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable); $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $watch = [Diagnostics.Stopwatch]::StartNew(); $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync(); [long]$peak=0
    while (!$process.WaitForExit(20)) {
        if ($watch.Elapsed.TotalSeconds -gt 600) { $process.Kill($true); throw 'tail completion timed out' }
        try { $process.Refresh(); $peak=[Math]::Max($peak,$process.PeakWorkingSet64) } catch [InvalidOperationException] {}
    }
    $text=$stdout.GetAwaiter().GetResult(); $errors=$stderr.GetAwaiter().GetResult(); $watch.Stop()
    if ($process.ExitCode) { throw "nod failed: $text $errors" }
    $measurements.measurements += @{phase='complete-tail';arguments=$Arguments;elapsedMs=$watch.Elapsed.TotalMilliseconds;
        cpuMs=$process.TotalProcessorTime.TotalMilliseconds;parentPeakWorkingSetBytes=$peak;workerMemory='per-effect private bytes in render report'}
    $process.Dispose(); return ($text | ConvertFrom-Json -AsHashtable)
}
foreach ($result in $manifest.results) {
    $directory=Join-Path $ProjectsDirectory $result.id; $song=Join-Path $directory 'song.json'; $cache=Join-Path $directory 'cache'
    $tools=@(); if ($result.variant -eq 'external') { $tools=@('--vst3-worker',$worker,'--freeze-external') }
    $master=Join-Path $directory 'master.wav'; $warmFile=Join-Path $directory 'warm.wav'
    $render=Invoke-Nod (@('render',$song,'--output',$master,'--stems',(Join-Path $directory 'stems'),
        '--report',(Join-Path $directory 'render.json'),'--cache-dir',$cache,'--tail-seconds',"$TailSeconds",'--json')+$tools)
    if ($render.tailTruncated) { throw "$($result.id) still truncates the declared tail" }
    $warm=Invoke-Nod (@('render',$song,'--output',$warmFile,'--cache-dir',$cache,'--tail-seconds',"$TailSeconds",'--json')+$tools)
    if ((Get-FileHash -LiteralPath $master -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $warmFile -Algorithm SHA256).Hash) {
        throw "$($result.id) complete-tail cold/warm differs"
    }
    $unmastered=Invoke-Nod (@('render',(Join-Path $directory 'unmastered.song.json'),'--output',(Join-Path $directory 'unmastered.wav'),
        '--cache-dir',$cache,'--tail-seconds',"$TailSeconds",'--json')+$tools)
    if ($unmastered.tailTruncated) { throw "$($result.id) unmastered tail remains truncated" }
    $analysis=Invoke-Nod @('analyze',$master,'--json'); if ($analysis.truePeakDbtp -gt -0.99) { throw 'Master ceiling exceeded' }
    Write-Json (Join-Path $directory 'analysis.json') $analysis
    $result.priorTailRender=$result.render; $result.render=$render; $result.warm=$warm; $result.unmasteredRender=$unmastered
    $result.analysis=$analysis; $result.durationSeconds=$render.frames/$render.sampleRate
    $result.masterSha256=(Get-FileHash -LiteralPath $master -Algorithm SHA256).Hash
    $result.technicalChecks += 'complete declared effect tail; no truncation'
    $manifest.finalTailSeconds=$TailSeconds
    $manifest.executableSha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
    Write-Json $manifestPath $manifest; Write-Json $measurementsPath $measurements
    Write-Output "Completed full tail: $($result.id)"
}
