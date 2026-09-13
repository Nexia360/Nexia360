using System;

namespace Nexia.XnaProbe {

internal static class Program {
  [STAThread]
  private static void Main(string[] args) {
    try {
      using (var game = new SceneGame(args)) {
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
