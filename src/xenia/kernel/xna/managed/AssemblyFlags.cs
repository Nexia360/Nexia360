// Clears the 32BITREQUIRED flag from a title's assemblies.
//
// Xbox 360 managed assemblies are ordinary I386 + ILONLY images - the IL inside
// is architecture neutral and there is no PPC code in them at all. Some are
// additionally marked 32BITREQUIRED in their CLI header, inherited from how
// they were built, and a 64-bit process refuses to load those with "the
// assembly architecture is not compatible with the current process
// architecture". Arcadecraft's SunBurn is one; so is AvatarCustomAnimation.
//
// The flag is a claim about a machine, not about the code. Clearing it makes
// the assembly load and changes nothing about what it does.
//
// The fix is written to the unpacked file rather than applied to a copy in
// memory, so the assembly keeps a real Location - middleware does look at it.
// The unpack directory is ours, so nothing a user supplied is modified.
using System;
using System.IO;

namespace Nexia.Xna;

internal static class AssemblyFlags {
  private const uint ILOnly = 0x00000001;
  private const uint Requires32Bit = 0x00000002;

  /// <summary>
  /// Clears 32BITREQUIRED if it is set. Returns true if the file was changed.
  /// Anything unexpected is left alone: a file this cannot parse is not one to
  /// start rewriting.
  /// </summary>
  /// <summary>
  /// The same 32BITREQUIRED clear, on an image held in memory. Returns the
  /// image, modified in place when the flag was set.
  /// </summary>
  public static byte[] MakeLoadableImage(byte[] image) {
    try {
      int offset = FindCorFlagsOffset(image);
      if (offset < 0) {
        return image;
      }
      uint flags = BitConverter.ToUInt32(image, offset);
      if ((flags & Requires32Bit) == 0 || (flags & ILOnly) == 0) {
        return image;
      }
      BitConverter.GetBytes(flags & ~Requires32Bit).CopyTo(image, offset);
      return image;
    } catch (Exception) {
      return image;
    }
  }

  public static bool MakeLoadable(string path) {
    try {
      byte[] image = File.ReadAllBytes(path);
      int offset = FindCorFlagsOffset(image);
      if (offset < 0) {
        return false;
      }

      uint flags = BitConverter.ToUInt32(image, offset);
      if ((flags & Requires32Bit) == 0) {
        return false;
      }
      // Only for pure IL. A mixed-mode image really does carry native code for
      // one architecture, and clearing the flag would just move the failure.
      if ((flags & ILOnly) == 0) {
        XnaOs.Log(XnaOs.Level.Warning,
                  $"   {Path.GetFileName(path)} is 32-bit and not IL-only - leaving it alone");
        return false;
      }

      BitConverter.GetBytes(flags & ~Requires32Bit).CopyTo(image, offset);
      File.WriteAllBytes(path, image);
      XnaOs.Log($"   cleared 32BITREQUIRED on {Path.GetFileName(path)}");
      return true;
    } catch (Exception e) {
      XnaOs.Log(XnaOs.Level.Warning,
                $"   could not inspect {Path.GetFileName(path)}: {e.Message}");
      return false;
    }
  }

  // Walks to the CLI header's Flags field, translating its RVA through the
  // section table - the data directory gives an RVA, and the file is not
  // loaded, so it has to be mapped back to an offset by hand.
  private static int FindCorFlagsOffset(byte[] image) {
    if (image.Length < 0x40 || BitConverter.ToUInt16(image, 0) != 0x5A4D) {
      return -1;
    }
    int pe = BitConverter.ToInt32(image, 0x3C);
    if (pe < 0 || pe + 0x78 > image.Length ||
        BitConverter.ToUInt32(image, pe) != 0x00004550) {
      return -1;
    }

    int sectionCount = BitConverter.ToUInt16(image, pe + 6);
    int optionalSize = BitConverter.ToUInt16(image, pe + 20);
    int optional = pe + 24;
    ushort magic = BitConverter.ToUInt16(image, optional);
    // PE32 keeps the directories at 0x60, PE32+ at 0x70.
    int directories = optional + (magic == 0x010B ? 0x60 : 0x70);

    const int ComDescriptorIndex = 14;
    int entry = directories + ComDescriptorIndex * 8;
    if (entry + 8 > image.Length) {
      return -1;
    }
    uint rva = BitConverter.ToUInt32(image, entry);
    if (rva == 0) {
      return -1;
    }

    int sections = optional + optionalSize;
    for (int i = 0; i < sectionCount; i++) {
      int section = sections + i * 40;
      if (section + 40 > image.Length) {
        return -1;
      }
      uint virtualSize = BitConverter.ToUInt32(image, section + 8);
      uint virtualAddress = BitConverter.ToUInt32(image, section + 12);
      uint rawAddress = BitConverter.ToUInt32(image, section + 20);
      if (rva >= virtualAddress && rva < virtualAddress + Math.Max(virtualSize, 1)) {
        // CLI header: cb, major, minor, MetaData (8), then Flags.
        int header = (int)(rawAddress + (rva - virtualAddress));
        int flags = header + 16;
        return flags + 4 <= image.Length ? flags : -1;
      }
    }
    return -1;
  }
}
