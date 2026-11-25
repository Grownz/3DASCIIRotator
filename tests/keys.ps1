# Injects key events into the console of a child process (helper for tests/interactive.ps1).
param([string]$Exe, [string]$Keys, [int]$StartupMs = 1500, [string]$SnapFile = "")
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
    [FieldOffset(4)] public short mx;
    [FieldOffset(6)] public short my;
    [FieldOffset(8)] public uint mbtn;
    [FieldOffset(12)] public uint mctrl;
    [FieldOffset(16)] public uint mflags;
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
  [StructLayout(LayoutKind.Sequential)] public struct CSBI { public short sx, sy, cx, cy; public ushort attr; public short l, t, r, b, mx, my; }
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetConsoleScreenBufferInfo(IntPtr h, out CSBI i);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool ReadConsoleOutputCharacterW(IntPtr h, System.Text.StringBuilder sb, uint n, uint coord, out uint read);
  // left-button drag from (x0,y0) to (x1,y1): press, one move, release (MOUSE_EVENT = 2)
  public static string Drag(uint pid, short x0, short y0, short x1, short y1) {
    FreeConsole();
    if (!AttachConsole(pid)) return "attach failed " + Marshal.GetLastWin32Error();
    IntPtr h = CreateFile();
    REC[] r = new REC[3];
    for (int i = 0; i < 3; i++) r[i].EventType = 2;
    r[0].mx = x0; r[0].my = y0; r[0].mbtn = 1; r[0].mflags = 0;
    r[1].mx = x1; r[1].my = y1; r[1].mbtn = 1; r[1].mflags = 1;
    r[2].mx = x1; r[2].my = y1; r[2].mbtn = 0; r[2].mflags = 0;
    uint w; bool ok = WriteConsoleInputW(h, r, 3, out w);
    FreeConsole();
    return ok ? "sent " + w : "write failed " + Marshal.GetLastWin32Error();
  }
  public static string Snap(uint pid) {
    FreeConsole();
    if (!AttachConsole(pid)) return "attach failed " + Marshal.GetLastWin32Error();
    IntPtr h = CreateFileW("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    CSBI i; if (!GetConsoleScreenBufferInfo(h, out i)) { FreeConsole(); return "info failed"; }
    System.Text.StringBuilder all = new System.Text.StringBuilder();
    for (int y = i.t; y <= i.b; y++) {
      System.Text.StringBuilder sb = new System.Text.StringBuilder(i.sx + 1); uint rd;
      ReadConsoleOutputCharacterW(h, sb, (uint)i.sx, (uint)((ushort)0 | ((uint)(ushort)y << 16)), out rd);
      all.AppendLine(sb.ToString().TrimEnd());
    }
    FreeConsole();
    return all.ToString();
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
    if ($t -like "DRAG:*") { $c = $t.Substring(5).Split(","); $r = [Inj]::Drag([uint32]$p.Id, [int16]$c[0], [int16]$c[1], [int16]$c[2], [int16]$c[3]); if ($r -notlike "sent*") { "drag: $r" }; Start-Sleep -Milliseconds 250; continue }
    if ($t -eq "SNAP") { Start-Sleep -Milliseconds 400; if ($SnapFile) { [Inj]::Snap([uint32]$p.Id) | Add-Content -LiteralPath $SnapFile -Encoding UTF8 }; continue }
    if ($vkmap.ContainsKey($t)) { $vk = $vkmap[$t]; $ch = if ($t -in 'SPACE','SHIFTSPACE') { 32 } elseif ($t -eq 'ENTER') { 13 } elseif ($t -eq 'TAB') { 9 } else { 0 }; if ($t -eq 'SHIFTSPACE') { $ctrl = 0x10 } }
    else { $c = [int][char]$t[0]; $ch = $c; $vk = [int][char]::ToUpper($t[0]) ; if ($t -eq '/') { $vk = 0xBF } }
    $r = [Inj]::Send([uint32]$p.Id, [uint16[]]@($vk), [uint16[]]@($ch), [uint32[]]@($ctrl))
    if ($r -notlike 'sent*') { "key $t : $r" }
    Start-Sleep -Milliseconds 250
}
if (-not $p.WaitForExit(4000)) { 'did not exit'; $p.Kill() }
"exit=$($p.ExitCode)"
