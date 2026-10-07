param(
    [Parameter(Mandatory=$true)][string]$Reference,
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$ProjectsDirectory = "$PSScriptRoot/../build/acceptance-production/projects",
    [string]$OutputDirectory = "$PSScriptRoot/../build/acceptance-production/listening",
    [double]$ReferenceStartSeconds = 64,
    [double]$DurationSeconds = 28,
    [double]$TargetLufs = -20,
    [switch]$Reshuffle
)
$ErrorActionPreference='Stop'
$Reference=[IO.Path]::GetFullPath($Reference);$Executable=[IO.Path]::GetFullPath($Executable)
$ProjectsDirectory=[IO.Path]::GetFullPath($ProjectsDirectory);$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$ffmpeg=(Get-Command ffmpeg -ErrorAction Stop).Source
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
function Write-Json($Path,$Value) {[IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 60),[Text.UTF8Encoding]::new($false))}
function Invoke-Tool($Tool,[string[]]$Arguments) {
    $info=[Diagnostics.ProcessStartInfo]::new($Tool);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach ($argument in $Arguments) {$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($info);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(120000)) {$process.Kill($true);throw 'listening preparation timed out'}
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult()
    if ($process.ExitCode) {throw "$Tool failed: $text $errors"};$process.Dispose();return $text
}
function Number($Value) {([double]$Value).ToString('G17',[Globalization.CultureInfo]::InvariantCulture)}
$sources=@(
    @{label='reference';path=$Reference;start=$ReferenceStartSeconds},
    @{label='native';path=(Join-Path $ProjectsDirectory 'trance-40-native/master.wav');report=(Join-Path $ProjectsDirectory 'trance-40-native/render.json');start=16*4*60/138},
    @{label='external';path=(Join-Path $ProjectsDirectory 'trance-40-external/master.wav');report=(Join-Path $ProjectsDirectory 'trance-40-external/render.json');start=16*4*60/138}
)
$entries=[Collections.Generic.List[object]]::new()
foreach ($source in $sources) {
    $raw=Join-Path $OutputDirectory "$($source.label).raw.wav"
    Invoke-Tool $ffmpeg @('-hide_banner','-loglevel','error','-ss',(Number $source.start),'-i',$source.path,'-t',(Number $DurationSeconds),
        '-ar','48000','-ac','2','-c:a','pcm_f32le','-y',$raw) | Out-Null
    $analysis=Invoke-Tool $Executable @('analyze',$raw,'--json') | ConvertFrom-Json -AsHashtable
    if ($null -eq $analysis.loudnessLufs) {throw 'a listening excerpt has no measurable loudness'}
    $renderId=$null
    if ($source.report) { $renderId=(Get-Content -LiteralPath $source.report -Raw | ConvertFrom-Json -AsHashtable).renderId }
    $entries.Add(@{label=$source.label;source=$source.path;sourceSha256=(Get-FileHash -LiteralPath $source.path -Algorithm SHA256).Hash;
        renderId=$renderId;startSeconds=$source.start;raw=$raw;before=$analysis;reviewer=$null;auditionStatus='unheard'})
}
$target=$TargetLufs
foreach ($entry in $entries) {
    $target=[Math]::Min($target,$entry.before.loudnessLufs-1-$entry.before.truePeakDbtp)
}
# Match with constant gain only. No dynamics processing is added to the reference.
$shuffled=@($entries.ToArray() | Sort-Object {Get-Random})
$mappingPath=Join-Path $OutputDirectory 'mapping.json'
if (!$Reshuffle -and (Test-Path -LiteralPath $mappingPath)) {
    $prior=Get-Content -LiteralPath $mappingPath -Raw | ConvertFrom-Json -AsHashtable
    $priorLabels=@($prior.entries | ForEach-Object {$_.label})
    if ($priorLabels.Count -ne $entries.Count -or @($priorLabels | Select-Object -Unique).Count -ne $entries.Count) { throw 'Prior listening labels are invalid; use Reshuffle for a new comparison' }
    $shuffled=@($priorLabels | ForEach-Object {$label=$_;$entries | Where-Object {$_.label -eq $label}})
    if ($shuffled.Count -ne $entries.Count) { throw 'Prior sources changed; use Reshuffle for a new comparison' }
}
for ($index=0;$index -lt $shuffled.Count;++$index) {
    $entry=$shuffled[$index];$gainDb=$target-$entry.before.loudnessLufs
    $file=Join-Path $OutputDirectory ("$([char](65+$index)).wav")
    Invoke-Tool $ffmpeg @('-hide_banner','-loglevel','error','-i',$entry.raw,'-af',"volume=$(Number $gainDb)dB",'-c:a','pcm_f32le','-y',$file) | Out-Null
    $after=Invoke-Tool $Executable @('analyze',$file,'--json') | ConvertFrom-Json -AsHashtable
    if ([Math]::Abs($after.loudnessLufs-$target) -gt 0.05) {throw 'loudness match failed'}
    if ($after.truePeakDbtp -gt -0.99) {throw 'listening normalization exceeds headroom'}
    $entry.file=$file;$entry.gainDb=$gainDb;$entry.after=$after;$entry.sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
}
Write-Json (Join-Path $OutputDirectory 'mapping.json') @{schemaVersion=1;targetLufs=$target;entries=$shuffled;referenceUse='user-provided local reference, private comparison only';
    dawComparison='skipped at user request';reviewer='user comparison pending';scope='Different arrangements and sounds; style reference, not a waveform-null comparison'}
$reviewPath=Join-Path $OutputDirectory 'review.json'
if (Test-Path -LiteralPath $reviewPath) { $reviewPath=Join-Path $OutputDirectory ("review-"+[Guid]::NewGuid().ToString('N')+'.json') }
Write-Json $reviewPath @{schemaVersion=1;reviewer=$null;date=$null;status='pending';
    excerpts=@($shuffled | ForEach-Object {@{file=[IO.Path]::GetFileName($_.file);sha256=$_.sha256;renderId=$_.renderId;
        transient=$null;lowEnd=$null;widthMono=$null;masking=$null;space=$null;harshnessAliasing=$null;motion=$null;transition=$null;comments=$null}})}
Write-Output "Loudness matched A/B/C prepared at $target LUFS: $OutputDirectory"
