param(
    [string]$Executable="$PSScriptRoot/../build/release/nod.exe",
    [string]$SourceSong="$PSScriptRoot/../build/acceptance-production/projects/trance-16-native/song.json",
    [string]$OutputDirectory="$PSScriptRoot/../build/acceptance-production/agent-workflow"
)
$ErrorActionPreference='Stop'
$Executable=[IO.Path]::GetFullPath($Executable);$SourceSong=[IO.Path]::GetFullPath($SourceSong)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new directory; existing evidence must not be overwritten'}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$toolEvents=[Collections.Generic.List[object]]::new()
function Write-Json($Path,$Value){[IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 90),[Text.UTF8Encoding]::new($false))}
function Invoke-Nod([string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Executable);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($info);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if(!$process.WaitForExit(600000)){$process.Kill($true);throw 'Workflow verification timed out'}
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$process.Dispose()
    $toolEvents.Add(@{kind='tool-call';tool='nod';arguments=$Arguments;exitCode=$code})
    if($code -ne $Expected){throw "Unexpected workflow result ($code): $text $errors"}
    return ($text | ConvertFrom-Json -AsHashtable)
}
$originalHash=(Get-FileHash -LiteralPath $SourceSong -Algorithm SHA256).Hash
$song=Join-Path $OutputDirectory 'before.json';$after=Join-Path $OutputDirectory 'after.json'
$batch=@{schemaVersion=1;commands=@(
    @{op='set-role';track='lead';role='lead'},
    @{op='set-section';id='workflow-preview';name='Preview';startTick=0;endTick=1920},
    @{op='collect-resources';includeAssets=$true})}
$batchFile=Join-Path $OutputDirectory 'setup.commands.json';Write-Json $batchFile $batch
Invoke-Nod @('song','apply',$SourceSong,'--commands',$batchFile,'--output',$song,'--json') | Out-Null
$roles=Invoke-Nod @('song','query',$song,'--view','roles','--role','lead','--json')
if($roles.tracks.Count -ne 1 -or $roles.tracks[0].track -ne 'lead'){throw 'Role selection failed'}
Write-Json (Join-Path $OutputDirectory 'roles.json') $roles
$capabilities=Invoke-Nod @('song','query',$song,'--view','capabilities','--json')
Write-Json (Join-Path $OutputDirectory 'capabilities.json') $capabilities
$proposal=$roles.tracks[0].startingChainProposal;$proposalFile=Join-Path $OutputDirectory 'chain.commands.json';Write-Json $proposalFile $proposal
$beforeHash=(Get-FileHash -LiteralPath $song -Algorithm SHA256).Hash
$review=Invoke-Nod @('song','apply',$song,'--commands',$proposalFile,'--dry-run','--json')
if((Get-FileHash -LiteralPath $song -Algorithm SHA256).Hash -ne $beforeHash){throw 'Dry run changed source'}
Write-Json (Join-Path $OutputDirectory 'review.json') $review
Invoke-Nod @('song','apply',$song,'--commands',$proposalFile,'--output',$after,'--json') | Out-Null
Invoke-Nod @('song','apply',$after,'--commands',$proposalFile,'--dry-run','--json') 5 | Out-Null
$search=Invoke-Nod @('preset','search','production stereo','--role','lead','--json')
if($search.count -ne 1 -or $search.presets[0].id -ne 'production-lead'){throw 'Preset search failed'}
Write-Json (Join-Path $OutputDirectory 'search.json') $search
$index=Invoke-Nod @('preset','index','--role','lead','--output',(Join-Path $OutputDirectory 'preset-index'),'--json')
if($index.status -ne 'ok'){throw 'Preset index incomplete'}
$versions=Invoke-Nod @('version','audition',$song,$after,'--section','workflow-preview','--output',(Join-Path $OutputDirectory 'ab'),'--json')
if($versions.status -ne 'ok' -or $versions.versions.Count -ne 2){throw 'Version audition failed'}
$trace=@{actor='public-interface-smoke-fixture';resourceSnapshotHash=$originalHash;events=@(
    @{kind='command-batch';intentId='setup';commands=$batch.commands;outcome='ok'},
    @{kind='command-batch';intentId='chain-review';commands=$proposal.commands;outcome='ok'},
    @{kind='command-batch';intentId='chain-apply';commands=$proposal.commands;outcome='ok'},
    @{kind='command-batch';intentId='chain-apply';commands=$proposal.commands;outcome='rejected';code='revision-conflict'})}
$trace.events+=@($toolEvents.ToArray())
$traceFile=Join-Path $OutputDirectory 'trace.json';Write-Json $traceFile $trace
$metrics=Invoke-Nod @('workflow','metrics',$traceFile,'--json');Write-Json (Join-Path $OutputDirectory 'metrics.json') $metrics
if((Get-FileHash -LiteralPath $SourceSong -Algorithm SHA256).Hash -ne $originalHash){throw 'Original production project changed'}
Write-Json (Join-Path $OutputDirectory 'manifest.json') @{status='ok';sourceSong=$SourceSong;sourceSha256=$originalHash;metrics=$metrics;
    scope='Public CLI smoke fixture; not a real Agent/human composition comparison or sound approval';
    checks=@('role selection','capability discovery','chain dry-run immutable','stale proposal rejected','preset search/index','named-range version audition','trace metrics','source unchanged')}
Write-Output 'Agent workflow public interface verification passed'
