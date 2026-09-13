// Where the console's own XNA assemblies leave managed code.
//
// Microsoft.Xna.Framework.Graphics and friends reach the console through named
// P/Invokes - D3D_Device_ReceivePackets, D3D_Texture2D_CopyData,
// XAM_IsGuideVisible, XInput_GetState - into modules ("D3D", "XAM", "XINPUT",
// "STORAGE", "Net", "AUDIO", "MEDIA") that exist only on an Xbox. Nothing on
// this machine exports those names.
//
// Rather than provide native libraries with those names, the calls are
// captured: NativeRewriter turns each extern into an ordinary managed call
// landing here, and this class routes it into Nexia. Everything not yet
// implemented is recorded once and returns a default, so a single run reports
// exactly which of the ~165 exports a title actually uses - which is a far
// better guide to what to implement than the export list is.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Nexia.Xna;

public static class NativeCalls {
  // Reported once each. A title calls some of these every frame, and a log
  // line per draw would bury everything else.
  private static readonly HashSet<string> reported = new(StringComparer.Ordinal);
  private static readonly Dictionary<string, long> counts =
      new(StringComparer.Ordinal);

  /// <summary>
  /// Records a console entry point that has no implementation yet. Called from
  /// rewritten P/Invoke bodies, which then return a default.
  /// </summary>
  public static void Unimplemented(string entry) {
    bool first;
    bool runaway = false;
    lock (reported) {
      counts.TryGetValue(entry, out long seen);
      long now = seen + 1;
      counts[entry] = now;
      first = reported.Add(entry);
      // A stub returning a default is a lie the caller may believe forever.
      // GetSupportedDisplayMode ends its loop by failing, so returning zero -
      // success - enumerated display modes until memory ran out, and looked
      // exactly like a hang. Anything called this often is doing the same.
      runaway = now == RunawayThreshold;
    }
    if (first) {
      XnaOs.Log(XnaOs.Level.Warning, $"   [native] {entry} is not implemented");
    }
    if (runaway) {
      XnaOs.Log(XnaOs.Level.Error,
                $"   [native] {entry} called {RunawayThreshold} times and is "
                + "still unimplemented - it probably ends a loop by failing, "
                + "and returning a default keeps that loop running");
    }
  }

  private const long RunawayThreshold = 100000;

  // ---- D3D --------------------------------------------------------------

  // DISPLAY_MODE_INFO, as declared inside MXF.Graphics: three 32-bit fields,
  // { int Width; int Height; SurfaceFormat Format; }. The type cannot be named
  // here - it lives in the console assembly - so it is written through a
  // pointer instead.
  [StructLayout(LayoutKind.Sequential)]
  private struct DisplayModeInfo {
    public int Width;
    public int Height;
    public int Format;  // SurfaceFormat.Color == 0
  }

  // The one display mode reported. The console offered a fixed list per video
  // standard; Nexia presents one buffer, so one mode is the honest answer.
  private const int ModeWidth = 1280;
  private const int ModeHeight = 720;

  /// <summary>
  /// Enumerates display modes. Returns 0 while a mode exists, non-zero to end.
  /// </summary>
  /// <remarks>
  /// The caller is a bare for-loop that continues while this returns 0:
  ///   for (uint i = 0; GetSupportedDisplayMode(h, i, out mode) == 0; i++)
  /// so an unimplemented stub returning 0 enumerates forever and grows the
  /// list until memory runs out - which is exactly what it did. Any console
  /// entry point that terminates a loop by failing has to be implemented
  /// before the title can start.
  /// </remarks>
  public static uint D3D_GetSupportedDisplayMode(uint adapter, uint index,
                                                 IntPtr mode) {
    if (index > 0) {
      return 1;
    }
    WriteMode(mode);
    return 0;
  }

  /// <summary>The mode in use. `refreshRate` is written in Hz.</summary>
  public static uint D3D_GetCurrentDisplayMode(uint adapter, IntPtr mode,
                                               IntPtr refreshRate) {
    WriteMode(mode);
    if (refreshRate != IntPtr.Zero) {
      Marshal.WriteInt32(refreshRate, 60);
    }
    return 0;
  }

  private static void WriteMode(IntPtr mode) {
    if (mode == IntPtr.Zero) {
      return;
    }
    Marshal.StructureToPtr(
        new DisplayModeInfo { Width = ModeWidth, Height = ModeHeight, Format = 0 },
        mode, fDeleteOld: false);
  }

  private delegate uint ReadTitleFileFn(
      [MarshalAs(UnmanagedType.LPStr)] string path, IntPtr buffer,
      uint capacity);
  private static ReadTitleFileFn readTitleFile;
  private static bool readTitleFileResolved;

  /// <summary>
  /// One file out of the mounted STFS package, or null if it holds no such
  /// file. The package is the title's filesystem - nothing is extracted.
  /// </summary>
  private static ReadTitleFileFn TitleFileReader(string path) {
    if (!readTitleFileResolved) {
      readTitleFileResolved = true;
      try {
        var program = NativeLibrary.GetMainProgramHandle();
        if (NativeLibrary.TryGetExport(program, "Nexia_XnaReadTitleFile",
                                       out IntPtr address)) {
          readTitleFile =
              Marshal.GetDelegateForFunctionPointer<ReadTitleFileFn>(address);
        }
      } catch (Exception) {
      }
    }
    return string.IsNullOrEmpty(path) ? null : readTitleFile;
  }

  /// <summary>Whether the package holds this file, without reading it.</summary>
  private delegate uint StatTitlePathFn(
      [MarshalAs(UnmanagedType.LPStr)] string path, out ulong size,
      out uint isDirectory);
  private static StatTitlePathFn statTitlePath;
  private static bool statTitlePathResolved;

  private static void ResolveStat() {
    if (statTitlePathResolved) {
      return;
    }
    statTitlePathResolved = true;
    try {
      var program = NativeLibrary.GetMainProgramHandle();
      if (NativeLibrary.TryGetExport(program, "Nexia_XnaStatTitlePath",
                                     out IntPtr address)) {
        statTitlePath =
            Marshal.GetDelegateForFunctionPointer<StatTitlePathFn>(address);
      }
    } catch (Exception) {
    }
  }

  public static bool CanStatTitlePath {
    get {
      ResolveStat();
      return statTitlePath != null;
    }
  }

  public static bool StatTitlePath(string path, out long size,
                                   out bool isDirectory) {
    size = 0;
    isDirectory = false;
    if (string.IsNullOrEmpty(path)) {
      return false;
    }
    ResolveStat();
    if (statTitlePath == null) {
      return false;
    }
    try {
      if (statTitlePath(path, out ulong bytes, out uint directory) == 0) {
        return false;
      }
      size = (long)bytes;
      isDirectory = directory != 0;
      return true;
    } catch (Exception) {
      return false;
    }
  }

  public static bool TitleFileExists(string path) {
    if (StatTitlePath(path, out _, out bool directory)) {
      return !directory;
    }
    var reader = TitleFileReader(path);
    if (reader == null) {
      return false;
    }
    try {
      return reader(path, IntPtr.Zero, 0) != 0;
    } catch (Exception) {
      return false;
    }
  }

  public static byte[] ReadTitleFile(string path) {
    var readTitleFile = TitleFileReader(path);
    if (readTitleFile == null) {
      return null;
    }
    try {
      uint size = readTitleFile(path, IntPtr.Zero, 0);
      if (size == 0) {
        return StatTitlePath(path, out _, out bool directory) && !directory
                   ? Array.Empty<byte>()
                   : null;
      }
      var bytes = new byte[size];
      var pinned = System.Runtime.InteropServices.GCHandle.Alloc(
          bytes, System.Runtime.InteropServices.GCHandleType.Pinned);
      try {
        uint read = readTitleFile(path, pinned.AddrOfPinnedObject(), size);
        if (read == 0) {
          return null;
        }
        if (read != size) {
          Array.Resize(ref bytes, (int)read);
        }
      } finally {
        pinned.Free();
      }
      return bytes;
    } catch (Exception) {
      return null;
    }
  }

  private delegate uint TitleDirectoryExistsFn(
      [MarshalAs(UnmanagedType.LPStr)] string path);
  private delegate uint ListTitleDirectoryFn(
      [MarshalAs(UnmanagedType.LPStr)] string path, uint directories,
      IntPtr buffer, uint capacity);
  private static TitleDirectoryExistsFn titleDirectoryExists;
  private static ListTitleDirectoryFn listTitleDirectory;
  private static bool titleDirectoryCallsResolved;

  private static void ResolveTitleDirectoryCalls() {
    if (titleDirectoryCallsResolved) {
      return;
    }
    titleDirectoryCallsResolved = true;
    try {
      var program = NativeLibrary.GetMainProgramHandle();
      if (NativeLibrary.TryGetExport(program, "Nexia_XnaTitleDirectoryExists",
                                     out IntPtr exists)) {
        titleDirectoryExists =
            Marshal.GetDelegateForFunctionPointer<TitleDirectoryExistsFn>(exists);
      }
      if (NativeLibrary.TryGetExport(program, "Nexia_XnaListTitleDirectory",
                                     out IntPtr list)) {
        listTitleDirectory =
            Marshal.GetDelegateForFunctionPointer<ListTitleDirectoryFn>(list);
      }
    } catch (Exception) {
    }
  }

  private delegate uint IsTitlePathFn(
      [MarshalAs(UnmanagedType.LPStr)] string path);
  private static IsTitlePathFn isTitlePath;
  private static bool isTitlePathResolved;

  public static bool IsTitlePath(string path) {
    if (string.IsNullOrEmpty(path)) {
      return false;
    }
    if (!isTitlePathResolved) {
      isTitlePathResolved = true;
      try {
        var program = NativeLibrary.GetMainProgramHandle();
        if (NativeLibrary.TryGetExport(program, "Nexia_XnaIsTitlePath",
                                       out IntPtr address)) {
          isTitlePath =
              Marshal.GetDelegateForFunctionPointer<IsTitlePathFn>(address);
        }
      } catch (Exception) {
      }
    }
    if (isTitlePath == null) {
      return false;
    }
    try {
      return isTitlePath(path) != 0;
    } catch (Exception) {
      return false;
    }
  }

  public static bool TitleDirectoryExists(string path) {
    if (string.IsNullOrEmpty(path)) {
      return false;
    }
    ResolveTitleDirectoryCalls();
    if (titleDirectoryExists == null) {
      return false;
    }
    try {
      return titleDirectoryExists(path) != 0;
    } catch (Exception) {
      return false;
    }
  }

  public static string[] ListTitleDirectory(string path, bool directories) {
    if (string.IsNullOrEmpty(path)) {
      return null;
    }
    ResolveTitleDirectoryCalls();
    if (listTitleDirectory == null) {
      return null;
    }
    try {
      uint want = directories ? 1u : 0u;
      uint size = listTitleDirectory(path, want, IntPtr.Zero, 0);
      if (size == 0) {
        return null;
      }
      var bytes = new byte[size];
      var pinned = System.Runtime.InteropServices.GCHandle.Alloc(
          bytes, System.Runtime.InteropServices.GCHandleType.Pinned);
      uint read;
      try {
        read = listTitleDirectory(path, want, pinned.AddrOfPinnedObject(),
                                  size);
      } finally {
        pinned.Free();
      }
      if (read == 0) {
        return null;
      }
      var text = System.Text.Encoding.UTF8
                     .GetString(bytes, 0, (int)Math.Min(read, size))
                     .TrimEnd('\0')
                     .TrimEnd('\n');
      return text.Length == 0 ? Array.Empty<string>() : text.Split('\n');
    } catch (Exception) {
      return null;
    }
  }

  /// <summary>
  /// TitleContainer.OpenStream, answered from the package. The console reads
  /// title files out of its STFS container; so do we.
  /// </summary>
  public static System.IO.Stream OpenTitleStream(string path) {
    var bytes = ReadTitleFile(path);
    if (bytes == null) {
      throw new System.IO.FileNotFoundException(
          "Error loading \"" + path + "\". File not found.", path);
    }
    return new System.IO.MemoryStream(bytes, writable: false);
  }

  private delegate void IndexElementSizeFn(uint handle, uint elementSize);
  private static IndexElementSizeFn indexElementSize;
  private static bool indexElementSizeResolved;

  public static void IndexElementSize(uint handle, uint elementSize) {
    if (!indexElementSizeResolved) {
      indexElementSizeResolved = true;
      try {
        var program = NativeLibrary.GetMainProgramHandle();
        if (NativeLibrary.TryGetExport(program, "Nexia_XnaIndexElementSize",
                                       out IntPtr address)) {
          indexElementSize =
              Marshal.GetDelegateForFunctionPointer<IndexElementSizeFn>(address);
        }
      } catch (Exception) {
      }
    }
    try {
      indexElementSize?.Invoke(handle, elementSize);
    } catch (Exception) {
    }
  }

  /// <summary>How many console calls have been made, of any kind.</summary>
  public static long Count {
    get {
      long total = 0;
      lock (reported) {
        foreach (var entry in counts) total += entry.Value;
      }
      return total;
    }
  }

  /// <summary>
  /// Every console entry point reached so far and how often, most-used first.
  /// This is the list worth working through.
  /// </summary>
  public static string Summarise() {
    var lines = new List<KeyValuePair<string, long>>();
    lock (reported) {
      lines.AddRange(counts);
    }
    lines.Sort((a, b) => b.Value.CompareTo(a.Value));

    var text = new System.Text.StringBuilder();
    text.Append(lines.Count).AppendLine(" console entry point(s) reached:");
    foreach (var line in lines) {
      text.Append("  ").Append(line.Value.ToString().PadLeft(9)).Append("  ")
          .AppendLine(line.Key);
    }
    return text.ToString();
  }
}
