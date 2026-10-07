param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$ProjectsDirectory = "$PSScriptRoot/../build/acceptance-production/projects",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production/detail-listening",
    [double]$TargetLufs = -20
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$ProjectsDirectory = [IO.Path]::GetFullPath($ProjectsDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
function Write-Json($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 80), [Text.UTF8Encoding]::new($false))
}
function Invoke-Tool($Tool, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($Tool)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(120000)) { $process.Kill($true); throw 'detail listening preparation timed out' }
    $text = $stdout.GetAwaiter().GetResult(); $errors = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode) { throw "$Tool failed: $text $errors" }
    $process.Dispose(); return $text
}
function Number($Value) { ([double]$Value).ToString('G17', [Globalization.CultureInfo]::InvariantCulture) }
$entries = [Collections.Generic.List[object]]::new()
$native = Join-Path $ProjectsDirectory 'trance-16-native'
$sources = [Collections.Generic.List[object]]::new()
foreach ($role in @('lead','pad','pluck','bass','sub','riser','impact','kick','clap','hats','percussion')) {
    $sources.Add(@{group='roles';label=$role;path=(Join-Path $native "stems/dry/$role.wav")})
}
foreach ($bus in @('room','echo')) {
    $sources.Add(@{group='returns';label=$bus;path=(Join-Path $native "stems/returns/$bus.wav")})
}
foreach ($case in @('baseline','instrument-only','effect-only','reverb-only')) {
    $sources.Add(@{group='controlled';label=$case;path=(Join-Path $native "control-$case.wav")})
}
foreach ($style in @('trance','house')) {
    $sources.Add(@{group='transitions';label=$style;path=(Join-Path $ProjectsDirectory "$style-40-native/master.wav")})
}
foreach ($source in $sources) {
    $before = Invoke-Tool $Executable @('analyze',$source.path,'--json') | ConvertFrom-Json -AsHashtable
    if ($null -eq $before.loudnessLufs -or $null -eq $before.truePeakDbtp) { throw "Unmeasurable source: $($source.path)" }
    $entries.Add(@{group=$source.group;label=$source.label;source=$source.path;before=$before;
        sourceSha256=(Get-FileHash -LiteralPath $source.path -Algorithm SHA256).Hash;auditionStatus='unheard'})
}
# Match within each comparison group, lowering the common target if a source needs peak headroom.
foreach ($group in @('roles','returns','controlled','transitions')) {
    $members = @($entries | Where-Object {$_.group -eq $group})
    $target = $TargetLufs
    foreach ($entry in $members) { $target = [Math]::Min($target,$entry.before.loudnessLufs - 1.5 - $entry.before.truePeakDbtp) }
    if ($group -eq 'controlled') { $members = @($members | Sort-Object {Get-Random}) }
    for ($index=0; $index -lt $members.Count; ++$index) {
        $entry = $members[$index]
        $name = $entry.label
        if ($group -eq 'controlled') { $name = [string][char](65+$index) }
        $directory = Join-Path $OutputDirectory $group
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        $file = Join-Path $directory "$name.wav"
        $gainDb = $target - $entry.before.loudnessLufs
        for ($attempt=0; $attempt -lt 8; ++$attempt) {
            Invoke-Tool $ffmpeg @('-hide_banner','-loglevel','error','-i',$entry.source,'-af',"volume=$(Number $gainDb)dB",
                '-c:a','pcm_f32le','-y',$file) | Out-Null
            $after = Invoke-Tool $Executable @('analyze',$file,'--json') | ConvertFrom-Json -AsHashtable
            if ($null -eq $after.loudnessLufs) { throw "Normalization produced unmeasurable loudness: $file" }
            if ([Math]::Abs($after.loudnessLufs-$target) -le 0.05) { break }
            $gainDb += $target - $after.loudnessLufs
        }
        if ([Math]::Abs($after.loudnessLufs-$target) -gt 0.05 -or $after.truePeakDbtp -gt -0.99) { throw "Normalization failed: $file" }
        $entry.file = $file; $entry.targetLufs = $target; $entry.gainDb = $gainDb; $entry.after = $after
        $entry.sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    }
}
Write-Json (Join-Path $OutputDirectory 'mapping.json') @{schemaVersion=1;entries=$entries.ToArray();
    scope='Full-length isolated dry roles, wet returns, one-factor blind comparisons and complete transitions; constant gain only';
    limitations='Role and return audition alone does not prove quality in a mix; no DAW comparison at user request'}
$review = Join-Path $OutputDirectory 'review.json'
if (Test-Path -LiteralPath $review) { $review = Join-Path $OutputDirectory ("review-"+[Guid]::NewGuid().ToString('N')+'.json') }
Write-Json $review @{schemaVersion=1;status='pending';reviewer=$null;productionExperience=$null;
    excerpts=@($entries | ForEach-Object {@{file=[IO.Path]::GetRelativePath($OutputDirectory,$_.file);sha256=$_.sha256;
        transient=$null;lowEnd=$null;widthMono=$null;masking=$null;space=$null;harshnessAliasing=$null;motion=$null;transition=$null;comments=$null}})}
Write-Output "Prepared $($entries.Count) detailed listening files: $OutputDirectory"
