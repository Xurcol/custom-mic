# Sends Spotify's sound to a chosen output device (Windows per-app output,
# the same setting as Settings > Sound > Volume mixer), or back to the default.
# Custom Mic captures Spotify per-process, so it still gets the audio while
# the real speakers stay silent.
#   spotify-route.ps1 -List                 -> JSON list of output devices
#   spotify-route.ps1 -Device "<id>"        -> route Spotify there
#   spotify-route.ps1 -Device ""            -> back to the Windows default
param([switch]$List, [switch]$Get, [string]$Name = '', [string]$Device = $null)

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace CMRoute {
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator {}
  [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceEnumerator { int EnumAudioEndpoints(int flow, int mask, out IMMDeviceCollection c); }
  [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceCollection { int GetCount(out int n); int Item(int i, out IMMDevice d); }
  [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDevice {
    int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
    int OpenPropertyStore(int access, out IPropertyStore store);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
  }
  [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IPropertyStore { int GetCount(out int n); int GetAt(int i, out PropertyKey k); int GetValue(ref PropertyKey k, out PropVariant v); }
  [StructLayout(LayoutKind.Sequential)] struct PropertyKey { public Guid fmtid; public int pid; }
  [StructLayout(LayoutKind.Sequential)] struct PropVariant { public ushort vt; public ushort r1, r2, r3; public IntPtr p; public IntPtr p2; }

  // Windows.Media.Internal.AudioPolicyConfig (Windows 10 21H2 and later).
  [Guid("ab3d4648-e242-459f-b02f-541c70306324"), InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
  interface IAudioPolicyConfigFactory {
    int m0(); int m1(); int m2(); int m3(); int m4(); int m5(); int m6(); int m7(); int m8(); int m9();
    int m10(); int m11(); int m12(); int m13(); int m14(); int m15(); int m16(); int m17(); int m18();
    [PreserveSig] int SetPersistedDefaultAudioEndpoint(uint pid, int flow, int role, IntPtr deviceId);
    [PreserveSig] int GetPersistedDefaultAudioEndpoint(uint pid, int flow, int role, out IntPtr deviceId);
    [PreserveSig] int ClearAllPersistedApplicationDefaultEndpoints();
  }

  public static class Router {
    [DllImport("combase.dll", PreserveSig = false)]
    static extern void RoGetActivationFactory(IntPtr classId, [In] ref Guid iid, [MarshalAs(UnmanagedType.IInspectable)] out object factory);
    [DllImport("combase.dll", PreserveSig = false)]
    static extern void WindowsCreateString([MarshalAs(UnmanagedType.LPWStr)] string s, int len, out IntPtr h);
    [DllImport("combase.dll", PreserveSig = false)]
    static extern void WindowsDeleteString(IntPtr h);

    const string Suffix = "#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    const string Prefix = @"\\?\SWD#MMDEVAPI#";

    public static string ListJson() {
      var en = (IMMDeviceEnumerator)new MMDeviceEnumerator();
      IMMDeviceCollection c; en.EnumAudioEndpoints(0, 1, out c);
      int n; c.GetCount(out n);
      var parts = new List<string>();
      var nameKey = new PropertyKey { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 };
      for (int i = 0; i < n; i++) {
        IMMDevice d; c.Item(i, out d);
        string id; d.GetId(out id);
        string name = id;
        try {
          IPropertyStore ps; d.OpenPropertyStore(0, out ps);
          PropVariant v; ps.GetValue(ref nameKey, out v);
          if (v.vt == 31) name = Marshal.PtrToStringUni(v.p);
        } catch {}
        parts.Add("{\"id\":\"" + Esc(id) + "\",\"name\":\"" + Esc(name) + "\"}");
      }
      return "[" + string.Join(",", parts) + "]";
    }
    static string Esc(string s) { return s.Replace("\\", "\\\\").Replace("\"", "\\\""); }

    static IAudioPolicyConfigFactory Factory() {
      string cls = "Windows.Media.Internal.AudioPolicyConfig";
      IntPtr h; WindowsCreateString(cls, cls.Length, out h);
      try {
        Guid iid = typeof(IAudioPolicyConfigFactory).GUID;
        object f; RoGetActivationFactory(h, ref iid, out f);
        return (IAudioPolicyConfigFactory)f;
      } finally { WindowsDeleteString(h); }
    }

    // Which device Windows has saved for each Spotify process ("" = default).
    public static string GetJson(string processName) {
      var f = Factory();
      var parts = new List<string>();
      foreach (var p in Process.GetProcessesByName(processName)) {
        IntPtr h;
        string dev = "";
        if (f.GetPersistedDefaultAudioEndpoint((uint)p.Id, 0, 1, out h) == 0 && h != IntPtr.Zero) {
          uint len; IntPtr raw = WindowsGetStringRawBuffer(h, out len);
          dev = Marshal.PtrToStringUni(raw, (int)len);
          WindowsDeleteString(h);
        }
        parts.Add("{\"pid\":" + p.Id + ",\"device\":\"" + Esc(dev) + "\"}");
      }
      return "[" + string.Join(",", parts) + "]";
    }
    [DllImport("combase.dll")]
    static extern IntPtr WindowsGetStringRawBuffer(IntPtr h, out uint len);

    public static int Route(string mmdeviceId) {
      var f = Factory();
      IntPtr h = IntPtr.Zero;
      if (!string.IsNullOrEmpty(mmdeviceId)) {
        string full = Prefix + mmdeviceId + Suffix;
        WindowsCreateString(full, full.Length, out h);
      }
      int count = 0;
      try {
        foreach (var p in Process.GetProcessesByName("Spotify")) {
          // Helper processes (crash handler, GPU) may refuse; only the ones
          // that play audio matter, so a refusal is skipped, not fatal.
          int a = f.SetPersistedDefaultAudioEndpoint((uint)p.Id, 0, 0, h);  // console
          int b = f.SetPersistedDefaultAudioEndpoint((uint)p.Id, 0, 1, h);  // multimedia
          if (a >= 0 && b >= 0) count++;
        }
      } finally { if (h != IntPtr.Zero) WindowsDeleteString(h); }
      return count;
    }
  }
}
"@

if ($List) { [CMRoute.Router]::ListJson(); exit 0 }
if ($Get) { [CMRoute.Router]::GetJson($(if ($Name) { $Name } else { 'Spotify' })); exit 0 }
if ($null -ne $Device) { $n = [CMRoute.Router]::Route($Device); "routed $n"; exit 0 }
