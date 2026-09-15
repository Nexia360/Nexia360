// Rewrites the handful of Compact Framework calls a 360 title makes that
// desktop .NET does not have.
//
// These titles were built against the .NET CF that shipped on the console. Its
// surface is nearly a subset of the desktop's, but not quite: BlockWorld calls
// Thread.SetProcessorAffinity, which only ever existed to pin work to one of
// the 360's hardware threads. There is no desktop equivalent and no need for
// one - the host scheduler already does the job - so the call is redirected to
// a shim that does nothing.
//
// Done at load time rather than by pre-recompiling the title, for the same
// reason the XNA facades are: there is then nothing to keep in sync and no
// second copy of the game to maintain. Cecil is already here for the facades.
//
// Idempotent: after a rewrite the call sites point at the shim, so a later pass
// finds nothing to do.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace Nexia.Xna;

/// <summary>
/// Stand-ins for Compact Framework methods. Each takes the original instance as
/// its first argument, because that is what is already on the stack where the
/// call was.
/// </summary>
public static partial class CfShims {
  /// <summary>
  /// TitleContainer.OpenStream, served from the mounted package rather than
  /// from a filesystem copy of it.
  /// </summary>
  public static System.IO.Stream OpenTitleStream(string name) {
    return NativeCalls.OpenTitleStream(name);
  }

  private static string[] MatchInPackage(string path, string[] names,
                                         string searchPattern) {
    var matched = new List<string>();
    foreach (var name in names) {
      if (string.IsNullOrEmpty(searchPattern) ||
          System.IO.Enumeration.FileSystemName.MatchesSimpleExpression(
              searchPattern, name, true)) {
        matched.Add(System.IO.Path.Combine(path, name));
      }
    }
    return matched.ToArray();
  }

  /// <summary>
  /// Thread affinity on the 360 pinned work to one of six hardware threads.
  /// Honouring it here would mean pinning a desktop thread to a core number
  /// that means something entirely different, which is worse than ignoring it.
  /// </summary>
  public static void SetProcessorAffinity(object thread, int[] processorIds) {
  }

  public static object StringBuilderValue(object field, object builder) {
    return builder?.ToString();
  }

  private delegate void DrawFn(int primitiveType, int baseVertex,
                               int minVertexIndex, int numVertices,
                               int startIndex, int primitiveCount,
                               int indexed);
  private static DrawFn draw;
  private static bool drawResolved;
  private static MethodInfo flushBuffer;
  private static bool flushResolved;

  // The draw is issued the moment the title asks for it, but the state that
  // goes with it is still sitting in the managed packet buffer - so it has to
  // be sent first or the draw runs against whatever the previous flush left.
  private static void FlushPending(object device) {
    if (device == null) {
      return;
    }
    if (!flushResolved) {
      flushResolved = true;
      try {
        flushBuffer = device.GetType().GetMethod(
            "FlushBuffer",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null, Type.EmptyTypes, null);
      } catch (Exception) {
      }
    }
    try {
      flushBuffer?.Invoke(device, null);
    } catch (Exception) {
    }
  }

  private static void ResolveDraw() {
    if (drawResolved) {
      return;
    }
    drawResolved = true;
    try {
      var handle = NativeLibrary.GetMainProgramHandle();
      if (NativeLibrary.TryGetExport(handle, "Nexia_XnaDraw",
                                     out IntPtr address)) {
        draw = Marshal.GetDelegateForFunctionPointer<DrawFn>(address);
      }
    } catch (Exception) {
    }
    XnaOs.Log(draw != null
                  ? "   draws go straight to the device"
                  : "   Nexia_XnaDraw is missing - draws stay on the packet path");
  }

  // The original packet builder, kept so a draw is never simply lost: if the
  // native side is not there, the call goes back the way it used to and the
  // packet decoder handles it exactly as before.
  private static readonly Dictionary<string, MethodInfo> Originals =
      new(StringComparer.Ordinal);

  private static bool CallOriginal(object device, string name,
                                   params object[] rest) {
    if (device == null) {
      return false;
    }
    MethodInfo original;
    lock (Originals) {
      if (!Originals.TryGetValue(name, out original)) {
        try {
          var helpers = device.GetType().Assembly.GetType(
              "Microsoft.Xna.Framework.Graphics.PacketHelpers");
          original = helpers?.GetMethod(
              name, BindingFlags.Static | BindingFlags.Public |
                        BindingFlags.NonPublic);
        } catch (Exception) {
          original = null;
        }
        Originals[name] = original;
      }
    }
    if (original == null) {
      return false;
    }
    try {
      var parameters = original.GetParameters();
      var arguments = new object[parameters.Length];
      arguments[0] = device;
      for (int i = 1; i < parameters.Length; i++) {
        var type = parameters[i].ParameterType;
        var value = rest[i - 1];
        arguments[i] = type.IsEnum ? Enum.ToObject(type, value) : value;
      }
      original.Invoke(null, arguments);
      return true;
    } catch (Exception) {
      return false;
    }
  }

  private delegate void EffectApplyFn(uint effectHandle, uint pass);
  private static EffectApplyFn effectApply;
  private static bool effectApplyResolved;
  private static readonly Dictionary<Type, FieldInfo> ComPtrs =
      new Dictionary<Type, FieldInfo>();

  // Every console graphics resource carries its native handle in pComPtr; it is
  // what the packet writer would have put in the header.
  private static uint HandleOf(object resource) {
    if (resource == null) {
      return 0;
    }
    FieldInfo field;
    lock (ComPtrs) {
      var type = resource.GetType();
      if (!ComPtrs.TryGetValue(type, out field)) {
        field = null;
        for (var walk = type; walk != null && field == null;
             walk = walk.BaseType) {
          field = walk.GetField("pComPtr", BindingFlags.Instance |
                                               BindingFlags.Public |
                                               BindingFlags.NonPublic);
        }
        ComPtrs[type] = field;
      }
    }
    if (field == null) {
      return 0;
    }
    try {
      return Convert.ToUInt32(field.GetValue(resource));
    } catch (Exception) {
      return 0;
    }
  }

  public static void EffectApply(object device, object effect, uint index,
                                 int stateFlags, uint textureFlags) {
    if (!effectApplyResolved) {
      effectApplyResolved = true;
      try {
        var handle = NativeLibrary.GetMainProgramHandle();
        if (NativeLibrary.TryGetExport(handle, "Nexia_XnaEffectApply",
                                       out IntPtr address)) {
          effectApply =
              Marshal.GetDelegateForFunctionPointer<EffectApplyFn>(address);
        }
      } catch (Exception) {
      }
    }
    if (effectApply == null) {
      CallOriginal(device, "SendEffectApplyPacket", effect, index, stateFlags,
                   textureFlags);
      return;
    }
    // Still sent the old way as well: the packet carries state this does not,
    // and the decoder is what the rest of the pipeline still reads.
    CallOriginal(device, "SendEffectApplyPacket", effect, index, stateFlags,
                 textureFlags);
    FlushPending(device);
    effectApply(HandleOf(effect), index);
  }

  public static void DrawPrimitive(object device, int primitiveType, int start,
                                   int primitiveCount) {
    ResolveDraw();
    if (draw == null) {
      CallOriginal(device, "SendDrawPrimitivePacket", primitiveType, start,
                   primitiveCount);
      return;
    }
    FlushPending(device);
    draw(primitiveType, 0, 0, 0, start, primitiveCount, 0);
  }

  private delegate void DrawUserFn(int primitiveType, int primitiveCount,
                                   IntPtr vertexData, uint vertexDataSize,
                                   uint vertexStride);
  private static DrawUserFn drawUser;
  private static bool drawUserResolved;

  /// <summary>
  /// DrawUserPrimitives carries its vertices in the packet instead of a bound
  /// stream. SunBurn's post-process chain draws every full-frame quad this way.
  /// </summary>
  public static void DrawUserPrimitive(object device, int primitiveType,
                                       int primitiveCount, IntPtr vertexData,
                                       uint vertexDataSize,
                                       uint vertexStride) {
    if (!drawUserResolved) {
      drawUserResolved = true;
      try {
        var program = NativeLibrary.GetMainProgramHandle();
        if (NativeLibrary.TryGetExport(program, "Nexia_XnaDrawUserPrimitives",
                                       out IntPtr address)) {
          drawUser = Marshal.GetDelegateForFunctionPointer<DrawUserFn>(address);
        }
      } catch (Exception) {
      }
    }
    if (drawUser == null) {
      CallOriginal(device, "SendDrawUserPrimitivePacket", primitiveType,
                   primitiveCount, vertexData, vertexDataSize, vertexStride);
      return;
    }
    FlushPending(device);
    drawUser(primitiveType, primitiveCount, vertexData, vertexDataSize,
             vertexStride);
  }

  private delegate void DrawUserIndexedFn(int primitiveType, int numVertices,
                                          int primitiveCount, IntPtr vertexData,
                                          uint vertexDataSize, IntPtr indexData,
                                          uint indexDataSize, uint vertexStride,
                                          uint sixteenBit);
  private static DrawUserIndexedFn drawUserIndexed;
  private static bool drawUserIndexedResolved;

  public static void DrawUserIndexedPrimitive(object device, int primitiveType,
                                              int numVertices,
                                              int primitiveCount,
                                              IntPtr vertexData,
                                              uint vertexDataSize,
                                              IntPtr indexData,
                                              uint indexDataSize,
                                              uint vertexStride,
                                              int sixteenBit) {
    if (!drawUserIndexedResolved) {
      drawUserIndexedResolved = true;
      try {
        var program = NativeLibrary.GetMainProgramHandle();
        if (NativeLibrary.TryGetExport(program,
                                       "Nexia_XnaDrawUserIndexedPrimitives",
                                       out IntPtr address)) {
          drawUserIndexed =
              Marshal.GetDelegateForFunctionPointer<DrawUserIndexedFn>(address);
        }
      } catch (Exception) {
      }
    }
    if (drawUserIndexed == null) {
      CallOriginal(device, "SendDrawUserIndexedPrimitivePacket", primitiveType,
                   numVertices, primitiveCount, vertexData, vertexDataSize,
                   indexData, indexDataSize, vertexStride, sixteenBit != 0);
      return;
    }
    FlushPending(device);
    drawUserIndexed(primitiveType, numVertices, primitiveCount, vertexData,
                    vertexDataSize, indexData, indexDataSize, vertexStride,
                    sixteenBit != 0 ? 1u : 0u);
  }

  public static void DrawIndexedPrimitive(object device, int primitiveType,
                                          int baseVertex, int minVertexIndex,
                                          int numVertices, int startIndex,
                                          int primitiveCount) {
    ResolveDraw();
    if (draw == null) {
      CallOriginal(device, "SendDrawIndexedPrimitivePacket", primitiveType,
                   baseVertex, minVertexIndex, numVertices, startIndex,
                   primitiveCount);
      return;
    }
    FlushPending(device);
    draw(primitiveType, baseVertex, minVertexIndex, numVertices, startIndex,
         primitiveCount, 1);
  }

  private delegate uint StorageRootFn(uint player, byte[] buffer, uint capacity);
  private static StorageRootFn storageRoot;
  private static bool storageRootResolved;
  private static readonly Dictionary<uint, string> StorageRoots =
      new Dictionary<uint, string>();

  public static string StorageRootForPlayer(uint player) {
    lock (StorageRoots) {
      if (StorageRoots.TryGetValue(player, out var cached)) {
        return cached;
      }
      if (!storageRootResolved) {
        storageRootResolved = true;
        try {
          var handle = NativeLibrary.GetMainProgramHandle();
          if (NativeLibrary.TryGetExport(handle, "Nexia_XnaStorageRoot",
                                         out IntPtr address)) {
            storageRoot =
                Marshal.GetDelegateForFunctionPointer<StorageRootFn>(address);
          }
        } catch (Exception) {
        }
      }
      string root = null;
      if (storageRoot != null) {
        var buffer = new byte[512];
        var length = storageRoot(player, buffer, (uint)buffer.Length);
        if (length > 0 && length < buffer.Length) {
          root = Encoding.UTF8.GetString(buffer, 0, (int)length);
        }
      }
      if (string.IsNullOrEmpty(root)) {
        root = Path.Combine(Path.GetTempPath(), "nexia_xna_storage",
                            "player" + player) + Path.DirectorySeparatorChar;
        Directory.CreateDirectory(root);
      }
      StorageRoots[player] = root;
      XnaOs.Log($"   storage root for player {player}: {root}");
      return root;
    }
  }
}

/// <summary>
/// An assembly resolver that gives up quietly.
/// </summary>
/// <remarks>
/// Cecil's DefaultAssemblyResolver throws when it cannot find a reference, and
/// a title's references include the XNA framework - which exists only in
/// memory here and is on no search path. Rewriting a call site does not need
/// those assemblies resolved, so failing to find one must not abandon the
/// rewrite.
/// </remarks>
internal sealed class TolerantResolver : DefaultAssemblyResolver {
  internal TolerantResolver(string directory) {
    if (!string.IsNullOrEmpty(directory)) {
      AddSearchDirectory(directory);
    }
    AddSearchDirectory(Bootstrap.OverlayDir);
  }

  private readonly Dictionary<string, AssemblyDefinition> fromPackage =
      new(StringComparer.OrdinalIgnoreCase);

  public override AssemblyDefinition Resolve(AssemblyNameReference name) {
    var packaged = ResolveFromPackage(name);
    if (packaged != null) {
      return packaged;
    }
    try {
      return base.Resolve(name);
    } catch (AssemblyResolutionException) {
      return null;
    }
  }

  private AssemblyDefinition ResolveFromPackage(AssemblyNameReference name) {
    if (name == null || string.IsNullOrEmpty(name.Name)) {
      return null;
    }
    if (fromPackage.TryGetValue(name.Name, out var cached)) {
      return cached;
    }
    AssemblyDefinition definition = null;
    var bytes = PackageAssemblies.Find(name.Name, name.Culture, out _);
    if (bytes != null) {
      try {
        definition = AssemblyDefinition.ReadAssembly(
            new MemoryStream(bytes),
            new ReaderParameters { AssemblyResolver = this });
      } catch (Exception) {
        definition = null;
      }
    }
    fromPackage[name.Name] = definition;
    return definition;
  }
}

/// <summary>
/// The same tolerance one level down, for types.
/// </summary>
/// <remarks>
/// Resolving an assembly and resolving a type through it are separate paths in
/// Cecil, and the second throws its own exception - a title referencing
/// Microsoft.Xna.Framework.Net.NetworkSessionType fails here even once the
/// assembly lookup gives up quietly. Nothing this does needs a type resolved.
/// </remarks>
internal sealed class TolerantMetadataResolver : MetadataResolver {
  internal TolerantMetadataResolver(IAssemblyResolver resolver) : base(resolver) {
  }

  public override TypeDefinition Resolve(TypeReference type) {
    try {
      return base.Resolve(type);
    } catch (Exception) {
      return null;
    }
  }

  public override FieldDefinition Resolve(FieldReference field) {
    try {
      return base.Resolve(field);
    } catch (Exception) {
      return null;
    }
  }

  public override MethodDefinition Resolve(MethodReference method) {
    try {
      return base.Resolve(method);
    } catch (Exception) {
      return null;
    }
  }
}

internal static class CompactFramework {
  // Keyed on "Namespace.Type::Method". Matching on the name alone would be too
  // eager - a title is free to have its own SetProcessorAffinity.
  private static readonly Dictionary<string, string> Redirects =
      new(StringComparer.Ordinal) {
        { "System.Threading.Thread::SetProcessorAffinity", "SetProcessorAffinity" },
        { "Microsoft.Xna.Framework.Storage.StorageContainer::GetStorageRootForPlayer",
          "StorageRootForPlayer" },
        { "Microsoft.Xna.Framework.Graphics.PacketHelpers::SendDrawPrimitivePacket",
          "DrawPrimitive" },
        { "Microsoft.Xna.Framework.Graphics.PacketHelpers::SendDrawIndexedPrimitivePacket",
          "DrawIndexedPrimitive" },
        { "Microsoft.Xna.Framework.Graphics.PacketHelpers::SendDrawUserPrimitivePacket",
          "DrawUserPrimitive" },
        { "Microsoft.Xna.Framework.Graphics.PacketHelpers::SendDrawUserIndexedPrimitivePacket",
          "DrawUserIndexedPrimitive" },
        { "Microsoft.Xna.Framework.Graphics.PacketHelpers::SendEffectApplyPacket",
          "EffectApply" },
        { "Microsoft.Xna.Framework.TitleContainer::OpenStream", "OpenTitleStream" },
        { "System.IO.File::Exists", "FileExists" },
        { "System.IO.File::Open", "FileOpen" },
        { "System.IO.File::OpenRead", "FileOpenRead" },
        { "System.IO.File::OpenText", "FileOpenText" },
        { "System.IO.File::OpenWrite", "FileOpenWrite" },
        { "System.IO.File::Create", "FileCreate" },
        { "System.IO.File::CreateText", "FileCreateText" },
        { "System.IO.File::AppendText", "FileAppendText" },
        { "System.IO.File::AppendAllText", "FileAppendAllText" },
        { "System.IO.File::ReadAllBytes", "FileReadAllBytes" },
        { "System.IO.File::ReadAllText", "FileReadAllText" },
        { "System.IO.File::ReadAllLines", "FileReadAllLines" },
        { "System.IO.File::WriteAllBytes", "FileWriteAllBytes" },
        { "System.IO.File::WriteAllText", "FileWriteAllText" },
        { "System.IO.File::WriteAllLines", "FileWriteAllLines" },
        { "System.IO.File::Delete", "FileDelete" },
        { "System.IO.File::Copy", "FileCopy" },
        { "System.IO.File::Move", "FileMove" },
        { "System.IO.File::GetAttributes", "FileGetAttributes" },
        { "System.IO.File::SetAttributes", "FileSetAttributes" },
        { "System.IO.File::GetCreationTime", "FileGetCreationTime" },
        { "System.IO.File::GetCreationTimeUtc", "FileGetCreationTimeUtc" },
        { "System.IO.File::GetLastWriteTime", "FileGetLastWriteTime" },
        { "System.IO.File::GetLastWriteTimeUtc", "FileGetLastWriteTimeUtc" },
        { "System.IO.File::GetLastAccessTime", "FileGetLastAccessTime" },
        { "System.IO.File::GetLastAccessTimeUtc", "FileGetLastAccessTimeUtc" },
        { "System.IO.File::SetCreationTime", "FileSetCreationTime" },
        { "System.IO.File::SetCreationTimeUtc", "FileSetCreationTimeUtc" },
        { "System.IO.File::SetLastWriteTime", "FileSetLastWriteTime" },
        { "System.IO.File::SetLastWriteTimeUtc", "FileSetLastWriteTimeUtc" },
        { "System.IO.File::SetLastAccessTime", "FileSetLastAccessTime" },
        { "System.IO.File::SetLastAccessTimeUtc", "FileSetLastAccessTimeUtc" },
        { "System.IO.Directory::Exists", "DirectoryExists" },
        { "System.IO.Directory::GetFiles", "DirectoryGetFiles" },
        { "System.IO.Directory::GetDirectories", "DirectoryGetDirectories" },
        { "System.IO.Directory::GetFileSystemEntries",
          "DirectoryGetFileSystemEntries" },
        { "System.IO.Directory::CreateDirectory", "DirectoryCreateDirectory" },
        { "System.IO.Directory::Delete", "DirectoryDelete" },
        { "System.IO.Directory::Move", "DirectoryMove" },
        { "System.IO.Directory::GetCreationTime", "FileGetCreationTime" },
        { "System.IO.Directory::GetCreationTimeUtc", "FileGetCreationTimeUtc" },
        { "System.IO.Directory::GetLastWriteTime", "FileGetLastWriteTime" },
        { "System.IO.Directory::GetLastWriteTimeUtc",
          "FileGetLastWriteTimeUtc" },
        { "System.IO.Directory::GetLastAccessTime", "FileGetLastAccessTime" },
        { "System.IO.Directory::GetLastAccessTimeUtc",
          "FileGetLastAccessTimeUtc" },
        { "System.IO.Directory::SetCreationTime", "DirectorySetCreationTime" },
        { "System.IO.Directory::SetCreationTimeUtc",
          "DirectorySetCreationTimeUtc" },
        { "System.IO.Directory::SetLastWriteTime", "DirectorySetLastWriteTime" },
        { "System.IO.Directory::SetLastWriteTimeUtc",
          "DirectorySetLastWriteTimeUtc" },
        { "System.IO.Directory::SetLastAccessTime",
          "DirectorySetLastAccessTime" },
        { "System.IO.Directory::SetLastAccessTimeUtc",
          "DirectorySetLastAccessTimeUtc" },
        { "System.IO.FileStream::.ctor", "NewFileStream" },
        { "System.IO.StreamReader::.ctor", "NewStreamReader" },
        { "System.IO.StreamWriter::.ctor", "NewStreamWriter" },
        { "System.IO.FileSystemInfo::get_Exists", "InfoExists" },
        { "System.IO.FileInfo::get_Exists", "InfoExists" },
        { "System.IO.DirectoryInfo::get_Exists", "InfoExists" },
        { "System.IO.FileSystemInfo::get_Attributes", "InfoGetAttributes" },
        { "System.IO.FileSystemInfo::set_Attributes", "InfoSetAttributes" },
        { "System.IO.FileSystemInfo::get_CreationTime", "InfoGetCreationTime" },
        { "System.IO.FileSystemInfo::get_CreationTimeUtc",
          "InfoGetCreationTimeUtc" },
        { "System.IO.FileSystemInfo::get_LastWriteTime",
          "InfoGetLastWriteTime" },
        { "System.IO.FileSystemInfo::get_LastWriteTimeUtc",
          "InfoGetLastWriteTimeUtc" },
        { "System.IO.FileSystemInfo::get_LastAccessTime",
          "InfoGetLastAccessTime" },
        { "System.IO.FileSystemInfo::get_LastAccessTimeUtc",
          "InfoGetLastAccessTimeUtc" },
        { "System.IO.FileSystemInfo::set_CreationTime", "InfoSetCreationTime" },
        { "System.IO.FileSystemInfo::set_CreationTimeUtc",
          "InfoSetCreationTimeUtc" },
        { "System.IO.FileSystemInfo::set_LastWriteTime",
          "InfoSetLastWriteTime" },
        { "System.IO.FileSystemInfo::set_LastWriteTimeUtc",
          "InfoSetLastWriteTimeUtc" },
        { "System.IO.FileSystemInfo::set_LastAccessTime",
          "InfoSetLastAccessTime" },
        { "System.IO.FileSystemInfo::set_LastAccessTimeUtc",
          "InfoSetLastAccessTimeUtc" },
        { "System.IO.FileSystemInfo::Delete", "InfoDelete" },
        { "System.IO.FileInfo::Delete", "InfoDelete" },
        { "System.IO.DirectoryInfo::Delete", "InfoDelete" },
        { "System.IO.FileInfo::get_Length", "InfoLength" },
        { "System.IO.FileInfo::OpenRead", "InfoOpenRead" },
        { "System.IO.FileInfo::OpenText", "InfoOpenText" },
        { "System.IO.FileInfo::Open", "InfoOpen" },
        { "System.IO.FileInfo::OpenWrite", "InfoOpenWrite" },
        { "System.IO.FileInfo::Create", "InfoCreateFile" },
        { "System.IO.FileInfo::CreateText", "InfoCreateText" },
        { "System.IO.FileInfo::AppendText", "InfoAppendText" },
        { "System.IO.FileInfo::CopyTo", "InfoCopyTo" },
        { "System.IO.FileInfo::MoveTo", "InfoMoveTo" },
        { "System.IO.DirectoryInfo::MoveTo", "InfoMoveTo" },
        { "System.IO.DirectoryInfo::GetFiles", "InfoGetFiles" },
        { "System.IO.DirectoryInfo::GetDirectories", "InfoGetDirectories" },
        { "System.IO.DirectoryInfo::GetFileSystemInfos",
          "InfoGetFileSystemInfos" },
        { "System.IO.DirectoryInfo::Create", "InfoCreateDirectory" },
        { "System.IO.DirectoryInfo::CreateSubdirectory",
          "InfoCreateSubdirectory" },
        { "System.Xml.XmlReader::Create", "XmlReaderCreate" },
        { "System.Xml.XmlWriter::Create", "XmlWriterCreate" },
        { "System.Xml.XmlDocument::Load", "XmlDocumentLoad" },
        { "System.Xml.XmlDocument::Save", "XmlDocumentSave" },
        { "System.Xml.XmlTextReader::.ctor", "NewXmlTextReader" },
        { "System.Xml.XmlTextWriter::.ctor", "NewXmlTextWriter" },
        { "System.Xml.Linq.XDocument::Load", "XDocumentLoad" },
        { "System.Xml.Linq.XDocument::Save", "XDocumentSave" },
        { "System.Xml.Linq.XElement::Load", "XElementLoad" },
        { "System.Xml.Linq.XElement::Save", "XElementSave" },
      };

  private static readonly HashSet<string> FilesystemTypes =
      new(StringComparer.Ordinal) {
        "System.IO.File",           "System.IO.Directory",
        "System.IO.FileStream",     "System.IO.StreamReader",
        "System.IO.StreamWriter",   "System.IO.FileInfo",
        "System.IO.DirectoryInfo",  "System.IO.FileSystemInfo",
        "System.Xml.XmlReader",     "System.Xml.XmlWriter",
        "System.Xml.XmlDocument",   "System.Xml.XmlTextReader",
        "System.Xml.XmlTextWriter", "System.Xml.Linq.XDocument",
        "System.Xml.Linq.XElement",
      };

  private static readonly HashSet<string> ConstructorOnlyTypes =
      new(StringComparer.Ordinal) {
        "System.IO.FileStream",   "System.IO.StreamReader",
        "System.IO.StreamWriter", "System.Xml.XmlTextReader",
        "System.Xml.XmlTextWriter",
      };

  private static readonly HashSet<string> XmlPathMembers =
      new(StringComparer.Ordinal) { "Create", "Load", "Save" };

  private static readonly HashSet<string> PureInfoMembers =
      new(StringComparer.Ordinal) {
        ".ctor",         "get_Name",    "get_FullName", "get_Extension",
        "get_DirectoryName", "get_Directory", "get_Parent", "get_Root",
        "Refresh",       "ToString",
      };

  private static bool TakesString(MethodReference called) {
    foreach (var parameter in called.Parameters) {
      if (parameter.ParameterType.FullName == "System.String") {
        return true;
      }
    }
    return false;
  }

  private static void ReportFilesystemCall(MethodReference called,
                                           string caller) {
    var type = called.DeclaringType.FullName;
    if (!FilesystemTypes.Contains(type)) {
      return;
    }
    var name = called.Name;
    if (ConstructorOnlyTypes.Contains(type)) {
      if (name != ".ctor" || !TakesString(called)) {
        return;
      }
    } else if (type.StartsWith("System.Xml", StringComparison.Ordinal)) {
      if (!XmlPathMembers.Contains(name) || !TakesString(called)) {
        return;
      }
    } else if (type == "System.IO.FileInfo" ||
               type == "System.IO.DirectoryInfo" ||
               type == "System.IO.FileSystemInfo") {
      if (PureInfoMembers.Contains(name)) {
        return;
      }
    } else if (!TakesString(called)) {
      return;
    }
    XnaOs.Log(XnaOs.Level.Warning,
              $"   host filesystem call not redirected in {caller}: " +
              called.FullName);
  }

  // SunBurn Pro checks a developer licence against SynapseGaming's activation
  // infrastructure, which no longer exists - there is no server to ask and no
  // activation file in the package, so the check cannot succeed and throws
  // "Product not activated" from a constructor. The licence was bought by the
  // studio and the game shipped on retail hardware; the check is simply
  // unanswerable now.
  private const string ActivationVendorPrefix = "SynapseGaming-SunBurn";
  private const string ActivationFailure =
      "Product not activated, please run activation tool.";

  private static bool IsActivationCheck(MethodDefinition method,
                                        string assemblyName) {
    if (!method.IsStatic || method.ReturnType.FullName != "System.Void" ||
        !assemblyName.StartsWith(ActivationVendorPrefix,
                                 StringComparison.OrdinalIgnoreCase)) {
      return false;
    }
    foreach (var instruction in method.Body.Instructions) {
      if (instruction.OpCode.Code == Code.Ldstr &&
          instruction.Operand is string text && text == ActivationFailure) {
        return true;
      }
    }
    return false;
  }

  /// <summary>
  /// Redirects any Compact Framework call in the assembly at
  /// <paramref name="path"/>, and empties SunBurn's activation checks.
  /// Returns true if the file was rewritten.
  /// </summary>
  // Everything a rewrite does to a module, shared by the file path and the
  // in-memory one. Returns how many call sites and bodies were changed.
  private static int RewriteModule(ModuleDefinition module,
                                   string assemblyName) {
    int rewritten = 0;
    rewritten += PatchEffectConstructor(module, assemblyName);
    rewritten += ReportIndexElementSize(module);
    rewritten += RewriteStringBuilderAliases(module);
    foreach (var type in AllTypes(module)) {
      foreach (var method in type.Methods) {
        if (!method.HasBody) continue;

        var identity = assemblyName + "!" + type.FullName + "::" + method.Name;
        if (IsActivationCheck(method, assemblyName)) {
          method.Body.Instructions.Clear();
          method.Body.Variables.Clear();
          method.Body.ExceptionHandlers.Clear();
          method.Body.GetILProcessor().Append(Instruction.Create(OpCodes.Ret));
          XnaOs.Log($"   neutralized {identity}");
          rewritten++;
          continue;
        }

        foreach (var instruction in method.Body.Instructions) {
          if (!(instruction.Operand is MethodReference called)) continue;
          bool construct = instruction.OpCode.Code == Code.Newobj;
          if (called.Name == ".ctor" && !construct) continue;
          var key = called.DeclaringType.FullName + "::" + called.Name;
          if (!Redirects.TryGetValue(key, out var shimName)) {
            ReportFilesystemCall(called, identity);
            continue;
          }

          MethodInfo target = null;
          MethodInfo first = null;
          foreach (var candidate in typeof(CfShims).GetMethods(
                       BindingFlags.Public | BindingFlags.Static)) {
            if (candidate.Name != shimName) {
              continue;
            }
            first ??= candidate;
            if (SignatureMatches(called, candidate, key, false, construct)) {
              target = candidate;
              break;
            }
          }
          if (target == null) {
            if (first != null && !construct) {
              SignatureMatches(called, first, key, true, construct);
            }
            ReportFilesystemCall(called, identity);
            continue;
          }
          var shim = module.ImportReference(target);
          // call, not callvirt: the shim is static, and the instance the call
          // site pushed becomes its first argument.
          instruction.OpCode = OpCodes.Call;
          instruction.Operand = shim;
          rewritten++;
        }
      }
    }
    return rewritten;
  }

  /// <summary>
  /// The same rewrite, on an image from the package. Returns the rewritten
  /// bytes, or null when nothing needed changing.
  /// </summary>
  public static byte[] RewriteImage(byte[] image, string name) {
    try {
      var resolver = new TolerantResolver(null);
      ModuleDefinition module;
      using (var input = new MemoryStream(image)) {
        module = ModuleDefinition.ReadModule(
            input, new ReaderParameters {
              AssemblyResolver = resolver,
              MetadataResolver = new TolerantMetadataResolver(resolver),
            });
        var assemblyName = Path.GetFileNameWithoutExtension(name);
        int rewritten = RewriteModule(module, assemblyName);
        if (rewritten == 0) {
          return null;
        }
        using var output = new MemoryStream();
        module.Write(output);
        XnaOs.Log($"   rewrote {rewritten} call(s) in {name} (from the package)");
        return output.ToArray();
      }
    } catch (Exception e) {
      XnaOs.Log(XnaOs.Level.Warning,
                $"   could not rewrite {name}: {e.Message}");
      return null;
    }
  }

  public static bool Rewrite(string path, string stamp = null) {
    try {
      bool needs = NeedsRewrite(path);
      if (!needs && stamp == null) {
        return false;
      }

      var resolver = new TolerantResolver(Path.GetDirectoryName(path));

      ModuleDefinition module;
      // Read through a copy: Cecil would otherwise hold the file open against
      // the write that follows.
      using (var image = new MemoryStream(File.ReadAllBytes(path))) {
        module = ModuleDefinition.ReadModule(
            image, new ReaderParameters {
              AssemblyResolver = resolver,
              MetadataResolver = new TolerantMetadataResolver(resolver),
            });

        var assemblyName = Path.GetFileNameWithoutExtension(path);
        int rewritten = needs ? RewriteModule(module, assemblyName) : 0;

        // Console P/Invokes are NOT rewritten any more: the emulator exports
        // those entry points itself and a DllImportResolver points the module
        // names at it, so the calls bind natively. Stripping them here would
        // remove the very P/Invokes that binding needs. NativeRewriter is kept
        // for reference - see xna_exports_generated.cc for what replaced it.
        if (rewritten == 0 && stamp == null) {
          return false;
        }
        if (stamp != null) {
          RewriteStamp.Apply(module, stamp);
        }

        using var output = new MemoryStream();
        module.Write(output);
        File.WriteAllBytes(path, output.ToArray());
        if (rewritten > 0) {
          XnaOs.Log($"   rewrote {rewritten} Compact Framework call(s) in " +
                    Path.GetFileName(path));
        }
        return true;
      }
    } catch (Exception e) {
      XnaOs.Log(XnaOs.Level.Warning,
                $"   could not rewrite {Path.GetFileName(path)}: {e.Message}");
      return false;
    }
  }

  // Reading the whole module to find nothing is the common case - almost no
  // assembly needs this - so the cheap check comes first: the name has to
  // appear in the file at all.

  /// <summary>
  /// Routes compiled effects through EffectImages.Prepare before the console
  /// runtime reads them.
  /// </summary>
  /// <remarks>
  /// Effect..ctor(GraphicsDevice, byte[]) checks the container magic with a
  /// native-order 32-bit load. The container was authored for a big-endian
  /// PowerPC and this now runs little-endian, so that first word reads
  /// reversed and every effect is refused before anything else happens. The
  /// fix is the one the XEX loader uses: put the image into host order first.
  ///
  /// Injected as three instructions at the top of the constructor -
  /// `effectCode = EffectImages.Prepare(effectCode)` - which leaves the rest of
  /// the method untouched and needs no knowledge of what it does.
  /// </remarks>
  private static int PatchEffectConstructor(ModuleDefinition module,
                                            string assemblyName) {
    var type = module.GetType("Microsoft.Xna.Framework.Graphics.Effect");
    if (type == null) {
      return 0;
    }
    MethodDefinition target = null;
    foreach (var method in type.Methods) {
      if (method.Name != ".ctor" || method.Parameters.Count != 2) continue;
      if (method.Parameters[1].ParameterType.FullName != "System.Byte[]") continue;
      target = method;
      break;
    }
    if (target == null || !target.HasBody) {
      return 0;
    }
    // Re-running the patch would stack a second call on top of the first.
    foreach (var instruction in target.Body.Instructions) {
      if (instruction.Operand is MethodReference existing &&
          existing.Name == "Prepare" &&
          existing.DeclaringType.Name == "EffectImages") {
        return 0;
      }
    }

    var prepare = module.ImportReference(
        typeof(EffectImages).GetMethod("Prepare"));
    var processor = target.Body.GetILProcessor();
    var first = target.Body.Instructions[0];
    var parameter = target.Parameters[1];
    processor.InsertBefore(first, processor.Create(OpCodes.Ldarg, parameter));
    processor.InsertBefore(first, processor.Create(OpCodes.Call, prepare));
    processor.InsertBefore(first, processor.Create(OpCodes.Starg, parameter));

    XnaOs.Log($"   effects in {assemblyName} load through Nexia");
    return 1;
  }

  private static readonly Dictionary<Code, OpCode> LongBranches = new() {
    { Code.Br_S, OpCodes.Br },
    { Code.Brfalse_S, OpCodes.Brfalse },
    { Code.Brtrue_S, OpCodes.Brtrue },
    { Code.Beq_S, OpCodes.Beq },
    { Code.Bge_S, OpCodes.Bge },
    { Code.Bgt_S, OpCodes.Bgt },
    { Code.Ble_S, OpCodes.Ble },
    { Code.Blt_S, OpCodes.Blt },
    { Code.Bne_Un_S, OpCodes.Bne_Un },
    { Code.Bge_Un_S, OpCodes.Bge_Un },
    { Code.Bgt_Un_S, OpCodes.Bgt_Un },
    { Code.Ble_Un_S, OpCodes.Ble_Un },
    { Code.Blt_Un_S, OpCodes.Blt_Un },
    { Code.Leave_S, OpCodes.Leave },
  };

  private static void WidenShortBranches(Mono.Cecil.Cil.MethodBody body) {
    foreach (var instruction in body.Instructions) {
      if (LongBranches.TryGetValue(instruction.OpCode.Code, out var wide)) {
        instruction.OpCode = wide;
      }
    }
  }

  private static bool SameField(FieldReference a, FieldReference b) {
    return a.Name == b.Name &&
           a.DeclaringType.FullName == b.DeclaringType.FullName;
  }

  private static int RewriteStringBuilderAliases(ModuleDefinition module) {
    var aliases = new List<(FieldReference alias, FieldReference builder)>();
    MethodReference value = null;
    int count = 0;
    foreach (var type in AllTypes(module)) {
      foreach (var method in type.Methods) {
        if (!method.HasBody) continue;
        var instructions = method.Body.Instructions;
        if (!instructions.Any(i => i.OpCode.Code == Code.Ldstr &&
                                   i.Operand as string == "m_StringValue")) {
          continue;
        }
        for (int i = 1; i + 2 < instructions.Count; ++i) {
          var call = instructions[i];
          if (!(call.Operand is MethodReference getValue) ||
              getValue.Name != "GetValue" ||
              getValue.DeclaringType.FullName !=
                  "System.Reflection.FieldInfo") {
            continue;
          }
          var source = instructions[i - 1].Operand as FieldReference;
          var store = instructions[i + 1].OpCode.Code == Code.Castclass
                          ? instructions[i + 2]
                          : instructions[i + 1];
          var alias = store.OpCode.Code == Code.Stfld
                          ? store.Operand as FieldReference
                          : null;
          if (source == null || alias == null ||
              source.FieldType.FullName != "System.Text.StringBuilder" ||
              alias.FieldType.FullName != "System.String") {
            continue;
          }
          value ??= module.ImportReference(
              typeof(CfShims).GetMethod(nameof(CfShims.StringBuilderValue)));
          call.OpCode = OpCodes.Call;
          call.Operand = value;
          aliases.Add((alias, source));
          XnaOs.Log($"   {alias.DeclaringType.FullName}::{alias.Name} is " +
                    $"read live from {source.Name}");
          count++;
        }
      }
    }
    if (aliases.Count == 0) {
      return count;
    }

    var toString = module.ImportReference(
        typeof(object).GetMethod(nameof(object.ToString)));
    foreach (var type in AllTypes(module)) {
      foreach (var method in type.Methods) {
        if (!method.HasBody) continue;
        var reads = method.Body.Instructions
                        .Where(i => i.OpCode.Code == Code.Ldfld &&
                                    i.Operand is FieldReference field &&
                                    aliases.Any(a => SameField(a.alias, field)))
                        .ToList();
        if (reads.Count == 0) continue;
        WidenShortBranches(method.Body);
        var il = method.Body.GetILProcessor();
        foreach (var read in reads) {
          var field = (FieldReference)read.Operand;
          read.Operand = aliases.First(a => SameField(a.alias, field)).builder;
          il.InsertAfter(read, il.Create(OpCodes.Callvirt, toString));
          count++;
        }
      }
    }
    return count;
  }

  private static int ReportIndexElementSize(ModuleDefinition module) {
    var indexBuffer = AllTypes(module).FirstOrDefault(
        t => t.FullName == "Microsoft.Xna.Framework.Graphics.IndexBuffer");
    if (indexBuffer == null) {
      return 0;
    }
    var report = module.ImportReference(
        typeof(NativeCalls).GetMethod(nameof(NativeCalls.IndexElementSize)));

    int count = 0;
    foreach (var method in indexBuffer.Methods) {
      if (method.Name != "SetData" && method.Name != "GetData") {
        continue;
      }
      if (!method.HasBody || method.GenericParameters.Count != 1) {
        continue;
      }
      var sizeOf = method.Body.Instructions
                       .Select(i => i.Operand as GenericInstanceMethod)
                       .FirstOrDefault(m => m != null &&
                                            m.ElementMethod.Name == "GetSizeOf");
      var comPtr = method.Body.Instructions
                       .Select(i => i.Operand as FieldReference)
                       .FirstOrDefault(f => f != null && f.Name == "pComPtr" &&
                                            f.DeclaringType.Name ==
                                                "GraphicsResource");
      if (sizeOf == null || comPtr == null) {
        continue;
      }

      var il = method.Body.GetILProcessor();
      var first = method.Body.Instructions[0];
      il.InsertBefore(first, il.Create(OpCodes.Ldarg_0));
      il.InsertBefore(first, il.Create(OpCodes.Ldfld, comPtr));
      il.InsertBefore(first, il.Create(OpCodes.Call, sizeOf));
      il.InsertBefore(first, il.Create(OpCodes.Call, report));
      XnaOs.Log($"   IndexBuffer::{method.Name}<T> now reports sizeof(T)");
      count++;
    }
    return count;
  }

  // A redirect only holds if what the call site left on the stack is what the
  // shim expects. Rejecting a mismatch leaves the original call in place, which
  // still works; emitting it anyway produces an InvalidProgramException the
  // first time the method runs.
  private static readonly HashSet<string> ReportedMismatches =
      new(StringComparer.Ordinal);

  private static bool SignatureMatches(MethodReference called, MethodInfo shim,
                                       string key, bool report = true,
                                       bool construct = false) {
    string reason = null;
    var shimParameters = shim.GetParameters();
    int offset = called.HasThis && !construct ? 1 : 0;
    if (construct && shim.ReturnType.FullName != called.DeclaringType.FullName) {
      reason = $"a constructor shim must return {called.DeclaringType.FullName}";
    } else if (offset == 1 && shimParameters.Length > 0 &&
        !shimParameters[0].ParameterType.IsAssignableFrom(typeof(object))) {
      reason = "instance call needs an object first argument";
    } else if (shimParameters.Length != called.Parameters.Count + offset) {
      reason = $"takes {shimParameters.Length} argument(s), call site pushes " +
               $"{called.Parameters.Count + offset}";
    } else {
      for (int i = 0; i < called.Parameters.Count; i++) {
        var from = called.Parameters[i].ParameterType;
        var to = shimParameters[i + offset].ParameterType;
        if (construct ? from.FullName != to.FullName
                      : !StackCompatible(from, to)) {
          reason = $"argument {i} is {from.FullName}, shim wants {to.FullName}";
          break;
        }
      }
    }
    if (reason == null) {
      return true;
    }
    if (report && ReportedMismatches.Add(key)) {
      XnaOs.Log(XnaOs.Level.Warning,
                $"   not redirecting {key}: {reason}");
    }
    return false;
  }

  private static bool StackCompatible(TypeReference from, Type to) {
    if (from.IsByReference || from.IsPointer || from.IsFunctionPointer) {
      return to == typeof(IntPtr) || to == typeof(UIntPtr);
    }
    if (to.IsEnum) {
      return to.FullName == from.FullName;
    }
    switch (from.FullName) {
      case "System.SByte":
      case "System.Byte":
      case "System.Int16":
      case "System.UInt16":
      case "System.Int32":
      case "System.UInt32":
      case "System.Boolean":
      case "System.Char":
        return to == typeof(int) || to == typeof(uint);
      case "System.Int64":
      case "System.UInt64":
        return to == typeof(long) || to == typeof(ulong);
      case "System.Single":
        return to == typeof(float);
      case "System.Double":
        return to == typeof(double);
      case "System.IntPtr":
      case "System.UIntPtr":
        return to == typeof(IntPtr) || to == typeof(UIntPtr);
    }
    TypeDefinition resolved = null;
    try {
      resolved = from.Resolve();
    } catch (Exception) {
    }
    if (resolved != null && resolved.IsEnum) {
      return to == typeof(int) || to == typeof(uint);
    }
    if (resolved != null && resolved.IsValueType) {
      return to.FullName == from.FullName;
    }
    if (to != typeof(object) && to.FullName != null &&
        to.FullName.StartsWith("System.", StringComparison.Ordinal)) {
      return from.FullName == to.FullName;
    }
    // A reference type, or one that could not be resolved because its assembly
    // is not on any search path - which is the normal case for console types.
    return !to.IsValueType;
  }

  private static bool NeedsRewrite(string path) {
    byte[] image;
    try {
      image = File.ReadAllBytes(path);
    } catch (Exception) {
      return false;
    }
    foreach (var key in Redirects.Keys) {
      var split = key.IndexOf("::", StringComparison.Ordinal);
      var method = key.Substring(split + 2);
      if (method == ".ctor") {
        var owner = key.Substring(0, split);
        method = owner.Substring(owner.LastIndexOf('.') + 1);
      }
      if (ContainsAscii(image, method)) {
        return true;
      }
    }
    if (Path.GetFileNameWithoutExtension(path).StartsWith(
            ActivationVendorPrefix, StringComparison.OrdinalIgnoreCase)) {
      return true;
    }
    // MXF.Graphics owns Effect..ctor, which is patched unconditionally.
    if (Path.GetFileName(path).Equals("MXF.Graphics.dlx",
                                      StringComparison.OrdinalIgnoreCase)) {
      return true;
    }
    // A console P/Invoke names its module in metadata, so the module name is
    // in the file as plain text.
    foreach (var console in new[] { "D3D", "XAM", "XINPUT", "STORAGE" }) {
      if (ContainsAscii(image, console + "_")) {
        return true;
      }
    }
    return false;
  }

  private static bool ContainsAscii(byte[] image, string needle) {
    if (needle.Length == 0 || image.Length < needle.Length) {
      return false;
    }
    byte first = (byte)needle[0];
    for (int i = 0; i <= image.Length - needle.Length; i++) {
      if (image[i] != first) continue;
      int j = 1;
      while (j < needle.Length && image[i + j] == (byte)needle[j]) j++;
      if (j == needle.Length) return true;
    }
    return false;
  }

  private static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition module) {
    var pending = new Stack<TypeDefinition>(module.Types);
    while (pending.Count > 0) {
      var type = pending.Pop();
      yield return type;
      foreach (var nested in type.NestedTypes) {
        pending.Push(nested);
      }
    }
  }
}
