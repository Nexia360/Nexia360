// Effect blobs, prepared before the console runtime reads them.
//
// A compiled console effect was authored for a big-endian PowerPC, and
// Effect..ctor checks its magic with a native-order 32-bit load. That code now
// runs little-endian on x64, so the first word it reads is byte-reversed and it
// rejects every effect with "Creating an effect requires that the shader code
// has been compiled" before anything else gets a look.
//
// So Effect..ctor is patched to call through here first, and the native side
// validates the container and swaps it into host order in place - the same
// thing the XEX loader does to an image before trusting its contents.
//
// The array is pinned for the call: the native side writes back into the very
// bytes the managed reader is about to parse, so a collection moving it
// mid-call would corrupt the effect.
using System;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Nexia.Xna;

public static class EffectImages {
  private delegate uint PrepareFn(IntPtr data, uint size);
  private delegate void LogFn(uint level, IntPtr message);

  private static PrepareFn prepare;
  private static LogFn log;
  private static bool resolved;

  // Deliberately not the shared logger. This class is called from IL injected
  // into MXF.Graphics, which the CLR loads into the title's
  // AssemblyLoadContext - so it gets its own copy of every static in this
  // assembly, including the function table the shared logger dispatches
  // through. That copy is never bound, so the call returns at its null check
  // and the message is silently dropped: the diagnostic reads as "the code did
  // not run" when the code ran fine. Resolving the export off the running
  // program works from any load context, which is why Prepare survives the
  // same trip.
  private static void Say(uint level, string message) {
    Resolve();
    if (log == null) {
      return;
    }
    // NUL-terminated explicitly: the native side takes a C string.
    var utf8 = System.Text.Encoding.UTF8.GetBytes(message);
    var bytes = new byte[utf8.Length + 1];
    Array.Copy(utf8, bytes, utf8.Length);
    var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
    try {
      log(level, pin.AddrOfPinnedObject());
    } finally {
      pin.Free();
    }
  }

  /// <summary>
  /// Called from the top of Effect..ctor with the effect bytes. Returns the
  /// same array so the patched call site can leave it on the stack.
  /// </summary>
  public static byte[] Prepare(byte[] effectCode) {
    if (effectCode == null || effectCode.Length < 8) {
      return effectCode;
    }
    Resolve();
    if (prepare == null) {
      return effectCode;
    }
    var pin = GCHandle.Alloc(effectCode, GCHandleType.Pinned);
    try {
      prepare(pin.AddrOfPinnedObject(), (uint)effectCode.Length);
    } catch (Exception e) {
      Say(3, $"   could not prepare an effect: {e.Message}");
    } finally {
      pin.Free();
    }
    return effectCode;
  }

  private static void Resolve() {
    if (resolved) {
      return;
    }
    resolved = true;
    try {
      var handle = NativeLibrary.GetMainProgramHandle();
      if (NativeLibrary.TryGetExport(handle, "Nexia_XnaPrepareEffect",
                                     out IntPtr address)) {
        prepare = Marshal.GetDelegateForFunctionPointer<PrepareFn>(address);
      }
      if (NativeLibrary.TryGetExport(handle, "Nexia_XnaLog",
                                     out IntPtr logAddress)) {
        log = Marshal.GetDelegateForFunctionPointer<LogFn>(logAddress);
      }
    } catch (Exception) {
      // Left null; effects then reach the console runtime untouched and fail
      // the way they did before, which is at least the familiar failure.
    }
  }
}
