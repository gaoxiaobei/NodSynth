param(
    [string]$Executable = "$PSScriptRoot/../build/release/nod.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../presets/production-roles"
)
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
function Write-Json($Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 60), [Text.UTF8Encoding]::new($false))
}
function Mapping($Node, $Parameter, $Low, $High, $Curve = 'linear') {
    @{node=$Node;parameter=$Parameter;minimum=$Low;maximum=$High;curve=$Curve}
}
$presets = [IO.Path]::GetFullPath("$PSScriptRoot/../presets")
$roles = @(
    @{id='production-lead';source='stereo-unison-lead.json';role='lead';range=@(60,96);velocity=@(55,120);width='stereo; balance at center';
        oscillator=@{voices=7;detune=18;spread=0.75;level=0.14};envelope=@{attack=0.008;decay=0.22;sustain=0.6;release=0.28};filter=@{cutoff=1600;depth=2}},
    @{id='production-pad';source='stereo-unison-pad.json';role='pad';range=@(48,84);velocity=@(40,110);width='stereo; avoid doubling sub register';
        oscillator=@{voices=8;detune=11;spread=0.9;level=0.1};envelope=@{attack=0.3;decay=0.5;sustain=0.7;release=1.2};filter=@{cutoff=1000;depth=1.2}},
    @{id='production-pluck';source='stereo-unison-lead.json';role='pluck';range=@(55,91);velocity=@(50,120);width='moderate stereo; mono compatible audition pending';
        oscillator=@{voices=3;detune=9;spread=0.45;phase=0;level=0.16};envelope=@{attack=0.002;decay=0.18;sustain=0.08;release=0.12};filter=@{cutoff=700;depth=3.5}},
    @{id='production-offbeat-bass';source='stereo-unison-lead.json';role='bass';range=@(28,55);velocity=@(65,120);width='mono (spread 0, phase retrigger)';
        oscillator=@{voices=1;detune=0;spread=0;phase=0;level=0.2};envelope=@{attack=0.003;decay=0.16;sustain=0.25;release=0.07};filter=@{cutoff=240;depth=2.5}},
    @{id='production-sub';source='stereo';role='sub';range=@(24,48);velocity=@(65,120);width='mono sine; center pan';
        oscillator=@{waveform=0;level=0.2};envelope=@{attack=0.008;decay=0.1;sustain=0.85;release=0.09};filter=@{cutoff=2000;depth=0}},
    @{id='production-riser';source='stereo-unison-pad.json';role='riser';range=@(60,84);velocity=@(50,110);width='stereo; highpass on track';
        oscillator=@{voices=8;detune=28;spread=1;level=0.08};envelope=@{attack=2;decay=0.4;sustain=0.8;release=0.2};filter=@{cutoff=300;depth=0}},
    @{id='production-impact';source='stereo-unison-lead.json';role='impact';range=@(28,52);velocity=@(65,120);width='narrow stereo; low-frequency track kept centered';
        oscillator=@{voices=5;detune=25;spread=0.2;phase=0;level=0.18};envelope=@{attack=0.002;decay=0.8;sustain=0;release=0.5};filter=@{cutoff=300;depth=4}}
)
$template = Join-Path $OutputDirectory 'stereo-template.json'
if (!(Test-Path -LiteralPath $template)) {
    & $Executable patch create-stereo $template --json | Out-Null
    if ($LASTEXITCODE) { throw 'failed to create stereo template' }
}
$manifest = [Collections.Generic.List[object]]::new()
foreach ($role in $roles) {
    $source = if ($role.source -eq 'stereo') {$template} else {Join-Path $presets $role.source}
    $patch = [IO.File]::ReadAllText($source) | ConvertFrom-Json -AsHashtable
    foreach ($node in $patch.nodes) {
        if ($role.ContainsKey($node.id)) { foreach ($key in $role[$node.id].Keys) { $node.parameters[$key] = $role[$node.id][$key] } }
        if ($node.id -eq 'pan') { $node.parameters.pan = 0 }
    }
    $patch.macros = @(
        @{id='tone';mappings=@((Mapping 'filter' 'cutoff' 120 10000 'log'))},
        @{id='contour';mappings=@((Mapping 'filter' 'depth' 0 4))},
        @{id='release';mappings=@((Mapping 'envelope' 'release' 0.03 1.5 'log'))}
    )
    if ($role.source -ne 'stereo') { $patch.macros += @{id='width';mappings=@((Mapping 'oscillator' 'spread' 0 1))} }
    $patch.production = @{schemaVersion=1;role=$role.role;recommendedMidiRange=$role.range;recommendedVelocity=$role.velocity;
        widthPolicy=$role.width;levelPolicy='single voice source level 0.08..0.2; measure actual polyphonic peaks before mastering';
        dependencies=@();auditionStatus='unheard';version=1}
    $path = Join-Path $OutputDirectory "$($role.id).json"
    Write-Json $path $patch
    $manifest.Add(@{id=$role.id;path="$($role.id).json";role=$role.role;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;metadata=$patch.production})
}
Write-Json (Join-Path $OutputDirectory 'manifest.json') @{schemaVersion=1;presets=$manifest.ToArray();auditionStatus='unheard';
    drums='../electronic-one-shots/kit.json';license='Original patch settings; sample kit CC0-1.0';
    listening=@{reviewer=$null;reference=$null;status='pending'}}
Write-Output "Production roles generated: $OutputDirectory"
