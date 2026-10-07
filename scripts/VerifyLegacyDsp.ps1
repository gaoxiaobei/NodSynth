param([string]$BuildDirectory = "$PSScriptRoot/../build/dev")
$ErrorActionPreference = 'Stop'
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$repository = [IO.Path]::GetFullPath("$PSScriptRoot/..")
$output = Join-Path $BuildDirectory 'production-legacy'
[IO.Directory]::CreateDirectory($output) | Out-Null
$compilerLine = Get-Content (Join-Path $BuildDirectory 'CMakeCache.txt') | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=' } | Select-Object -First 1
if (!$compilerLine) { throw 'Configured C++ compiler not found' }
$compiler = ($compilerLine -split '=',2)[1]
$info = [Diagnostics.ProcessStartInfo]::new('git')
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.WorkingDirectory = $repository
$info.ArgumentList.Add('show')
$info.ArgumentList.Add('eb20d0f:src/nodes/DspNodes.cpp')
$process = [Diagnostics.Process]::Start($info)
$source = $process.StandardOutput.ReadToEndAsync()
$errors = $process.StandardError.ReadToEndAsync()
if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'Reading pinned baseline timed out' }
$text = $source.GetAwaiter().GetResult()
$failure = $errors.GetAwaiter().GetResult()
if ($process.ExitCode -ne 0) { throw "Pinned baseline unavailable: $failure" }
$process.Dispose()
$oldSource = Join-Path $output 'DspNodes-eb20d0f.cpp'
[IO.File]::WriteAllText($oldSource, $text, [Text.UTF8Encoding]::new($false))
$object = Join-Path $output 'DspNodes-eb20d0f.obj'
& $compiler '-std=c++20' '-O2' '-DbuiltinImplementations=legacyBuiltinImplementations' "-I$repository/include" '-c' $oldSource '-o' $object
if ($LASTEXITCODE -ne 0) { throw 'Baseline DSP compilation failed' }
$probe = Join-Path $output 'legacy-probe.exe'
& $compiler '-std=c++20' '-O2' "-I$repository/include" (Join-Path $repository 'tests/runtime/LegacyDspProbe.cpp') $object `
    (Join-Path $BuildDirectory 'libnod_nodes.a') (Join-Path $BuildDirectory 'libnod_runtime.a') `
    (Join-Path $BuildDirectory 'libnod_compiler.a') (Join-Path $BuildDirectory 'libnod_model.a') `
    (Join-Path $BuildDirectory 'libnod_effects.a') '-o' $probe
if ($LASTEXITCODE -ne 0) { throw 'Baseline probe linking failed' }
& $probe
if ($LASTEXITCODE -ne 0) { throw 'Legacy DSP changed relative to eb20d0f' }
