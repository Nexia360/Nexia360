// Fabricates the Microsoft.Xna.Framework.* assemblies at load time instead of
// shipping them.
//
// A title asks for "Microsoft.Xna.Framework.Graphics"; the runtime insists the
// assembly it gets back carries that simple name, so MonoGame cannot simply be
// handed over. The usual answer is to build ten little facade DLLs full of
// [TypeForwardedTo] and install them somewhere. There is no need: the same
// assembly can be emitted into memory the moment it is asked for, from whatever
// MonoGame and host happen to be present.
//
// That is strictly better than shipping them. A generated forwarder lists
// exactly the types the loaded MonoGame exports, so it cannot drift out of step
// with it, and there is nothing to reinstall when either side changes.
//
// Reflection.Emit cannot do this - it has no way to write ExportedType rows -
// so the module is built with Cecil and handed to LoadFromStream.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using Mono.Cecil;

namespace Nexia.Xna;

internal static class FacadeFactory {
  // Every XNA assembly a 360 title can reference. A title asking for one of
  // these gets a fabricated forwarder; anything else is its own code.
  private static readonly string[] Facades = {
    "Microsoft.Xna.Framework",
    "Microsoft.Xna.Framework.Avatar",
    "Microsoft.Xna.Framework.Game",
    "Microsoft.Xna.Framework.GamerServices",
    "Microsoft.Xna.Framework.Graphics",
    "Microsoft.Xna.Framework.Input.Touch",
    "Microsoft.Xna.Framework.Net",
    "Microsoft.Xna.Framework.Storage",
    "Microsoft.Xna.Framework.Video",
    "Microsoft.Xna.Framework.Xact",
  };

  public static bool IsFacade(string simpleName) =>
      Array.IndexOf(Facades, simpleName) >= 0;

  /// <summary>
  /// Builds an assembly named <paramref name="simpleName"/> that forwards every
  /// public type in <paramref name="providers"/> living under the
  /// Microsoft.Xna.Framework namespace.
  /// </summary>
  /// <remarks>
  /// Providers are searched in order, and the first to export a type wins - so
  /// the host's own implementations (Storage, GamerServices) take precedence
  /// over MonoGame's, and MonoGame supplies everything else.
  /// </remarks>
  public static Stream Build(string simpleName, IReadOnlyList<Assembly> providers) {
    var assembly = AssemblyDefinition.CreateAssembly(
        new AssemblyNameDefinition(simpleName, new Version(4, 0, 0, 0)),
        simpleName, ModuleKind.Dll);
    var module = assembly.MainModule;

    // One AssemblyNameReference per provider, reused by every forward into it.
    var scopes = new Dictionary<Assembly, AssemblyNameReference>();
    foreach (var provider in providers) {
      var name = provider.GetName();
      var reference = new AssemblyNameReference(name.Name, name.Version);
      var token = name.GetPublicKeyToken();
      if (token != null && token.Length > 0) {
        reference.PublicKeyToken = token;
      }
      module.AssemblyReferences.Add(reference);
      scopes[provider] = reference;
    }

    var claimed = new Dictionary<string, ExportedType>(StringComparer.Ordinal);
    foreach (var provider in providers) {
      foreach (var type in SafeExportedTypes(provider)) {
        if (type.Namespace == null ||
            !type.Namespace.StartsWith("Microsoft.Xna.Framework", StringComparison.Ordinal)) {
          continue;
        }
        AddForward(module, type, scopes[provider], claimed, null);
      }
    }

    var stream = new MemoryStream();
    assembly.Write(stream);
    stream.Position = 0;
    return stream;
  }

  // A nested type forwards through its declaring type, so the outer row has to
  // exist first and be referenced as the inner one's implementation.
  private static ExportedType AddForward(ModuleDefinition module, Type type,
                                         AssemblyNameReference scope,
                                         Dictionary<string, ExportedType> claimed,
                                         ExportedType declaring) {
    if (type.IsNested && declaring == null) {
      declaring = AddForward(module, type.DeclaringType, scope, claimed, null);
      if (declaring == null) {
        return null;
      }
    }

    // Keyed on Type.FullName, which spells a nested type "Ns.Outer+Inner".
    // An ExportedType cannot answer that - a nested row carries only its own
    // name and an empty namespace - so the mapping is kept here rather than
    // rebuilt by scanning, which silently failed to match anything nested.
    if (claimed.TryGetValue(type.FullName, out var already)) {
      return already;
    }

    var exported = new ExportedType(
        type.IsNested ? null : type.Namespace,
        type.Name,
        module,
        type.IsNested ? (IMetadataScope)null : scope) {
      // Qualified: System.Reflection has a TypeAttributes too, and both are
      // in scope here.
      Attributes = Mono.Cecil.TypeAttributes.Public,
      // A nested type carries no scope of its own - it is reached through
      // its declaring type, which is why null was passed above.
      DeclaringType = declaring,
    };
    module.ExportedTypes.Add(exported);
    claimed[type.FullName] = exported;

    foreach (var nested in SafeNestedTypes(type)) {
      AddForward(module, nested, scope, claimed, exported);
    }
    return exported;
  }

  // A provider with a type it cannot load must not take the whole facade down
  // with it - the rest still forwards.
  private static IEnumerable<Type> SafeExportedTypes(Assembly assembly) {
    try {
      return assembly.GetExportedTypes();
    } catch (ReflectionTypeLoadException e) {
      return e.Types.Where(t => t != null && t.IsPublic);
    } catch (Exception) {
      return Array.Empty<Type>();
    }
  }

  private static IEnumerable<Type> SafeNestedTypes(Type type) {
    try {
      return type.GetNestedTypes(BindingFlags.Public);
    } catch (Exception) {
      return Array.Empty<Type>();
    }
  }
}
