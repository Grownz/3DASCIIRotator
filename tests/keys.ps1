# Injects key events into the console of a child process (helper for tests/interactive.ps1).
param([string]$Exe, [string]$Keys, [int]$StartupMs = 1500)
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Inj {
  [StructLayout(LayoutKind.Explicit, Size=20)] public struct REC {
    [FieldOffset(0)] public ushort EventType;
    [FieldOffset(4)] public int bKeyDown;
    [FieldOffset(8)] public ushort wRepeat;
    [FieldOffset(10)] public ushort vk;
    [FieldOffset(12)] public ushort sc;
    [FieldOffset(14)] public ushort ch;
    [FieldOffset(16)] public uint ctrl;
  }
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetStdHandle(int n);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool WriteConsoleInputW(IntPtr h, REC[] r, uint n, out uint w);
  public static string Send(uint pid, ushort[] vks, ushort[] chs, uint[] ctrls) {
    FreeConsole();
    if (!AttachConsole(pid)) return "attach failed " + Marshal.GetLastWin32Error();
    IntPtr h = CreateFile();
    REC[] r = new REC[vks.Length * 2];
    for (int i = 0; i < vks.Length; i++) {
      r[2*i].EventType = 1; r[2*i].bKeyDown = 1; r[2*i].wRepeat = 1; r[2*i].vk = vks[i]; r[2*i].ch = chs[i]; r[2*i].ctrl = ctrls[i];
      r[2*i+1] = r[2*i]; r[2*i+1].bKeyDown = 0;
    }
    uint w; bool ok = WriteConsoleInputW(h, r, (uint)r.Length, out w);
    FreeConsole();
    return ok ? "sent " + w : "write failed " + Marshal.GetLastWin32Error();
  }
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
  static IntPtr CreateFile() { return CreateFileW("CONIN$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero); }
}
'@
# Keys: space separated tokens: ESC UP DOWN DEL TAB ENTER SPACE SHIFTSPACE or a literal char
$vkmap = @{ ESC=27; UP=38; DOWN=40; DEL=46; TAB=9; ENTER=13; SPACE=32; SHIFTSPACE=32; BACK=8; LEFT=37; RIGHT=39; HOME=36 }
$p = Start-Process $Exe -PassThru -WorkingDirectory (Split-Path $Exe)
Start-Sleep -Milliseconds $StartupMs
foreach ($t in $Keys.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
    $ctrl = 0
    if ($vkmap.ContainsKey($t)) { $vk = $vkmap[$t]; $ch = if ($t -in 'SPACE','SHIFTSPACE') { 32 } elseif ($t -eq 'ENTER') { 13 } elseif ($t -eq 'TAB') { 9 } else { 0 }; if ($t -eq 'SHIFTSPACE') { $ctrl = 0x10 } }
    else { $c = [int][char]$t[0]; $ch = $c; $vk = [int][char]::ToUpper($t[0]) ; if ($t -eq '/') { $vk = 0xBF } }
    $r = [Inj]::Send([uint32]$p.Id, [uint16[]]@($vk), [uint16[]]@($ch), [uint32[]]@($ctrl))
    if ($r -notlike 'sent*') { "key $t : $r" }
    Start-Sleep -Milliseconds 250
}
if (-not $p.WaitForExit(4000)) { 'did not exit'; $p.Kill() }
"exit=$($p.ExitCode)"
