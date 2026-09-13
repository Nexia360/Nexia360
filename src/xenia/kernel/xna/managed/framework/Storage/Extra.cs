// Storage, answered by Nexia rather than borrowed from MonoGame.
//
// MonoGame's StorageDevice saves to a desktop folder of its own choosing. On a
// console the save belongs to a profile and the system decides where it lives,
// so this asks Nexia (XnaOs.ResolveStorageContainer) and works inside whatever
// directory it hands back. Everything below that is ordinary System.IO: a
// StorageContainer IS a directory of files, and the title only ever sees
// streams.
//
// Signatures come from the console's own MXF.Storage.dlx - see Stubs.cs for
// the members not implemented here, and Stubs.skip for the ones that are.
using System;
using System.IO;
using System.Threading;
using Nexia.Xna;

namespace Microsoft.Xna.Framework.Storage {
  // XNA's Begin/End pattern, completed before Begin even returns. There is
  // nothing to wait for: resolving a directory is not slow, and a title that
  // polls IsCompleted or blocks on AsyncWaitHandle sees a finished operation
  // either way.
  internal sealed class SyncAsyncResult : IAsyncResult, IDisposable {
    private ManualResetEvent handle;

    internal SyncAsyncResult(object result, object state) {
      Result = result;
      AsyncState = state;
    }

    internal object Result { get; }
    public object AsyncState { get; }
    public bool CompletedSynchronously => true;
    public bool IsCompleted => true;

    public WaitHandle AsyncWaitHandle {
      get {
        // Only allocated if someone actually waits, which most titles never do.
        if (handle == null) {
          Interlocked.CompareExchange(ref handle, new ManualResetEvent(true), null);
        }
        return handle;
      }
    }

    internal SyncAsyncResult Complete(AsyncCallback callback) {
      callback?.Invoke(this);
      return this;
    }

    public void Dispose() => handle?.Dispose();
  }

  public sealed partial class StorageDevice {
    // Which profile's storage this device represents. XNA lets a title ask
    // without naming a player, which on a console means "the signed-in one" -
    // slot 0 is the honest reading of that.
    internal int Slot;

    internal StorageDevice(int slot) { Slot = slot; }

    public long FreeSpace =>
        XnaOs.IsAvailable ? (long)Math.Min(XnaOs.StorageFreeSpace, long.MaxValue) : 0;

    public long TotalSpace =>
        XnaOs.IsAvailable ? (long)Math.Min(XnaOs.StorageTotalSpace, long.MaxValue) : 0;

    // The hard drive is not removable, so it is connected whenever Nexia is
    // there to answer at all.
    public bool IsConnected => XnaOs.IsAvailable;

    public static IAsyncResult BeginShowSelector(AsyncCallback callback, object state) =>
        BeginShowSelector(PlayerIndex.One, callback, state);

    public static IAsyncResult BeginShowSelector(int sizeInBytes, int directoryCount,
                                                 AsyncCallback callback, object state) =>
        BeginShowSelector(PlayerIndex.One, callback, state);

    public static IAsyncResult BeginShowSelector(PlayerIndex player, int sizeInBytes,
                                                 int directoryCount,
                                                 AsyncCallback callback, object state) =>
        BeginShowSelector(player, callback, state);

    // There is exactly one device, so there is nothing to select between and no
    // reason to put a picker in front of the user.
    public static IAsyncResult BeginShowSelector(PlayerIndex player,
                                                 AsyncCallback callback, object state) =>
        new SyncAsyncResult(new StorageDevice((int)player), state).Complete(callback);

    public static StorageDevice EndShowSelector(IAsyncResult result) =>
        (result as SyncAsyncResult)?.Result as StorageDevice;

    public IAsyncResult BeginOpenContainer(string displayName, AsyncCallback callback,
                                           object state) =>
        new SyncAsyncResult(new StorageContainer(this, displayName), state)
            .Complete(callback);

    public StorageContainer EndOpenContainer(IAsyncResult result) =>
        (result as SyncAsyncResult)?.Result as StorageContainer;

    public void DeleteContainer(string titleName) {
      string root = XnaOs.IsAvailable
          ? XnaOs.ResolveStorageContainer(Slot, titleName) : null;
      if (root == null) return;
      try {
        Directory.Delete(root, true);
      } catch (DirectoryNotFoundException) {
        // Deleting a container that was never created is not an error.
      }
    }
  }

  public partial class StorageContainer {
    private readonly string root;

    internal StorageContainer(StorageDevice device, string displayName) {
      StorageDevice = device;
      DisplayName = displayName;
      root = XnaOs.IsAvailable
          ? XnaOs.ResolveStorageContainer(device.Slot, displayName) : null;
    }

    public string DisplayName { get; }
    public StorageDevice StorageDevice { get; }
    public bool IsDisposed { get; private set; }

    // Field-like, not generated: a stubbed event has empty add/remove and
    // throws away every handler the title attaches.
    public event EventHandler<EventArgs> Disposing;

    // Every path a title gives is relative to its container, and must stay
    // inside it: "../../someone else" is not a filename.
    private string Resolve(string relative) {
      if (root == null) {
        throw new StorageDeviceNotConnectedException(
            "Nexia is not providing storage.");
      }
      string full = Path.GetFullPath(Path.Combine(root, relative ?? string.Empty));
      string prefix = Path.GetFullPath(root);
      if (!full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) {
        throw new ArgumentException("path leaves the container", nameof(relative));
      }
      return full;
    }

    private void EnsureParent(string full) {
      string directory = Path.GetDirectoryName(full);
      if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
    }

    public bool DirectoryExists(string directory) => Directory.Exists(Resolve(directory));
    public bool FileExists(string file) => File.Exists(Resolve(file));

    public void CreateDirectory(string directory) => Directory.CreateDirectory(Resolve(directory));
    public void DeleteDirectory(string directory) => Directory.Delete(Resolve(directory), true);
    public void DeleteFile(string file) => File.Delete(Resolve(file));

    public Stream CreateFile(string file) {
      string full = Resolve(file);
      EnsureParent(full);
      return File.Create(full);
    }

    public Stream OpenFile(string file, FileMode fileMode) =>
        OpenFile(file, fileMode, FileAccess.ReadWrite, FileShare.None);

    public Stream OpenFile(string file, FileMode fileMode, FileAccess fileAccess) =>
        OpenFile(file, fileMode, fileAccess, FileShare.None);

    public Stream OpenFile(string file, FileMode fileMode, FileAccess fileAccess,
                           FileShare fileShare) {
      string full = Resolve(file);
      // A mode that can create the file has to be able to create the directory
      // holding it too, or a title that saves into a subfolder it never
      // explicitly made fails on a fresh profile.
      if (fileMode != FileMode.Open && fileMode != FileMode.Truncate) {
        EnsureParent(full);
      }
      return new FileStream(full, fileMode, fileAccess, fileShare);
    }

    public string[] GetDirectoryNames() => GetDirectoryNames("*");

    public string[] GetDirectoryNames(string searchPattern) =>
        Names(Directory.Exists(Resolve(string.Empty))
                  ? Directory.GetDirectories(Resolve(string.Empty), searchPattern ?? "*")
                  : Array.Empty<string>());

    public string[] GetFileNames() => GetFileNames("*");

    public string[] GetFileNames(string searchPattern) =>
        Names(Directory.Exists(Resolve(string.Empty))
                  ? Directory.GetFiles(Resolve(string.Empty), searchPattern ?? "*")
                  : Array.Empty<string>());

    // XNA hands back names, not paths - a title passes what it gets straight
    // back to OpenFile.
    private static string[] Names(string[] paths) {
      var names = new string[paths.Length];
      for (int i = 0; i < paths.Length; i++) names[i] = Path.GetFileName(paths[i]);
      return names;
    }

    public void Dispose() {
      if (IsDisposed) return;
      IsDisposed = true;
      Disposing?.Invoke(this, EventArgs.Empty);
    }
  }

  public partial class StorageDeviceNotConnectedException {
    public StorageDeviceNotConnectedException() { }
    public StorageDeviceNotConnectedException(string message) : base(message) { }
    public StorageDeviceNotConnectedException(string message, Exception innerException)
        : base(message, innerException) { }
  }
}
