param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$FFmpeg = "ffmpeg",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production/loudness"
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$FFmpeg = (Get-Command $FFmpeg -ErrorAction Stop).Source
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
function Invoke-Tool($Tool, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($Tool)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(120000)) { $process.Kill(); throw 'Meter reference command timed out' }
    $text = $stdout.GetAwaiter().GetResult()
    $errors = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "$Tool failed: $text $errors" }
    $process.Dispose()
    return @{output=$text;errors=$errors}
}
$cases = [Collections.Generic.List[object]]::new()
foreach ($rate in @(44100,48000,96000)) {
    foreach ($mode in @('mono','stereo','dynamic')) {
        $audio = Join-Path $OutputDirectory "$rate-$mode.wav"
        $filter = 'volume=0.8'
        if ($mode -eq 'stereo') { $filter += ',pan=stereo|c0=c0|c1=c0' }
        if ($mode -eq 'dynamic') { $filter = "volume='if(lt(t,6),0.2,if(lt(t,12),0.8,0.4))':eval=frame" }
        Invoke-Tool $FFmpeg @('-hide_banner','-loglevel','error','-f','lavfi','-i',"sine=frequency=1000:sample_rate=$rate`:duration=18",
            '-af',$filter,'-c:a','pcm_f32le','-y',$audio) | Out-Null
        $measured = (Invoke-Tool $Executable @('analyze',$audio,'--json')).output | ConvertFrom-Json
        $reference = Invoke-Tool $FFmpeg @('-hide_banner','-nostats','-i',$audio,'-af','ebur128=peak=true','-f','null','-')
        [IO.File]::WriteAllText((Join-Path $OutputDirectory "$rate-$mode.ffmpeg.txt"),$reference.errors)
        $summaries = [regex]::Matches($reference.errors,'(?s)Summary:.*?I:\s*([-\d.]+) LUFS.*?LRA:\s*([-\d.]+) LU.*?Peak:\s*([-\d.]+) dBFS')
        if (!$summaries.Count) { throw 'FFmpeg EBU summary was not found' }
        $summary = $summaries[$summaries.Count-1]
        $integrated = [double]::Parse($summary.Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture)
        $lra = [double]::Parse($summary.Groups[2].Value,[Globalization.CultureInfo]::InvariantCulture)
        $truePeak = [double]::Parse($summary.Groups[3].Value,[Globalization.CultureInfo]::InvariantCulture)
        if ([Math]::Abs($measured.loudnessLufs-$integrated) -gt 0.15) { throw "$rate $mode integrated LUFS disagrees with FFmpeg" }
        if ([Math]::Abs($measured.loudnessRangeLu-$lra) -gt 0.5) { throw "$rate $mode LRA disagrees with FFmpeg" }
        if ([Math]::Abs($measured.truePeakDbtp-$truePeak) -gt 0.2) { throw "$rate $mode true peak disagrees with FFmpeg" }
        $cases.Add(@{sampleRate=$rate;mode=$mode;audioHash=(Get-FileHash -LiteralPath $audio -Algorithm SHA256).Hash;
            measured=$measured;reference=@{integratedLufs=$integrated;lraLu=$lra;truePeakDbtp=$truePeak}})
    }
}
$report = @{meter='libebur128-1.2.6';ffmpeg=$FFmpeg;ffmpegHash=(Get-FileHash -LiteralPath $FFmpeg -Algorithm SHA256).Hash;
    scope='Independent FFmpeg EBU R128 reference; generated 1 kHz calibration and level transitions; listening remains unheard';
    tolerance=@{integratedLu=0.15;lraLu=0.5;truePeakDb=0.2};cases=$cases.ToArray()}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'reference.json'),($report | ConvertTo-Json -Depth 30),[Text.UTF8Encoding]::new($false))
Write-Output "BS.1770 reference verified: $OutputDirectory"
