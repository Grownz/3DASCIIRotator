# Smoke tests for build\ascii3D.exe. Sanity checks only: output is not byte-stable (/fp:fast, threads).
# Run from the repo root after build.bat:  powershell -File tests\smoke.ps1
$ErrorActionPreference = 'Continue'  # native stderr must not abort checks
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'build\ascii3D.exe'
$fail = 0
function Check($name, $ok) { if ($ok) { "ok    $name" } else { "FAIL  $name"; $script:fail++ } }
function Snap($a) {  # returns exit code and count of non-space characters
    $o = & $exe @a 2>$null   # stdout only: stderr text must not count as ink
    [pscustomobject]@{ Code = $LASTEXITCODE; Ink = (($o -join '') -replace '\s', '').Length }
}

Push-Location $root
python tools\gen_min_formats.py | Out-Null   # stdlib only; writes build\models\

# Tetrahedron fixtures for the text loaders (STL/OBJ/PLY/OFF). FBX is binary and not covered.
$TetV = @(@(0,0,0),@(1,0,0),@(0,1,0),@(0,0,1)); $TetF = @(@(0,2,1),@(0,1,3),@(0,3,2),@(1,2,3))
$md = Join-Path $root 'build\models'
$stl = "solid t`n"; foreach ($f in $TetF) { $stl += " facet normal 0 0 0`n  outer loop`n"; foreach ($i in $f) { $stl += "   vertex $($TetV[$i] -join ' ')`n" }; $stl += "  endloop`n endfacet`n" }
Set-Content "$md\tet_stl.stl" ($stl + "endsolid t`n") -NoNewline
Set-Content "$md\tet_obj.obj" ((($TetV | % { "v $($_ -join ' ')" }) + ($TetF | % { "f $(($_ | % { $_ + 1 }) -join ' ')" })) -join "`n")
Set-Content "$md\tet_ply.ply" ((@('ply','format ascii 1.0','element vertex 4','property float x','property float y','property float z','element face 4','property list uchar int vertex_indices','end_header') + ($TetV | % { $_ -join ' ' }) + ($TetF | % { "3 $($_ -join ' ')" })) -join "`n")
Set-Content "$md\tet_off.off" ((@('OFF','4 4 0') + ($TetV | % { $_ -join ' ' }) + ($TetF | % { "3 $($_ -join ' ')" })) -join "`n")

$shapes = 'cube','cylinder','diamond','sphere','stego','f1','companion','maus','fish','kebab','berlin','china'
foreach ($s in $shapes) { $r = Snap @('--snapshot','-s',$s); Check "shape $s" ($r.Code -eq 0 -and $r.Ink -gt 50) }
foreach ($m in 'min_b64','min_ext','min_glb','min_mat','min_cube','tet_stl','tet_obj','tet_ply','tet_off') { $r = Snap @('--snapshot','-s',$m); Check "model $m" ($r.Code -eq 0 -and $r.Ink -gt 50) }
foreach ($x in @('--shatter'), @('--menu')) { $r = Snap (@('--snapshot','-s','cube') + $x); Check "cube $x" ($r.Code -eq 0 -and $r.Ink -gt 50) }
foreach ($l in 0..6) { $r = Snap @('--snapshot','-s','maus','--lod',"$l"); Check "maus lod $l" ($r.Code -eq 0 -and $r.Ink -gt 50) }
$m = (& $exe --snapshot -s cube --menu 2>$null) -join "`n"; Check 'menu marks loaded model' ($m -match '\|>\*cube')
$f = (& $exe --snapshot -s cube --menu --filter ste 2>$null) -join "`n"
Check 'filter shows stego, hides cube' ($f -match '\|>.stego' -and $f -notmatch '\|[> ]\*?cube')
Check 'slider hex labels' ($f -match 'mesh #[0-9A-F]{6}' -and $f -match 'light #[0-9A-F]{6}')
$ini = Join-Path $root 'build\ascii3D.ini'   # saved settings must never affect snapshots
$base = (& $exe --snapshot -s cube 2>$null) -join "`n"
try {
    Set-Content $ini "[ascii3D]`nshape=maus`ntilt=60`nspeed=90`nmesh=196`nlight=46`nfps=1`n"
    $with = (& $exe --snapshot -s cube 2>$null) -join "`n"
} finally { Remove-Item $ini -ErrorAction SilentlyContinue }
Check 'snapshot ignores ascii3D.ini' ($with -eq $base -and $base.Length -gt 0)
# scrollbar and truncation marker need a list longer than the panel and a long name
$md = Join-Path $root 'build\models'
$many = 1..30 | ForEach-Object { Join-Path $md ("many_{0:D2}.stl" -f $_) }
$long = Join-Path $md 'a_really_long_model_name_that_cannot_fit_the_panel.stl'
try {
    $many + $long | ForEach-Object { Copy-Item (Join-Path $md 'tet_stl.stl') $_ }
    $m = (& $exe --snapshot -s cube --menu 2>$null)
    $last = ($m[1..25] | ForEach-Object { $_.Substring($_.Length - 1) }) -join ''   # list rows only (the slider area below has other text)
    Check 'list scrollbar (thumb and v at the top of the list, no ^)' ($last -match '#' -and $last -match 'v' -and $last -notmatch '\^')
    $m3 = (& $exe --snapshot -s tet_stl --menu 2>$null)
    $last3 = ($m3[1..25] | ForEach-Object { $_.Substring($_.Length - 1) }) -join ''
    Check 'list scrollbar at the end (^ and thumb, no v)' ($last3 -match '\^' -and $last3 -match '#' -and $last3 -notmatch 'v')
    $m2 = (& $exe --snapshot -s a_really_long_model_name_that_cannot_fit_the_panel --menu 2>$null) -join "`n"
    Check 'cut-off names get a ~ marker' ($m2 -match 'a_really\S*~')
} finally { $many + $long | ForEach-Object { Remove-Item $_ -ErrorAction SilentlyContinue } }
$def = (& $exe --snapshot -s sphere 2>$null) -join "`n"
$bold = (& $exe --snapshot -s sphere --ramp bold 2>$null) -join "`n"
Check 'ramp presets change the glyphs' ($bold -ne $def -and $bold -match '[xX$&8]' -and $def -notmatch '[xX$&8]')
& $exe --snapshot -s sphere --ramp nope 2>&1 | Out-Null; Check 'unknown ramp fails' ($LASTEXITCODE -ne 0)
# a model file on the command line (also what dropping a file on the exe does): space and non-ASCII in the path
$dd = Join-Path $env:TEMP ("a3d drop $PID")
New-Item -ItemType Directory $dd | Out-Null
try {
    $mf = Join-Path $dd ("W{0}rfel.stl" -f [char]0xFC)
    Copy-Item (Join-Path $md 'tet_stl.stl') $mf
    Set-Content (Join-Path $dd 'notes.txt') 'x'
    $r = Snap @('--snapshot', $mf);                  Check 'model file argument loads' ($r.Code -eq 0 -and $r.Ink -gt 50)
    Push-Location $dd
    try { $r = Snap @('--snapshot', ".\$([IO.Path]::GetFileName($mf))"); Check 'relative model file path works' ($r.Code -eq 0 -and $r.Ink -gt 50) } finally { Pop-Location }
    $r = Snap @('--snapshot', '-s', 'cube', $mf);    Check '-s wins over a model file argument' ($r.Code -eq 0 -and $r.Ink -gt 1000)
    $r = Snap @('--snapshot', (Join-Path $dd 'missing.stl'));  Check 'missing model file fails' ($r.Code -ne 0)
    $r = Snap @('--snapshot', (Join-Path $dd 'notes.txt'));    Check 'unsupported model file fails' ($r.Code -ne 0)
} finally { Remove-Item $dd -Recurse -Force -ErrorAction SilentlyContinue }
& $exe --version | Out-Null; Check 'version' ($LASTEXITCODE -eq 0)
& $exe --help    | Out-Null; Check 'help'    ($LASTEXITCODE -eq 0)
& $exe --snapshot -s doesnotexist 2>&1 | Out-Null; Check 'unknown shape fails' ($LASTEXITCODE -ne 0)

$tmp = Join-Path $env:TEMP "a3d_smoke_$PID"; New-Item -ItemType Directory $tmp | Out-Null
Push-Location $tmp
& $exe --export gif -s cube | Out-Null
Check 'export gif' ($LASTEXITCODE -eq 0 -and [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("$tmp\ascii3D_cube.gif")[0..5]) -eq 'GIF89a')
& $exe --export png -s cube | Out-Null
$sig = [IO.File]::ReadAllBytes("$tmp\ascii3D_cube.png")[0..7] -join ','
Check 'export png' ($LASTEXITCODE -eq 0 -and $sig -eq '137,80,78,71,13,10,26,10')
Pop-Location; Pop-Location
Remove-Item $tmp -Recurse -Force

if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0   # CI's pwsh wrapper returns $LASTEXITCODE; do not leak the last native exit code
