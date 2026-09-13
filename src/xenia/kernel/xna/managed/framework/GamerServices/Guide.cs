// The guide, answered by Nexia's own XAM dialogs.
//
// A title calls Guide.BeginShowMessageBox or BeginShowKeyboardInput and gets
// back an IAsyncResult; on the console that dialog is up until a person deals
// with it. Nexia already draws both - the same MessageBoxDialog and
// KeyboardInputDialog a real 360 title gets - so all this does is start one and
// watch it.
//
// Unlike Storage, these genuinely cannot complete synchronously, so the wait
// happens on a poll: the dialog lives on the emulator's UI thread while the
// title's loop runs on its own. Titles already poll IAsyncResult.IsCompleted
// from Update, which is the same shape.
using System;
using System.Threading;
using System.Threading.Tasks;
using Nexia.Xna;

namespace Microsoft.Xna.Framework.GamerServices {
  internal sealed class GuideAsyncResult : IAsyncResult {
    private readonly ManualResetEventSlim completed = new ManualResetEventSlim(false);
    private readonly uint request;

    internal GuideAsyncResult(uint request, bool wantsText, object state,
                              AsyncCallback callback) {
      this.request = request;
      WantsText = wantsText;
      AsyncState = state;

      if (request == 0) {
        // Nexia could not show the dialog. Report it finished and cancelled
        // rather than leaving the title waiting on something that will never
        // happen.
        Status = XnaOsGuideStatus.Cancelled;
        Finish(callback);
        return;
      }

      Task.Run(() => Watch(callback));
    }

    internal bool WantsText { get; }
    internal XnaOsGuideStatus Status { get; private set; }
    internal int Button { get; private set; } = -1;
    internal string Text { get; private set; }

    public object AsyncState { get; }
    public bool CompletedSynchronously => false;
    public bool IsCompleted => completed.IsSet;
    public WaitHandle AsyncWaitHandle => completed.WaitHandle;

    private void Watch(AsyncCallback callback) {
      try {
        while (true) {
          var status = XnaOs.PollGuide(request, out int button);
          if (status == XnaOsGuideStatus.Pending) {
            // Roughly a frame. Polling faster would only burn a core waiting
            // on a person to read a dialog.
            Thread.Sleep(16);
            continue;
          }
          Status = status;
          Button = button;
          if (WantsText && status == XnaOsGuideStatus.Completed) {
            Text = XnaOs.GetGuideText(request);
          }
          break;
        }
      } catch (Exception) {
        Status = XnaOsGuideStatus.Cancelled;
      } finally {
        // The id is Nexia's to forget once the answer has been read.
        if (request != 0) {
          try { XnaOs.ReleaseGuide(request); } catch { }
        }
        Finish(callback);
      }
    }

    private void Finish(AsyncCallback callback) {
      completed.Set();
      callback?.Invoke(this);
    }
  }

  public sealed partial class Guide {
    public static bool IsVisible => XnaOs.IsAvailable && XnaOs.GuideIsVisible;

    // Purely managed, and the only honest answer once the licence is known: a
    // title hosted here was not bought as a trial. SimulateTrialMode exists so
    // a developer can test the trial path anyway, so it has to actually drive
    // IsTrialMode.
    public static bool SimulateTrialMode { get; set; }

    /// <summary>
    /// Set once a GamerServicesComponent has initialized. Until then the
    /// licence is genuinely unknown.
    /// </summary>
    internal static bool GamerServicesReady { get; set; }

    /// <summary>Whether the title is running as a trial.</summary>
    /// <remarks>
    /// True until gamer services initialize, which is not a fudge - it is what
    /// the console must do, and titles depend on it. Arcadecraft constructs a
    /// GamerServicesComponent, sets SimulateTrialMode = false, then asks for
    /// its ticker text - all BEFORE base.Initialize() runs the component. That
    /// text path reads an OnlineManager which is not created until much later,
    /// so the only reason the shipped game does not crash on its own first
    /// frame is that IsTrialMode returns true there and it returns early.
    /// Trial is the safe assumption before the licence is known.
    /// </remarks>
    public static bool IsTrialMode => SimulateTrialMode || !GamerServicesReady;

    public static IAsyncResult BeginShowMessageBox(
        string title, string text, System.Collections.Generic.IEnumerable<string> buttons,
        int focusButton, MessageBoxIcon icon, AsyncCallback callback, object state) =>
        BeginShowMessageBox(PlayerIndex.One, title, text, buttons, focusButton, icon,
                            callback, state);

    public static IAsyncResult BeginShowMessageBox(
        PlayerIndex player, string title, string text,
        System.Collections.Generic.IEnumerable<string> buttons, int focusButton,
        MessageBoxIcon icon, AsyncCallback callback, object state) {
      var labels = new System.Collections.Generic.List<string>();
      if (buttons != null) labels.AddRange(buttons);
      uint request = XnaOs.IsAvailable
          ? XnaOs.BeginMessageBox((int)player, title, text, labels.ToArray(), focusButton)
          : 0;
      return new GuideAsyncResult(request, false, state, callback);
    }

    /// <summary>Which button was pressed, or null if there was no answer.</summary>
    public static int? EndShowMessageBox(IAsyncResult result) {
      var guide = result as GuideAsyncResult;
      if (guide == null) return null;
      guide.AsyncWaitHandle.WaitOne();
      return guide.Status == XnaOsGuideStatus.Completed && guide.Button >= 0
          ? guide.Button : (int?)null;
    }

    public static IAsyncResult BeginShowKeyboardInput(
        PlayerIndex player, string title, string description, string defaultText,
        AsyncCallback callback, object state) =>
        BeginShowKeyboardInput(player, title, description, defaultText, callback,
                               state, false);

    public static IAsyncResult BeginShowKeyboardInput(
        PlayerIndex player, string title, string description, string defaultText,
        AsyncCallback callback, object state, bool usePasswordMode) {
      // Nexia's keyboard dialog has no password mode; showing the text when a
      // title asked for it to be hidden would be worse than ignoring the
      // request, so the flag is dropped and the field still works.
      const int maxLength = 256;
      uint request = XnaOs.IsAvailable
          ? XnaOs.BeginKeyboard((int)player, title, description, defaultText, maxLength)
          : 0;
      return new GuideAsyncResult(request, true, state, callback);
    }

    /// <summary>What was typed, or null if the person cancelled.</summary>
    public static string EndShowKeyboardInput(IAsyncResult result) {
      var guide = result as GuideAsyncResult;
      if (guide == null) return null;
      guide.AsyncWaitHandle.WaitOne();
      return guide.Status == XnaOsGuideStatus.Completed ? guide.Text : null;
    }
  }
}
