// Where the probe's findings go.
//
// Console first, because that is the one channel both hosts have: on Windows it
// is stdout, and under Nexia the managed host forwards it into the emulator
// log. A file is attempted as well and its failure ignored, since a console
// title has no writable working directory to count on.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Nexia.XnaProbe {

internal static class Report {
  private static readonly List<string> lines_ = new List<string>();

  // A case is RE-RENDERED every frame it is on screen - if it were drawn once
  // and the following frames skipped, Present would keep flipping between two
  // buffers holding different cases and the window would strobe between two
  // colours at half the refresh rate. So the drawing repeats and only the
  // frame that ends a case is allowed to speak.
  public static bool Enabled = true;

  public static void Line(string text) {
    if (!Enabled) {
      return;
    }
    lines_.Add(text);
    try {
      Console.WriteLine(text);
    } catch {
      // A title without a console still gets the file and the debugger.
    }
    try {
      System.Diagnostics.Debug.WriteLine(text);
    } catch {
    }
  }

  public static void Line(string format, params object[] args) {
    Line(string.Format(CultureInfo.InvariantCulture, format, args));
  }

  // A pixel as an exact, diffable token. Hex because a channel that is off by
  // one bit is the interesting case and decimal hides it.
  public static string Pixel(Microsoft.Xna.Framework.Color c) {
    return string.Format(CultureInfo.InvariantCulture, "{0:X2}{1:X2}{2:X2}{3:X2}",
                         c.R, c.G, c.B, c.A);
  }

  public static void Flush() {
    try {
      var text = new StringBuilder();
      foreach (var line in lines_) {
        text.AppendLine(line);
      }
      System.IO.File.WriteAllText("xnaprobe.txt", text.ToString());
    } catch {
      // Expected under a sandboxed host; the console copy is the real one.
    }
  }
}

}
