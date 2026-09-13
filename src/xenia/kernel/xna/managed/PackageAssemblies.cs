using System;
using System.Collections.Generic;

namespace Nexia.Xna;

internal static class PackageAssemblies {
  private static readonly object Gate = new object();
  private static readonly Dictionary<string, byte[]> Images =
      new(StringComparer.OrdinalIgnoreCase);
  private static readonly Dictionary<string, string> Files =
      new(StringComparer.OrdinalIgnoreCase);
  private static readonly HashSet<string> Missing =
      new(StringComparer.OrdinalIgnoreCase);
  private static readonly HashSet<string> MissingFiles =
      new(StringComparer.OrdinalIgnoreCase);

  internal static bool IsOptional(string name) {
    return !string.IsNullOrEmpty(name) &&
           (name.EndsWith(".resources", StringComparison.OrdinalIgnoreCase) ||
            name.EndsWith(".XmlSerializers",
                          StringComparison.OrdinalIgnoreCase));
  }

  internal static byte[] Find(string name, string culture, out string file) {
    file = null;
    if (string.IsNullOrEmpty(name)) {
      return null;
    }
    var key = (culture ?? string.Empty) + "|" + name;
    lock (Gate) {
      if (Missing.Contains(key)) {
        return null;
      }
      if (Images.TryGetValue(key, out var cached)) {
        file = Files[key];
        return cached;
      }
    }
    var canStat = NativeCalls.CanStatTitlePath;
    foreach (var candidate in Candidates(name, culture)) {
      lock (Gate) {
        if (MissingFiles.Contains(candidate)) {
          continue;
        }
      }
      if (canStat &&
          (!NativeCalls.StatTitlePath(candidate, out _, out bool directory) ||
           directory)) {
        lock (Gate) {
          MissingFiles.Add(candidate);
        }
        continue;
      }
      var bytes = NativeCalls.ReadTitleFile(candidate);
      if (bytes == null) {
        lock (Gate) {
          MissingFiles.Add(candidate);
        }
        continue;
      }
      lock (Gate) {
        Images[key] = bytes;
        Files[key] = candidate;
      }
      file = candidate;
      return bytes;
    }
    lock (Gate) {
      Missing.Add(key);
    }
    return null;
  }

  private static IEnumerable<string> Candidates(string name, string culture) {
    var folders = new List<string>();
    if (!string.IsNullOrEmpty(culture)) {
      folders.Add(culture + "\\");
      var dash = culture.IndexOf('-');
      if (dash > 0) {
        folders.Add(culture.Substring(0, dash) + "\\");
      }
    }
    folders.Add(string.Empty);
    foreach (var folder in folders) {
      yield return folder + name + ".dll";
      yield return folder + name + ".exe";
    }
  }
}
