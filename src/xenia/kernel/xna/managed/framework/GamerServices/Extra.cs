// Hand-written completions for the generated GamerServices stubs.
//
// Two kinds of thing live here. Constructors the generator cannot emit because
// the base class takes arguments, and - the interesting part - the members that
// are answered for real by Nexia. The names implemented here are listed in
// Stubs.skip so the generator leaves them out instead of colliding.
//
// When Nexia is not underneath (a plain desktop run of the tools), XnaOs is
// unbound and every one of these degrades to "nobody is signed in", which is a
// true statement rather than a crash.
using System;
using System.Collections.Generic;
using Nexia.Xna;

namespace Microsoft.Xna.Framework.GamerServices {
  // ReadOnlyCollection<T> has no parameterless constructor, so the generated
  // partial cannot rely on an implicit one. Empty is the right default: a
  // stub has no gamers to report.
  public partial class GamerCollection<T> {
    public GamerCollection()
        : base(new global::System.Collections.Generic.List<T>()) { }

    internal GamerCollection(global::System.Collections.Generic.IList<T> items)
        : base(items) { }
  }

  public partial class SignedInGamerCollection {
    internal SignedInGamerCollection(
        global::System.Collections.Generic.IList<SignedInGamer> items)
        : base(items) { }
  }

  public abstract partial class Gamer {
    // Set once when the gamer is built from a slot; a Gamer is a snapshot, and
    // XNA titles hold onto them across frames.
    internal string GamertagValue;

    public string Gamertag => GamertagValue ?? string.Empty;

    // The console distinguishes these - DisplayName can be a friendlier form -
    // but Nexia has one name per profile, so claiming two would be inventing.
    public string DisplayName => Gamertag;

    public override string ToString() => Gamertag;

    // A stub that swallowed this would lose whatever the title parked on the
    // gamer, with no error to point at.
    public object Tag { get; set; }

    // One SignedInGamer per slot, kept alive across reads. XNA titles stash
    // things on a gamer (Tag) and compare the one they saw last frame against
    // the one they see now, so handing back a fresh object every time would
    // quietly break both. The collection is rebuilt only when the membership
    // actually changes.
    private static readonly SignedInGamer[] BySlot = new SignedInGamer[4];
    private static SignedInGamerCollection cachedCollection;
    private static string cachedKey;

    /// <summary>
    /// Everyone signed in, as Nexia sees it. Re-read on each access because
    /// profiles sign in and out while a title is running, and XNA titles poll
    /// this rather than subscribing.
    /// </summary>
    public static SignedInGamerCollection SignedInGamers {
      get {
        var gamers = new List<SignedInGamer>();
        var key = new System.Text.StringBuilder();

        if (XnaOs.IsAvailable) {
          int slots = Math.Min(XnaOs.UserSlotCount, BySlot.Length);
          for (int slot = 0; slot < slots; slot++) {
            if (!XnaOs.TryGetUser(slot, out var user)) continue;
            var state = (XnaOsSigninState)user.SigninState;
            if (state == XnaOsSigninState.NotSignedIn) {
              BySlot[slot] = null;
              continue;
            }

            string gamertag = XnaOs.GetGamertag(user);
            // A different gamertag in the slot means a different person, so the
            // old object must not be handed out as if it were them.
            var gamer = BySlot[slot];
            if (gamer == null || !string.Equals(gamer.GamertagValue, gamertag,
                                                StringComparison.Ordinal)) {
              gamer = new SignedInGamer { GamertagValue = gamertag };
              BySlot[slot] = gamer;
            }
            gamer.PlayerIndexValue = (PlayerIndex)slot;
            gamer.IsSignedInToLiveValue = state == XnaOsSigninState.SignedInToLive;
            gamer.IsGuestValue = user.IsGuest != 0;

            gamers.Add(gamer);
            key.Append(slot).Append(':').Append(gamertag).Append(';');
          }
        }

        string currentKey = key.ToString();
        if (cachedCollection == null || currentKey != cachedKey) {
          cachedCollection = new SignedInGamerCollection(gamers);
          cachedKey = currentKey;
        }
        return cachedCollection;
      }
    }
  }

  public sealed partial class SignedInGamer {
    internal PlayerIndex PlayerIndexValue;
    internal bool IsSignedInToLiveValue;
    internal bool IsGuestValue;

    public PlayerIndex PlayerIndex => PlayerIndexValue;
    public bool IsSignedInToLive => IsSignedInToLiveValue;
    public bool IsGuest => IsGuestValue;
  }
}
