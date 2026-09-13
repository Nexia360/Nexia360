// Hand-written because these types' base classes take constructor arguments,
// which a generated partial cannot chain to. NetworkGamer and
// NetworkSessionJoinException used to live here too - stubgen emits them now.
#pragma warning disable CS0067, CS0649, CS0108, CS0114

namespace Microsoft.Xna.Framework.Net {
  public partial class NetworkException : global::System.Exception {
    public NetworkException() { }
    public NetworkException(string message) : base(message) { }
    public NetworkException(string message, global::System.Exception innerException)
        : base(message, innerException) { }
  }

  public partial class NetworkNotAvailableException : NetworkException {
    public NetworkNotAvailableException() { }
    public NetworkNotAvailableException(string message) : base(message) { }
  }

}

namespace Microsoft.Xna.Framework.Net {
  // Base classes below take constructor arguments, so the generated partials
  // deliberately declare none - these supply the chaining.
  public partial class AvailableNetworkSessionCollection {
    public AvailableNetworkSessionCollection()
        : base(new global::System.Collections.Generic.List<AvailableNetworkSession>()) { }
  }

  public partial class PacketReader {
    // An empty buffer: a stub never has a packet to hand back.
    public PacketReader() : base(new global::System.IO.MemoryStream()) { }
    public PacketReader(int capacity) : base(new global::System.IO.MemoryStream(capacity)) { }
  }
}
