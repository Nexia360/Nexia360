// Nexia XNA probe - a reference oracle for the hosted XNA path.
//
// WHY THIS EXISTS. Diagnosing the hosted path against a commercial title means
// inferring what the GPU state SHOULD be from a third party engine. This is the
// other way round: a title whose intended state is known exactly, one case at a
// time, that runs on the PC XNA 4.0 runtime AND inside Nexia from the same
// binary - the console assemblies carry the identical assembly identity
// (Microsoft.Xna.Framework 4.0.0.0, PublicKeyToken 842cf8be1de50553), so the
// reference resolves to the GAC on Windows and to MXF.dlx under Nexia.
//
// Every case reports what it drew by READING THE FRAMEBUFFER BACK and printing
// pixel values as text. Comparing PC against Nexia is then a diff, with nothing
// to look at and no capture to review.

using System;

namespace Nexia.XnaProbe {

internal static class Program {
  [STAThread]
  private static void Main(string[] args) {
    try {
      using (var game = new ProbeGame(args)) {
        game.Run();
      }
    } catch (Exception e) {
      Report.Line("FATAL " + e.GetType().Name + ": " + e.Message);
      Report.Line(e.StackTrace ?? "<no stack>");
      Report.Flush();
    }
  }
}

}
