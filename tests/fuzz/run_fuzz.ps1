# Runs every build\fuzz\fuzz_<fmt>.exe for a fixed time, seeded from the committed seed
# corpus (tests\fuzz\corpus\<fmt>) plus the files in models\ and build\models\.
# Build first:  tests\fuzz\build_fuzz.bat
# Usage:        powershell -File tests\fuzz\run_fuzz.ps1 [-Seconds 30]
param([int]$Seconds = 30)
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$bin  = Join-Path $root 'build\fuzz'
$fail = 0

# the ASan runtime DLL lives next to the compiler; put it on PATH
$vsw = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs  = & $vsw -latest -products * -property installationPath
$asan = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$env:PATH = (Join-Path $asan.FullName 'bin\Hostx64\x64') + ';' + $env:PATH

foreach ($fmt in 'stl','obj','ply','off','gltf','glb','3mf','fbx') {
    $exe = Join-Path $bin "fuzz_$fmt.exe"
    $work = Join-Path $bin "corpus\$fmt"            # libFuzzer grows this directory
    New-Item -ItemType Directory $work -Force | Out-Null
    $seeds = @(Join-Path $PSScriptRoot "corpus\$fmt")
    foreach ($d in 'models','build\models') {
        Get-ChildItem (Join-Path $root $d) -Filter "*.$fmt" -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName $work -Force }
    }
    if (Test-Path $seeds[0]) { Copy-Item "$($seeds[0])\*" $work -Force }
    & $exe $work "-max_total_time=$Seconds" "-artifact_prefix=$bin\crash-$fmt-" 2>&1 | Select-String 'ERROR|SUMMARY|Done .* runs' | ForEach-Object { "$fmt : $($_.Line)" }
    if ($LASTEXITCODE -ne 0) { "FAIL  $fmt (exit $LASTEXITCODE; see $bin\crash-$fmt-*)"; $fail++ } else { "ok    $fmt" }
}
if ($fail) { "$fail format(s) crashed"; exit 1 }
'no crashes'
exit 0
