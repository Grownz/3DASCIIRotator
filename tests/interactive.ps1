# Drives the real interactive program by injecting keys into its console, then checks
# what it saved to build\ascii3D.ini on exit. Local only (needs a desktop session, not run in CI).
# Run from anywhere after build.bat:  powershell -File tests\interactive.ps1
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'build\ascii3D.exe'
$ini  = Join-Path $root 'build\ascii3D.ini'
$out  = Join-Path $env:TEMP "a3d_keys_$PID.txt"
$fail = 0
$backup = if (Test-Path $ini) { Get-Content $ini -Raw } else { $null }

function Run($keys) {   # returns the saved settings as a hashtable
    Start-Process pwsh -ArgumentList '-NoProfile','-File',"$PSScriptRoot\keys.ps1",'-Exe',$exe,'-Keys',"`"$keys`"" `
        -Wait -WindowStyle Hidden -RedirectStandardOutput $out
    $h = @{}
    Get-Content $ini | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $h[$Matches[1]] = $Matches[2] } }
    $h
}
function Screen($keys) {   # returns the console text captured at each SNAP token
    $f = Join-Path $env:TEMP "a3d_snap_$PID.txt"
    Remove-Item $f -ErrorAction SilentlyContinue
    Start-Process pwsh -ArgumentList '-NoProfile','-File',"$PSScriptRoot\keys.ps1",'-Exe',$exe,'-Keys',"`"$keys`"",'-SnapFile',$f `
        -Wait -WindowStyle Hidden -RedirectStandardOutput $out
    if (Test-Path $f) { $t = Get-Content $f -Raw; Remove-Item $f; $t } else { '' }
}
function Check($name, $ok) { if ($ok) { "ok    $name" } else { "FAIL  $name"; $script:fail++ } }

try {
    Remove-Item $ini -ErrorAction SilentlyContinue
    $s = Run '+ + UP UP 3 ESC'
    Check 'speed, tilt and 1-9 jump are saved' ($s.speed -eq '25' -and $s.tilt -eq '10' -and $s.shape -eq 'diamond')
    $s = Run '+ UP ESC'
    Check 'saved settings are loaded at start' ($s.speed -eq '30' -and $s.tilt -eq '15')
    $s = Run 'SHIFTSPACE ESC';                     Check 'Shift+Space goes to the previous shape' ($s.shape -eq 'cylinder')
    $s = Run 'TAB / s t e ENTER TAB ESC';          Check 'list filter loads the match' ($s.shape -eq 'stego')
    $s = Run 'TAB RIGHT RIGHT RIGHT TAB ESC';      $m = [int]$s.mesh
    $s = Run 'TAB DEL TAB ESC';                    Check 'Del resets the mesh colour' ($m -ne 7 -and $s.mesh -eq '7')
    $t = Screen '? SNAP ESC ESC';                  Check 'help box is drawn on screen' ($t -match 'ESC / q\s+quit' -and $t -match 'any key closes')
    $t = Screen 'h SNAP ESC';                      Check 'h hides the status line' ($t -notmatch '3D ASCII Rotator')
    $t = Screen 'k SNAP k ESC';                    Check 'k shows paused in the status line' ($t -match '\(paused\)')
    $t = Screen 'H SNAP H ESC';                    Check 'Shift+H gives the compact status line without version' ($t -match '\w+\s+\d+ deg/s\s+tilt' -and $t -notmatch 'v3\.' -and $t -notmatch '\[ESC\] quit')
    $t = Screen 'x SNAP x ESC';                    Check 'x reports the quality mode' ($t -match 'quality: one sample per cell')
    $s = Run ']  ] ESC';                           Check '] raises the spin speed like +' ([int]$s.speed -ge 10)
    $t = Screen 't SNAP t t t ESC';                Check 't cycles the ramp and says so' ($t -match 'ramp: soft')
    $s = Run 'TAB . . ESC ESC';                    Check '. widens the list panel and saves it' ([int]$s.panel -ge 35)
    $s = Run 'TAB , , , , , , , , , , , , , , , ESC ESC'; Check ', narrows the panel down to its 15% minimum' ($s.panel -eq '15')
    Get-ChildItem (Join-Path $root 'build') -Filter 'ascii3D_*.gif' | Remove-Item -ErrorAction SilentlyContinue
    $t = Screen 'g ESC SNAP ESC';                  Check 'Esc cancels an export without leaving a file' ($t -match 'export cancelled' -and -not (Get-ChildItem (Join-Path $root 'build') -Filter 'ascii3D_*.gif'))
    $s = Run '? ESC HOME ESC';                     Check 'help closes without quitting; Home toggles FPS' ($s.fps -eq '1')
} finally {
    Remove-Item $out -ErrorAction SilentlyContinue
    if ($null -ne $backup) { Set-Content $ini $backup -NoNewline } else { Remove-Item $ini -ErrorAction SilentlyContinue }
}
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
