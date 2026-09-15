// The console's own XNA assemblies, used in place of MonoGame.
//
// MonoGame cannot do anything useful with an Xbox 360 title's content: the
// .xnb are LZX-compressed and big-endian, the textures are GPU-tiled, and the
// shaders are Xenos microcode. Handing them to SDL/OpenGL produces a window
// that flickers and draws nothing, and no amount of debugging the game will
// change that.
//
// The console's Microsoft.Xna.Framework.Graphics already knows how to read all
// of it - that IS its native format - and it reaches the hardware through named
// P/Invokes (D3D_Texture2D_CopyData, D3D_Effect_CreateHandle,
// D3D_Device_ReceivePackets). NativeRewriter captures those, so loading the
// console assembly puts Nexia underneath the title's rendering instead of SDL.
//
// The files are the ones from the XNA Indie Player's title update, named
// MXF*.dlx. They are ordinary managed PE images despite the extension.
using System;
using System.Collections.Generic;
using System.IO;

namespace Nexia.Xna;

internal static class ConsoleAssemblies {
  // Assembly simple name -> file name in the console directory. The file names
  // are abbreviated on the console and do not match the assembly names, so the
  // mapping has to be explicit.
  private static readonly Dictionary<string, string> Files =
      new(StringComparer.Ordinal) {
        { "Microsoft.Xna.Framework", "MXF.dlx" },
        { "Microsoft.Xna.Framework.Avatar", "MXF.Avatar.dlx" },
        { "Microsoft.Xna.Framework.Game", "MXF.Game.dlx" },
        { "Microsoft.Xna.Framework.GamerServices", "MXF.GamerServices.dlx" },
        { "Microsoft.Xna.Framework.Graphics", "MXF.Graphics.dlx" },
        { "Microsoft.Xna.Framework.Input.Touch", "MXF.Input.Touch.dlx" },
        { "Microsoft.Xna.Framework.Net", "MXF.Net.dlx" },
        { "Microsoft.Xna.Framework.Storage", "MXF.Storage.dlx" },
        { "Microsoft.Xna.Framework.Video", "MXF.Video.dlx" },
        { "Microsoft.Xna.Framework.Xact", "MXF.Xact.dlx" },

        // The console's own BCL, shipped in the same title update. These are
        // only ever reached as a LAST resort: AssemblyLoadContext.Resolving
        // fires after the default context has already failed, and .NET
        // satisfies mscorlib and System from its own facades long before that.
        // So installing them is free, and they cover the Compact Framework
        // libraries the desktop genuinely does not have.
        //
        // If mscorlib ever IS served from here, expect trouble: a second
        // corelib means a second System.Object, and nothing would cross
        // between them. That is why serving any of these is logged.
        { "mscorlib", "mscorlib.dlx" },
        { "System", "System.dlx" },
        { "System.Core", "System.Core.dlx" },
        { "System.Xml", "System.xml.dlx" },
        { "System.Xml.Linq", "System.xml.linq.dlx" },
        { "System.SR", "System.sr.dlx" },
      };

  /// <summary>The console BCL, which is not part of the XNA framework.</summary>
  private static readonly HashSet<string> CoreLibraries =
      new(StringComparer.Ordinal) {
        "mscorlib", "System", "System.Core", "System.Xml", "System.Xml.Linq",
        "System.SR",
      };

  public static bool IsCoreLibrary(string simpleName) =>
      CoreLibraries.Contains(simpleName);

  /// <summary>Where the console runtime assemblies are installed.</summary>
  public static string Directory =>
      Path.Combine(Bootstrap.OverlayDir, "console");

  /// <summary>
  /// The console assembly for <paramref name="simpleName"/>, or null when it
  /// is not installed - in which case the caller falls back to fabricating a
  /// MonoGame facade.
  /// </summary>
  public static string Find(string simpleName) {
    if (!Files.TryGetValue(simpleName, out var file)) {
      return null;
    }
    var path = Path.Combine(Directory, file);
    return File.Exists(path) ? path : null;
  }

  /// <summary>
  /// A working copy of the console assembly, safe to rewrite in place.
  /// </summary>
  /// <remarks>
  /// The installed files are treated as read-only masters and never modified.
  /// A rewrite that goes wrong is not always recoverable by rewriting again -
  /// stripping a P/Invoke is one-way, so a half-converted method cannot be
  /// spotted or repaired on a later pass - and without a pristine copy the
  /// only fix would be reinstalling the runtime by hand.
  /// </remarks>
  public static string Stage(string simpleName) {
    var source = Find(simpleName);
    if (source == null) {
      return null;
    }
    var workingDirectory = Path.Combine(Directory, "prepared");
    var working = Path.Combine(workingDirectory, Path.GetFileName(source));
    try {
      System.IO.Directory.CreateDirectory(workingDirectory);
      var expected = RewriteStamp.Expected(source);
      var current = RewriteStamp.Read(working);
      if (expected != null && current == expected) {
        return working;
      }
      File.Copy(source, working, overwrite: true);
      TitleLoadContext.Prepare(working, expected);
      XnaOs.Log(current == null
          ? $"   prepared {Path.GetFileName(source)} with host {RewriteStamp.HostVersion}"
          : $"   replaced {Path.GetFileName(source)}: it was prepared by a " +
            $"different host or from a different master (now host " +
            $"{RewriteStamp.HostVersion})");
      return working;
    } catch (Exception e) {
      XnaOs.Log(XnaOs.Level.Warning,
                $"   could not stage {Path.GetFileName(source)}: {e.Message}");
      return null;
    }
  }

  /// <summary>
  /// True once enough of the console runtime is present to render with it.
  /// Graphics is the one that matters; the rest can still come from MonoGame.
  /// </summary>
  public static bool Available => Find("Microsoft.Xna.Framework.Graphics") != null;
}
