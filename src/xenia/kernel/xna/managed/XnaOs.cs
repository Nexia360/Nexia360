// The managed mirror of xna_os.h.
//
// Nexia is the operating system under a hosted XNA title, and this is the
// syscall stub layer: a struct of unmanaged function pointers handed over at
// startup, plus wrappers that turn its raw buffers into ordinary .NET values.
//
// Layout here must match xna_os.h field for field. It is append-only and
// carries its own size, so a managed side built against an older host still
// works - check Supports() before calling anything added after version 1.
using System;
using System.Runtime.InteropServices;

namespace Nexia.Xna;

[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct XnaOsUser {
  public ulong Xuid;
  public ulong OnlineXuid;
  public uint SigninState;
  public uint IsLiveEnabled;
  public uint IsGuest;
  public uint Country;
  public uint Language;
  // Fixed 16 bytes of NUL-terminated UTF-8; a gamertag is at most 15 chars.
  public unsafe fixed byte GamertagUtf8[16];
}

public enum XnaOsSigninState : uint {
  NotSignedIn = 0,
  SignedInLocally = 1,
  SignedInToLive = 2,
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
public unsafe struct XnaOsTable {
  public uint Size;
  public uint Version;
  public delegate* unmanaged<uint> GetUserSlotCount;
  public delegate* unmanaged<uint, XnaOsUser*, int> GetUser;
  public delegate* unmanaged<uint> GetTitleId;
  public delegate* unmanaged<byte*, uint, uint> GetTitleName;
  public delegate* unmanaged<uint, byte*, byte*, uint, uint> ResolveStorageContainer;
  public delegate* unmanaged<byte*, uint, uint> GetStorageDeviceName;
  public delegate* unmanaged<ulong> GetStorageTotalSpace;
  public delegate* unmanaged<ulong> GetStorageFreeSpace;
  public delegate* unmanaged<uint> GuideIsVisible;
  public delegate* unmanaged<uint, byte*, byte*, byte**, uint, uint, uint> GuideBeginMessageBox;
  public delegate* unmanaged<uint, byte*, byte*, byte*, uint, uint> GuideBeginKeyboard;
  public delegate* unmanaged<uint, int*, uint*, int> GuidePoll;
  public delegate* unmanaged<uint, byte*, uint, uint> GuideGetText;
  public delegate* unmanaged<uint, void> GuideRelease;
  public delegate* unmanaged<uint, byte*, void> Log;
}

public enum XnaOsGuideStatus {
  UnknownRequest = -1,
  Pending = 0,
  Completed = 1,
  Cancelled = 2,
}

/// <summary>The console, as seen from managed code.</summary>
public static unsafe class XnaOs {
  private static XnaOsTable* table;

  public static bool IsAvailable => table != null;

  internal static void Bind(XnaOsTable* osTable) {
    if (osTable == null) throw new ArgumentNullException(nameof(osTable));
    table = osTable;
  }

  /// <summary>
  /// Whether the host is new enough to have the field at <paramref name="offset"/>.
  /// Everything the host actually shipped lies within its reported Size.
  /// </summary>
  private static bool Supports(int offset) =>
      table != null && table->Size >= (uint)offset + (uint)IntPtr.Size;

  private static XnaOsTable* Required() =>
      table != null ? table
                    : throw new InvalidOperationException(
                          "Nexia did not hand over an OS table - this assembly " +
                          "is only usable inside the emulator.");

  // ---- users --------------------------------------------------------------

  public static int UserSlotCount => (int)Required()->GetUserSlotCount();

  public static bool TryGetUser(int slot, out XnaOsUser user) {
    user = default;
    fixed (XnaOsUser* p = &user) {
      return Required()->GetUser((uint)slot, p) == 0;
    }
  }

  // By value, not `in`: a fixed buffer in a local is already fixed and can be
  // read straight through, which a readonly reference would not allow.
  public static string GetGamertag(XnaOsUser user) => Utf8(user.GamertagUtf8, 16);

  // ---- title --------------------------------------------------------------

  public static uint TitleId => Required()->GetTitleId();

  public static string TitleName => CallStringOut(&CallTitleName);

  // ---- storage ------------------------------------------------------------

  public static string StorageDeviceName => CallStringOut(&CallDeviceName);

  public static ulong StorageTotalSpace => Required()->GetStorageTotalSpace();
  public static ulong StorageFreeSpace => Required()->GetStorageFreeSpace();

  /// <summary>
  /// The host directory backing a StorageContainer, created if missing.
  /// Nexia owns where saves live; the title just gets a path it can open.
  /// </summary>
  public static string ResolveStorageContainer(int slot, string displayName) {
    var t = Required();
    byte[] nameBytes = ToUtf8(displayName);
    fixed (byte* namePtr = nameBytes) {
      uint needed = t->ResolveStorageContainer((uint)slot, namePtr, null, 0);
      if (needed == 0) return null;
      byte[] buffer = new byte[needed];
      fixed (byte* bufferPtr = buffer) {
        if (t->ResolveStorageContainer((uint)slot, namePtr, bufferPtr, needed) == 0) {
          return null;
        }
        return Utf8(bufferPtr, (int)needed);
      }
    }
  }

  // ---- guide --------------------------------------------------------------

  public static bool GuideIsVisible => Required()->GuideIsVisible() != 0;

  /// <summary>
  /// Puts up Nexia's message box. Returns a request id to poll, or 0 if the
  /// dialog could not be shown. Every non-zero id must be released.
  /// </summary>
  public static uint BeginMessageBox(int slot, string title, string text,
                                     string[] buttons, int focusButton) {
    var t = Required();
    byte[][] buttonBytes = new byte[buttons?.Length ?? 0][];
    for (int i = 0; i < buttonBytes.Length; i++) buttonBytes[i] = ToUtf8(buttons[i]);

    // Each button string has to stay pinned for the duration of the call, and
    // `fixed` cannot pin a variable number of them - so they are pinned by
    // hand and released in the finally.
    var handles = new GCHandle[buttonBytes.Length];
    byte*[] pointers = new byte*[buttonBytes.Length];
    try {
      for (int i = 0; i < buttonBytes.Length; i++) {
        handles[i] = GCHandle.Alloc(buttonBytes[i], GCHandleType.Pinned);
        pointers[i] = (byte*)handles[i].AddrOfPinnedObject();
      }
      byte[] titleBytes = ToUtf8(title);
      byte[] textBytes = ToUtf8(text);
      fixed (byte* titlePtr = titleBytes)
      fixed (byte* textPtr = textBytes)
      fixed (byte** buttonPtr = pointers) {
        return t->GuideBeginMessageBox((uint)slot, titlePtr, textPtr, buttonPtr,
                                       (uint)pointers.Length, (uint)Math.Max(focusButton, 0));
      }
    } finally {
      for (int i = 0; i < handles.Length; i++) {
        if (handles[i].IsAllocated) handles[i].Free();
      }
    }
  }

  public static uint BeginKeyboard(int slot, string title, string description,
                                   string defaultText, int maxLength) {
    var t = Required();
    byte[] titleBytes = ToUtf8(title);
    byte[] descriptionBytes = ToUtf8(description);
    byte[] defaultBytes = ToUtf8(defaultText);
    fixed (byte* titlePtr = titleBytes)
    fixed (byte* descriptionPtr = descriptionBytes)
    fixed (byte* defaultPtr = defaultBytes) {
      return t->GuideBeginKeyboard((uint)slot, titlePtr, descriptionPtr, defaultPtr,
                                   (uint)Math.Max(maxLength, 0));
    }
  }

  public static XnaOsGuideStatus PollGuide(uint request, out int button) {
    int chosen = -1;
    uint textBytes = 0;
    int status = Required()->GuidePoll(request, &chosen, &textBytes);
    button = chosen;
    return (XnaOsGuideStatus)status;
  }

  public static string GetGuideText(uint request) {
    var t = Required();
    uint needed = t->GuideGetText(request, null, 0);
    if (needed <= 1) return string.Empty;
    byte[] buffer = new byte[needed];
    fixed (byte* p = buffer) {
      t->GuideGetText(request, p, needed);
      return Utf8(p, (int)needed);
    }
  }

  public static void ReleaseGuide(uint request) => Required()->GuideRelease(request);

  // ---- logging ------------------------------------------------------------

  public enum Level : uint { Debug = 0, Info = 1, Warning = 2, Error = 3 }

  /// <summary>
  /// Writes to the emulator's log. Managed code has nowhere else to go: the
  /// emulator is a windowed process, so Console output is discarded.
  /// </summary>
  public static void Log(Level level, string message) {
    var t = table;
    if (t == null || t->Log == null) {
      return;
    }
    byte[] bytes = ToUtf8(message);
    fixed (byte* p = bytes) {
      t->Log((uint)level, p);
    }
  }

  public static void Log(string message) => Log(Level.Info, message);

  /// <summary>
  /// Sends Console.Out and Console.Error to the log as well, so a title's own
  /// printing and any unhandled stack trace are not lost.
  /// </summary>
  public static void CaptureConsole() {
    Console.SetOut(new LogWriter(Level.Info));
    Console.SetError(new LogWriter(Level.Error));
  }

  private sealed class LogWriter : System.IO.TextWriter {
    private readonly Level level;
    private readonly System.Text.StringBuilder pending = new();

    internal LogWriter(Level level) { this.level = level; }

    public override System.Text.Encoding Encoding => System.Text.Encoding.UTF8;

    // Buffered to a line: the log takes whole messages, and Write(char) is how
    // most of the framework emits text.
    public override void Write(char value) {
      if (value == '\n') {
        Flush();
        return;
      }
      if (value != '\r') {
        pending.Append(value);
      }
    }

    public override void Write(string value) {
      if (value == null) return;
      foreach (char c in value) Write(c);
    }

    public override void WriteLine(string value) {
      Write(value);
      Flush();
    }

    public override void Flush() {
      if (pending.Length == 0) return;
      Log(level, pending.ToString());
      pending.Clear();
    }
  }

  // ---- plumbing -----------------------------------------------------------

  private static uint CallTitleName(byte* buffer, uint capacity) =>
      Required()->GetTitleName(buffer, capacity);

  private static uint CallDeviceName(byte* buffer, uint capacity) =>
      Required()->GetStorageDeviceName(buffer, capacity);

  // Every string getter is "ask for the size, then ask for the bytes".
  private static string CallStringOut(delegate*<byte*, uint, uint> get) {
    uint needed = get(null, 0);
    if (needed <= 1) return string.Empty;
    byte[] buffer = new byte[needed];
    fixed (byte* p = buffer) {
      get(p, needed);
      return Utf8(p, (int)needed);
    }
  }

  private static string Utf8(byte* p, int capacity) {
    int length = 0;
    while (length < capacity && p[length] != 0) length++;
    return System.Text.Encoding.UTF8.GetString(p, length);
  }

  private static byte[] ToUtf8(string value) {
    byte[] encoded = System.Text.Encoding.UTF8.GetBytes(value ?? string.Empty);
    byte[] terminated = new byte[encoded.Length + 1];
    Buffer.BlockCopy(encoded, 0, terminated, 0, encoded.Length);
    return terminated;
  }
}
