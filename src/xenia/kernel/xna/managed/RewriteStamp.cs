using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using Mono.Cecil;

namespace Nexia.Xna;

internal static class RewriteStamp {
  internal const string ResourceName = "Nexia.Xna.RewriteStamp";

  private static readonly Dictionary<string, string> sourceIdentities =
      new(StringComparer.OrdinalIgnoreCase);

  internal static string HostVersion {
    get {
      var assembly = typeof(RewriteStamp).Assembly;
      var informational = assembly
          .GetCustomAttribute<AssemblyInformationalVersionAttribute>()
          ?.InformationalVersion;
      return string.IsNullOrEmpty(informational)
          ? assembly.GetName().Version?.ToString() ?? "0"
          : informational;
    }
  }

  internal static Guid HostMvid => typeof(RewriteStamp).Module.ModuleVersionId;

  internal static string Expected(string sourcePath) {
    var source = SourceIdentity(sourcePath);
    if (source == null) {
      return null;
    }
    return $"host={HostVersion}\nhostMvid={HostMvid}\n{source}";
  }

  internal static string Read(string path) {
    if (!File.Exists(path)) {
      return null;
    }
    try {
      var module = ModuleDefinition.ReadModule(
          new MemoryStream(File.ReadAllBytes(path)),
          new ReaderParameters { ReadingMode = ReadingMode.Deferred });
      var resource = module.Resources.OfType<EmbeddedResource>()
          .FirstOrDefault(r => r.Name == ResourceName);
      return resource == null
          ? null
          : Encoding.UTF8.GetString(resource.GetResourceData());
    } catch (Exception) {
      return null;
    }
  }

  internal static void Apply(ModuleDefinition module, string stamp) {
    for (int i = module.Resources.Count - 1; i >= 0; --i) {
      if (module.Resources[i].Name == ResourceName) {
        module.Resources.RemoveAt(i);
      }
    }
    module.Resources.Add(new EmbeddedResource(
        ResourceName, Mono.Cecil.ManifestResourceAttributes.Private,
        Encoding.UTF8.GetBytes(stamp)));
  }

  private static string SourceIdentity(string sourcePath) {
    lock (sourceIdentities) {
      if (sourceIdentities.TryGetValue(sourcePath, out var cached)) {
        return cached;
      }
    }
    string identity = null;
    try {
      var module = ModuleDefinition.ReadModule(
          new MemoryStream(File.ReadAllBytes(sourcePath)),
          new ReaderParameters { ReadingMode = ReadingMode.Deferred });
      var name = module.Assembly?.Name?.FullName ?? module.Name;
      identity = $"source={name}\nsourceMvid={module.Mvid}\n" +
                 $"sourceSize={new FileInfo(sourcePath).Length}";
    } catch (Exception e) {
      XnaOs.Log(XnaOs.Level.Warning,
                $"   could not read the identity of {Path.GetFileName(sourcePath)}: " +
                e.Message);
    }
    lock (sourceIdentities) {
      sourceIdentities[sourcePath] = identity;
    }
    return identity;
  }
}
