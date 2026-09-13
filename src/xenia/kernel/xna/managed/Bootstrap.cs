// The first managed code Nexia runs when it hosts an XNA title.
//
// hostfxr binds Start directly - it is [UnmanagedCallersOnly], so the OS table
// arrives as a raw pointer with no marshalling and no delegate to keep alive.
// Start must return promptly: it is called on the thread that asked to launch
// the title, so the game's own loop goes on a thread of its own.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Linq;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Threading;

namespace Nexia.Xna;

/// <summary>
/// Resolves a title's assemblies against its own folder first, then against a
/// single shared overlay.
/// </summary>
/// <remarks>
/// The overlay is the directory this assembly was published to - it holds the
/// host and MonoGame, one copy for every title. The XNA framework assemblies
/// are not there at all: they are fabricated on demand (see FacadeFactory).
/// Unpacked titles get only what was in their package.
/// </remarks>
internal sealed class TitleLoadContext : AssemblyLoadContext {
  private readonly string overlayDir;
  private readonly Dictionary<string, Assembly> facades =
      new(StringComparer.Ordinal);
  private Assembly monoGame;
  private readonly HashSet<string> misses = new(StringComparer.Ordinal);

  internal TitleLoadContext(string titleDir) : base("xna-title") {

    this.overlayDir = Bootstrap.OverlayDir;
    Resolving += Resolve;
  }

  private Assembly Resolve(AssemblyLoadContext context, AssemblyName name) {
    // An XNA framework assembly is never loaded from disk. It is fabricated
    // here, forwarding into whatever MonoGame and host are present - so there
    // is nothing to install and nothing that can fall out of step with them.
    // A copy inside the package would be the Compact Framework build anyway,
    // which cannot load on the desktop runtime.
    if (FacadeFactory.IsFacade(name.Name)) {
      if (facades.TryGetValue(name.Name, out var built)) {
        return built;
      }

      // The console's own assembly, when it is installed. It understands the
      // title's 360 content natively - LZX .xnb, tiled textures, Xenos
      // shaders - and reaches the hardware through P/Invokes that
      // NativeRewriter routes into Nexia. MonoGame cannot read any of that.
      var console = ConsoleAssemblies.Stage(name.Name);
      if (console != null) {
        Prepare(console);
        var loaded = LoadFromAssemblyPath(console);
        BindConsoleModules(loaded);
        facades[name.Name] = loaded;
        XnaOs.Log($"   console {name.Name,-37} <- {Path.GetFileName(console)}");
        return loaded;
      }

      var providers = new List<Assembly> { typeof(FacadeFactory).Assembly };
      var framework = LoadMonoGame();
      if (framework == null) {
        // Without MonoGame the facade would be built from the host alone and
        // come out nearly empty - the title then dies on a TypeLoadException
        // naming a type that was never going to be there. Say the real reason.
        throw new InvalidOperationException(
            "MonoGame is not installed in " + overlayDir +
            " - use File > XNA Titles > Install Dependency Package...");
      }
      providers.Add(framework);
      using var image = FacadeFactory.Build(name.Name, providers);
      built = LoadFromStream(image);
      facades[name.Name] = built;
      XnaOs.Log($"   made {name.Name,-40} ({providers.Count} providers)");
      return built;
    }

    // THE TITLE'S OWN CODE COMES OUT OF THE PACKAGE, NOT OFF DISK.
    //
    // Nothing is unpacked: the STFS container is the title's filesystem, so an
    // assembly is read from it, rewritten in memory and loaded from the stream.
    // An extraction beside the package is a second copy that can disagree with
    // it, and the launcher's "reuse" branch served a three day old one.
    lock (misses) {
      if (misses.Contains(name.FullName)) {
        return null;
      }
    }
    var fromPackage =
        PackageAssemblies.Find(name.Name, name.CultureName, out var file);
    if (fromPackage != null) {
      XnaOs.Log($"   load {name.Name,-40} <- the package ({file})");
      return LoadFromStream(PrepareImage(fromPackage, file));
    }
    if (PackageAssemblies.IsOptional(name.Name)) {
      lock (misses) {
        misses.Add(name.FullName);
      }
      XnaOs.Log(XnaOs.Level.Debug,
                $"   no {name.FullName} in the package - optional, the " +
                "runtime falls back");
      return null;
    }

    var path = Path.Combine(overlayDir, name.Name + ".dll");
    if (!File.Exists(path)) {
      // Last resort, and only reached because the default context already
      // failed - which for mscorlib and System it never does. What this
      // actually covers is the Compact Framework libraries the desktop
      // genuinely lacks.
      var consoleCore = ConsoleAssemblies.Stage(name.Name);
      if (consoleCore != null) {
        XnaOs.Log(XnaOs.Level.Warning,
                  $"   console BCL {name.Name} - the desktop runtime could not "
                  + "supply it");
        Prepare(consoleCore);
        return LoadFromAssemblyPath(consoleCore);
      }
    }
    if (!File.Exists(path)) {
      // XmlSerializer probes for a pre-generated "<assembly>.XmlSerializers"
      // and falls back to generating at run time when it is absent. Every XNA
      // title triggers it, and it is not a problem - saying so at warning
      // level would just be a red herring in the log.
      var level = name.Name.EndsWith(".XmlSerializers", StringComparison.Ordinal)
          ? XnaOs.Level.Debug
          : XnaOs.Level.Warning;
      lock (misses) {
        misses.Add(name.FullName);
      }
      XnaOs.Log(level, $"   MISS {name.Name} v{name.Version}");
      return null;
    }
    XnaOs.Log($"   load {name.Name,-40} <- {Path.GetDirectoryName(path)}");
    Prepare(path);
    return LoadFromAssemblyPath(path);
  }

  /// <summary>
  /// Makes a title's assembly loadable on the desktop runtime, in place.
  /// </summary>
  /// <remarks>
  /// Order matters: the Compact Framework rewrite goes through Cecil, which
  /// re-emits the PE header, so the 32BITREQUIRED bit has to be cleared after
  /// it rather than before.
  /// </remarks>
  internal static void Prepare(string path) {
    CompactFramework.Rewrite(path);
    AssemblyFlags.MakeLoadable(path);
  }

  /// <summary>
  /// The same preparation, on bytes from the package instead of a file. Returns
  /// a stream over the rewritten image.
  /// </summary>
  internal static Stream PrepareImage(byte[] image, string name) {
    var rewritten = CompactFramework.RewriteImage(image, name);
    return new MemoryStream(AssemblyFlags.MakeLoadableImage(rewritten ?? image));
  }

  // Console modules do not exist as libraries anywhere: D3D, XAM, XINPUT and
  // the rest are names an Xbox resolved internally. The emulator exports those
  // entry points itself, so every P/Invoke in a console assembly is pointed at
  // the running program and the runtime resolves each name there.
  //
  // This is what DllImportResolver is for. It chooses a MODULE and the runtime
  // looks the entry point up inside it - which is exactly right once the module
  // is us.
  private static readonly HashSet<string> ConsoleModules =
      new(StringComparer.OrdinalIgnoreCase) {
        "D3D", "XAM", "XINPUT", "STORAGE", "Net", "AUDIO", "MEDIA", "SYSTEM",
      };

  private static void BindConsoleModules(Assembly assembly) {
    if (assembly == null) {
      return;
    }
    try {
      NativeLibrary.SetDllImportResolver(assembly, (name, asm, path) =>
          ConsoleModules.Contains(name) ? NativeLibrary.GetMainProgramHandle()
                                        : IntPtr.Zero);
    } catch (InvalidOperationException) {
      // Already bound - one resolver per assembly is all the runtime allows.
    }
  }

  // Resolved through the DEFAULT context, not this one. This assembly is loaded
  // there by hostfxr and is compiled against MonoGame, so its own code - the
  // Storage and GamerServices implementations - resolves MonoGame there too.
  // Loading a second copy here would give the forwarders a different instance
  // than the host uses, and every type would silently split in two.
  private Assembly LoadMonoGame() {
    if (monoGame != null) {
      return monoGame;
    }
    try {
      monoGame = Assembly.Load(new AssemblyName("MonoGame.Framework"));
    } catch (Exception) {
      return null;
    }
    return monoGame;
  }
}

public static unsafe class Bootstrap {
  private static Thread gameThread;
  // The context the title and the console assemblies are loaded into. Anything
  // reaching for a console type has to go through THIS, not Type.GetType: the
  // console runtime is not in the default load context, and a lookup there
  // quietly finds nothing (or a second, unrelated copy).
  private static TitleLoadContext titleContext;

  /// <summary>Where the host, Cecil and MonoGame live.</summary>
  internal static string OverlayDir {
    get {
      var directory = Path.GetDirectoryName(typeof(Bootstrap).Assembly.Location);
      // AppContext.BaseDirectory is EMPTY under hostfxr - the runtime was
      // started from a runtimeconfig, not an app, so there is no app base.
      return string.IsNullOrEmpty(directory) ? AppContext.BaseDirectory ?? string.Empty
                                             : directory;
    }
  }

  // Anything beside this assembly is resolvable from the default context, so
  // the host and the title share one instance of it.
  private static Assembly ResolveFromOverlay(AssemblyLoadContext context,
                                             AssemblyName name) {
    var path = Path.Combine(OverlayDir, name.Name + ".dll");
    return File.Exists(path) ? context.LoadFromAssemblyPath(path) : null;
  }

  [UnmanagedCallersOnly]
  public static int Start(XnaOsTable* osTable, byte* gamePathUtf8) {
    try {
      if (osTable == null) return 1;
      if (osTable->Version < 1) return 2;
      XnaOs.Bind(osTable);
      // Anything the title prints, and any stack trace the runtime writes,
      // would otherwise be discarded - this process has no console.
      XnaOs.CaptureConsole();
      AppDomain.CurrentDomain.FirstChanceException -= LogFirstChance;
      AppDomain.CurrentDomain.FirstChanceException += LogFirstChance;
      AssemblyLoadContext.Default.Resolving -= ResolveFromOverlay;
      AssemblyLoadContext.Default.Resolving += ResolveFromOverlay;

      // A path INSIDE the package now, not on disk. Nothing is unpacked, so
      // its presence is checked by asking the package for it.
      string gamePath = Marshal.PtrToStringUTF8((IntPtr)gamePathUtf8);
      if (string.IsNullOrEmpty(gamePath)) return 3;
      if (NativeCalls.ReadTitleFile(gamePath) == null) {
        XnaOs.Log(XnaOs.Level.Error,
                  $"the package holds no {gamePath}");
        return 4;
      }

      XnaOs.Log($"title {XnaOs.TitleId:X8} \"{XnaOs.TitleName}\"");
      for (int slot = 0; slot < XnaOs.UserSlotCount; slot++) {
        if (!XnaOs.TryGetUser(slot, out var user)) continue;
        if ((XnaOsSigninState)user.SigninState == XnaOsSigninState.NotSignedIn) continue;
        XnaOs.Log($"   slot {slot}: {XnaOs.GetGamertag(user)} " +
                          $"({user.Xuid:X16}, {(XnaOsSigninState)user.SigninState})");
      }

      // A game loop owns its thread for the life of the title, so it cannot run
      // on the caller's - the emulator would never get control back.
      gameThread = new Thread(() => RunTitle(gamePath)) {
        Name = "XNA Title",
        IsBackground = true,
      };
      gameThread.Start();
      StartWatchdog();
      StartGamerServicesPump();
      return 0;
    } catch (Exception e) {
      // Letting an exception cross back into C++ would tear down the emulator,
      // so the failure is reported as a status instead.
      XnaOs.Log(XnaOs.Level.Error, $"bootstrap failed: {e}");
      return -1;
    }
  }

  // A title's engine swallows exceptions in dozens of places, so a failure
  // shows up as a hang or a wrong picture rather than a stack trace. First
  // chance catches them at the throw, before anyone can hide them. Deduped by
  // type, message and top frame - a per-frame failure would otherwise fill the
  // log with one line per draw.
  private static readonly HashSet<string> seenThrows =
      new(StringComparer.Ordinal);

  private static void LogFirstChance(object sender,
                                     FirstChanceExceptionEventArgs e) {
    if (e.Exception is FileNotFoundException missing &&
        PackageAssemblies.IsOptional(
            missing.FileName?.Split(',')[0].Trim())) {
      return;
    }
    string frame = (e.Exception.StackTrace ?? string.Empty)
                       .Split('\n').FirstOrDefault()?.Trim() ?? string.Empty;
    string key = e.Exception.GetType().FullName + "|" + e.Exception.Message +
                 "|" + frame;
    lock (seenThrows) {
      if (!seenThrows.Add(key)) {
        return;
      }
    }
    XnaOs.Log(XnaOs.Level.Warning,
              $"   [throw] {e.Exception.GetType().Name}: {e.Exception.Message}");
    if (frame.Length > 0) {
      XnaOs.Log(XnaOs.Level.Warning, $"           {frame}");
    }
  }

  /// <summary>
  /// Reports what the title thread is doing, every few seconds.
  /// </summary>
  /// <remarks>
  /// A hosted title that stops producing log lines is otherwise unreadable: it
  /// could be running its loop, blocked, or spinning, and they look identical
  /// from outside. Thread state separates blocked from running, and the native
  /// call count says whether it is reaching the console at all - a count that
  /// stays at zero means it never got past managed initialisation.
  /// </remarks>
  /// <summary>
  /// Drives GamerServicesDispatcher.Update so sign-in events actually arrive.
  /// </summary>
  /// <remarks>
  /// GamerServicesComponent.Update calls GamerServicesDispatcher.Update with no
  /// condition on it, and Arcadecraft adds that component to Game.Components and
  /// calls base.Update - yet the native side has NEVER seen the Update command
  /// (6) in any run, only the one init command at startup. Nothing else pumps
  /// the dispatcher, and everything downstream of it is stuck as a result:
  ///
  ///   * Gamer.SignedInGamers stays empty, so OnlineManager.Update calls
  ///     Guide.ShowSignIn and returns forever - mSignInRequired never clears
  ///     and the press-start transition returns at its first line every frame.
  ///   * ShowSignIn sets Guide.forceGuideVisible, and the ONLY thing that ever
  ///     clears it is the flags word of the dispatcher's reply. With no reply
  ///     the guide is up permanently, so GuideIsUp_PauseGame runs
  ///     StopAllAudioExceptMusic - stopping CustomMachines, Default and
  ///     AmbientLoops, which is exactly the 4, 1, 3 seen in the log, and which
  ///     kills the InsertCoin cue a few mixer frames after it starts.
  ///
  /// Calling Update here is the same call the component makes, so a run where
  /// the component does work loses nothing: the extra call reports "no events"
  /// and clears the flags exactly as it would anyway.
  /// </remarks>
  private static void StartGamerServicesPump() {
    var pump = new Thread(() => {
      MethodInfo update = null;
      PropertyInfo initialized = null;
      bool announced = false;
      while (gameThread != null && gameThread.IsAlive) {
        Thread.Sleep(16);
        try {
          if (update == null) {
            // Through the TITLE'S context. Type.GetType looks in the default
            // one, where the console runtime is not loaded - the same per-ALC
            // trap that made the injected logger a silent no-op.
            if (titleContext == null) {
              continue;
            }
            Type type = null;
            foreach (var assembly in titleContext.Assemblies) {
              if (assembly.GetName().Name !=
                  "Microsoft.Xna.Framework.GamerServices") {
                continue;
              }
              type = assembly.GetType(
                  "Microsoft.Xna.Framework.GamerServices.GamerServicesDispatcher",
                  false);
              break;
            }
            if (type == null) {
              continue;
            }
            update = type.GetMethod("Update",
                                    BindingFlags.Public | BindingFlags.Static);
            initialized = type.GetProperty(
                "IsInitialized", BindingFlags.Public | BindingFlags.Static);
          }
          // Update throws if the dispatcher has not been initialised yet, and
          // the title initialises it from its own Initialize().
          if (initialized != null &&
              !(bool)initialized.GetValue(null)) {
            continue;
          }
          update?.Invoke(null, null);
          if (!announced) {
            announced = true;
            XnaOs.Log("pumping GamerServicesDispatcher.Update");
          }
        } catch (Exception e) {
          // One report, then keep trying - a transient failure here must not
          // take the title down with it.
          if (!announced) {
            announced = true;
            XnaOs.Log(XnaOs.Level.Warning,
                      $"GamerServices pump: {e.InnerException?.Message ?? e.Message}");
          }
        }
      }
    }) {
      Name = "XNA GamerServices Pump",
      IsBackground = true,
    };
    pump.Start();
  }

  private static void StartWatchdog() {
    var watchdog = new Thread(() => {
      int quiet = 0;
      while (gameThread != null && gameThread.IsAlive) {
        Thread.Sleep(5000);
        if (gameThread == null || !gameThread.IsAlive) {
          break;
        }
        quiet++;
        XnaOs.Log($"still running after {quiet * 5}s: thread is " +
                  $"{gameThread.ThreadState}, {NativeCalls.Count} native " +
                  "call(s) so far");
      }
    }) {
      Name = "XNA Watchdog",
      IsBackground = true,
    };
    watchdog.Start();
  }

  // What the title actually asked the console for, gathered on the native side.
  //
  // Every P/Invoke in the console assemblies now binds straight to the running
  // program, so the counting happens in C++ and the managed side has nothing of
  // its own to report. This is read at the two points where a title is over -
  // it returned, or it threw - and is the best guide there is to which console
  // entry point to implement next.
  private delegate IntPtr ExportUsageFn();

  private static string ExportUsage() {
    try {
      var handle = NativeLibrary.GetMainProgramHandle();
      if (!NativeLibrary.TryGetExport(handle, "Nexia_XnaExportUsage",
                                      out IntPtr address)) {
        return "(the host exports no usage report)";
      }
      var read = Marshal.GetDelegateForFunctionPointer<ExportUsageFn>(address);
      return Marshal.PtrToStringUTF8(read()) ?? string.Empty;
    } catch (Exception e) {
      return "(could not read the usage report: " + e.Message + ")";
    }
  }

  private static void RunTitle(string gamePath) {
    try {
      string titleDir = Path.GetDirectoryName(Path.GetFullPath(gamePath));
      XnaOs.Log($"loading {gamePath}");
      XnaOs.Log($"overlay {OverlayDir}");
      XnaOs.Log(ConsoleAssemblies.Available
                    ? $"graphics via the console runtime in {ConsoleAssemblies.Directory}"
                    : "graphics via MonoGame - the console runtime is not "
                      + "installed, so 360 content cannot be read");

      var context = new TitleLoadContext(titleDir);
      titleContext = context;

      // OUT OF THE PACKAGE. Content paths in an XNA title are relative
      // ("Content\\Textures\\..."), and relative now means relative to the game
      // folder inside the STFS container - TitleContainer.OpenStream is
      // redirected there, so nothing needs a working directory on disk.
      var image = NativeCalls.ReadTitleFile(gamePath);
      if (image == null) {
        XnaOs.Log(XnaOs.Level.Error, $"the package holds no {gamePath}");
        return;
      }
      Assembly game = context.LoadFromStream(
          TitleLoadContext.PrepareImage(image, gamePath));

      MethodInfo entry = game.EntryPoint;
      if (entry == null) {
        XnaOs.Log(XnaOs.Level.Error, "the title has no entry point");
        return;
      }

      // An XNA Main is `static void Main(string[])`, but a few are parameterless
      // - passing arguments to one that takes none throws before the game ever
      // starts.
      object[] arguments =
          entry.GetParameters().Length == 0 ? null : new object[] { Array.Empty<string>() };
      // Logged before the call, so a title that never returns can still be
      // distinguished from one that never started.
      XnaOs.Log($"invoking {entry.DeclaringType?.FullName}.{entry.Name}");
      entry.Invoke(null, arguments);
      XnaOs.Log("title returned");
      XnaOs.Log(ExportUsage());
    } catch (TargetInvocationException e) {
      // The useful exception is always the inner one; the wrapper says nothing.
      XnaOs.Log(XnaOs.Level.Error, $"title failed: {e.InnerException}");
      // What the title got through before it died is the list worth reading.
      XnaOs.Log(ExportUsage());
    } catch (Exception e) {
      XnaOs.Log(XnaOs.Level.Error, $"title failed: {e}");
    }
  }
}
