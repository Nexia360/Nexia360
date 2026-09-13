// On the 360, GamerServicesComponent lives in Microsoft.Xna.Framework.Game,
// not in .GamerServices where Windows XNA (and MonoGame) puts it - and MonoGame
// has it in neither. It is the only type MXF.Game.dlx exports that MonoGame
// does not.
//
// Which assembly it is declared in here does not matter: every facade forwards
// every Microsoft.Xna.Framework.* type this host exports, so a title asking
// Game for it finds it regardless of the namespace it sits in.
namespace Microsoft.Xna.Framework.GamerServices {
  public class GamerServicesComponent : global::Microsoft.Xna.Framework.GameComponent {
    public GamerServicesComponent(global::Microsoft.Xna.Framework.Game game) : base(game) { }
    public override void Initialize() {
      // From here on the licence is knowable, so Guide.IsTrialMode stops
      // assuming a trial. See Guide.IsTrialMode for why that matters.
      global::Microsoft.Xna.Framework.GamerServices.Guide.GamerServicesReady = true;
    }
    public override void Update(global::Microsoft.Xna.Framework.GameTime gameTime) { }
  }
}
