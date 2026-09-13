// The cases.
//
// One case per 45 frames, each leaving the device in a stated condition and
// then reading the framebuffer back. Vertices are given in CLIP SPACE with
// identity transforms wherever the case is not about transforms, so the pixel
// a triangle must cover is arithmetic rather than a matter of opinion - x -1 is
// the left edge, +1 the right, y +1 the top. Culling is off throughout except
// in the case that exists to test it, so a winding mistake cannot be mistaken
// for a state bug.
//
// EVERY VALUE IS UNIQUE AND SAYS WHERE IT CAME FROM - see Tag. Nothing is
// cleared to black and nothing is drawn in a plain primary, because a value
// that turns up somewhere it should not be is only useful if it names its
// origin. A stale register, a buffer that was never rewritten, a render target
// read at the wrong EDRAM base: each of those hands back a value, and the
// value has to be enough to say which case and which role produced it.

using System;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;

namespace Nexia.XnaProbe {

internal sealed class ProbeGame : Game {
  private const int kWidth = 1280;
  private const int kHeight = 720;
  // The device is not worth reading until it has presented a few times; the
  // first frames of a fresh swap chain are not a state anyone chose.
  private const int kWarmupFrames = 8;
  // HOW LONG EACH CASE STAYS ON SCREEN, IN FRAMES AT 60Hz.
  //
  // Every case fills the window with a different flat colour, so running one
  // per frame makes this a 60Hz full-screen strobe - which is exactly the
  // pattern that must not be produced. Held for three quarters of a second the
  // change rate is about 1.3Hz, well under the three-per-second threshold, and
  // the whole run still takes seven seconds.
  private const int kFramesPerCase = 45;
  private const int kSceneFrames = 300;

  private readonly GraphicsDeviceManager graphics_;
  private BasicEffect effect_;
  private Texture2D checker_;
  // Created once, not per frame: a case is re-rendered every frame it is up,
  // and building a render target sixty times a second would be measuring
  // resource churn rather than the state the case is about.
  // Distinct sizes per case, and none of them square or a power of two: a
  // readback taken at the wrong pitch, or a target reached at another case's
  // EDRAM base, shows up as scrambled rows instead of a plausible picture.
  private const int kSingleW = 200, kSingleH = 120;  // case 4
  private const int kMrtW = 176, kMrtH = 144;        // case 7, both targets
  private RenderTarget2D rt_single_;
  private RenderTarget2D rt_mrt0_;
  private RenderTarget2D rt_mrt1_;
  private RenderTarget2D rt_scene_;
  private Texture2D white_;
  private SpriteBatch sprites_;
  private Texture2D sky_;
  private VertexBuffer sky_vertices_;
  private IndexBuffer sky_indices_;
  private VertexBuffer cube_vertices_;
  private IndexBuffer cube_indices_;
  private Color[] rt_readback_;
  private Color[] readback_;
  private int frame_;
  private int case_index_;
  private int case_frame_;
  private bool finished_;

  public ProbeGame(string[] args) {
    graphics_ = new GraphicsDeviceManager(this);
    graphics_.PreferredBackBufferWidth = kWidth;
    graphics_.PreferredBackBufferHeight = kHeight;
    // HiDef is what the Xbox 360 is, and multiple render targets need it.
    graphics_.GraphicsProfile = GraphicsProfile.HiDef;
    // Paced, not free-running - see kFramesPerCase.
    graphics_.SynchronizeWithVerticalRetrace = true;
    IsFixedTimeStep = true;
    Content.RootDirectory = "Content";
  }

  // ---- the tag scheme ------------------------------------------------------

  // THE ROLE IS THE HIGH NIBBLE, THE CASE IS THE LOW NIBBLE.
  //
  // Roles are COMPLEMENTARY PAIRS, because the thing a case is usually asking
  // is "did the left half differ from the right" - and two colours that differ
  // only in one channel by 0x10 are the same colour to look at. Red against
  // cyan is not. The pairs are red/cyan, green/magenta, yellow/blue, so
  // whichever two roles a case puts side by side are opposites.
  //
  // The case still travels in the low nibble of every channel, where it costs
  // nothing visually and reads straight out of the hex: F32323 is role 0 of
  // case 2, 23F3F3 is role 1 of the same case. No two (case, role) pairs
  // collide, and no channel is ever 00 or FF - so a value from an
  // uninitialised buffer, a cleared one or a saturated one can never be
  // mistaken for one the probe wrote.
  private static readonly byte[][] kRolePalette = {
    new byte[] { 0xF, 0x2, 0x2 },  // 0 red
    new byte[] { 0x2, 0xF, 0xF },  // 1 cyan     - complement of red
    new byte[] { 0x2, 0xF, 0x2 },  // 2 green
    new byte[] { 0xF, 0x2, 0xF },  // 3 magenta  - complement of green
    new byte[] { 0xF, 0xF, 0x2 },  // 4 yellow
    new byte[] { 0x2, 0x2, 0xF },  // 5 blue     - complement of yellow
  };

  private static Color Tag(int case_index, int role) {
    var p = kRolePalette[role % kRolePalette.Length];
    var low = (byte)((case_index + 1) & 0xF);
    return new Color((byte)((p[0] << 4) | low), (byte)((p[1] << 4) | low),
                     (byte)((p[2] << 4) | low), (byte)0xFF);
  }

  private static string Hex(Color c) {
    return Report.Pixel(c);
  }

  // THE SAME IDEA FOR EVERYTHING THAT IS NOT A COLOUR.
  //
  // A depth, an extent, an angle or a camera distance that is shared between
  // two cases cannot be traced when it turns up in the wrong one, so none of
  // them are. Depths are 0.05 + 0.09*case + 0.012*role, which stays inside
  // 0..1 for every case here and never repeats; extents are per case and
  // deliberately not round, so a quad drawn from a stale vertex buffer covers
  // a visibly different rectangle rather than the same one. Nothing is 0, 1,
  // 0.5 or a screen fraction that another case also uses.
  private static float Z(int case_index, int role) {
    return 0.05f + 0.09f * case_index + 0.012f * role;
  }

  // The clip space rectangle case N draws into, as x0 y0 x1 y1. Cases 3, 6 and
  // 7 must cover the whole target for what they test, so they are full frame
  // by necessity and are told apart by their depth and their tags instead.
  private static readonly float[][] kRects = {
    new[] { -0.93f, -0.87f, 0.89f, 0.91f },   // 0 unused, clear only
    new[] { -0.98f, -0.94f, -0.06f, 0.92f },  // 1 a left band, not exactly half
    new[] { 0f, 0f, 0f, 0f },                 // 2 computed from the projection
    new[] { -1f, -1f, 1f, 1f },               // 3 full frame, textured
    new[] { -0.97f, -0.89f, -0.11f, 0.93f },  // 4 a left band inside the target
    new[] { -0.62f, -0.55f, 0.58f, 0.61f },   // 5 both depth quads share this
    new[] { -1f, -1f, 1f, 1f },               // 6 full frame, blended
    new[] { -1f, -1f, 1f, 1f },               // 7 full frame, into two targets
    new[] { -0.99f, -0.83f, -0.03f, 0.87f },  // 8 left; right is mirrored below
  };

  protected override void LoadContent() {
    effect_ = new BasicEffect(GraphicsDevice);
    effect_.World = Matrix.Identity;
    effect_.View = Matrix.Identity;
    effect_.Projection = Matrix.Identity;
    effect_.VertexColorEnabled = true;
    effect_.LightingEnabled = false;
    effect_.TextureEnabled = false;

    // Built here rather than loaded, so the probe carries no content and no
    // content pipeline is needed to build it. Four distinct tags, so which
    // texel landed in which quadrant is readable from the value alone - a
    // texture sampled with the wrong V, the wrong stride or the wrong swizzle
    // reports a tag that says which texel it actually fetched.
    checker_ = new Texture2D(GraphicsDevice, 2, 2, false, SurfaceFormat.Color);
    // Adjacent texels are complementary pairs, so a quadrant landing in the
    // wrong place is unmistakable rather than a slightly different shade.
    checker_.SetData(new[] {
      Tag(3, 0), Tag(3, 1),   // red    cyan
      Tag(3, 2), Tag(3, 3),   // green  magenta
    });

    rt_single_ = new RenderTarget2D(GraphicsDevice, kSingleW, kSingleH, false,
                                    SurfaceFormat.Color,
                                    DepthFormat.Depth24Stencil8);
    // Multiple render targets must agree on size, so these two match each
    // other and differ from the single target above.
    rt_mrt0_ = new RenderTarget2D(GraphicsDevice, kMrtW, kMrtH, false,
                                  SurfaceFormat.Color,
                                  DepthFormat.Depth24Stencil8);
    rt_mrt1_ = new RenderTarget2D(GraphicsDevice, kMrtW, kMrtH, false,
                                  SurfaceFormat.Color, DepthFormat.None);
    rt_scene_ = new RenderTarget2D(GraphicsDevice, kWidth, kHeight, false,
                                   SurfaceFormat.Color,
                                   DepthFormat.Depth24Stencil8);
    white_ = new Texture2D(GraphicsDevice, 1, 1, false, SurfaceFormat.Color);
    white_.SetData(new[] { Color.White });
    sprites_ = new SpriteBatch(GraphicsDevice);
    sky_ = LoadSky();

    var sky = new System.Collections.Generic.List<VertexPositionNormalTexture>();
    AddSkyFace(sky, new Vector3(0, 0, -1), new Vector3(1, 0, 0), new Vector3(0, -1, 0));
    AddSkyFace(sky, new Vector3(1, 0, 0), new Vector3(0, 0, 1), new Vector3(0, -1, 0));
    AddSkyFace(sky, new Vector3(0, 0, 1), new Vector3(-1, 0, 0), new Vector3(0, -1, 0));
    AddSkyFace(sky, new Vector3(-1, 0, 0), new Vector3(0, 0, -1), new Vector3(0, -1, 0));
    AddSkyFace(sky, new Vector3(0, 1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, 1));
    AddSkyFace(sky, new Vector3(0, -1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, -1));
    sky_vertices_ = new VertexBuffer(GraphicsDevice,
                                     typeof(VertexPositionNormalTexture),
                                     sky.Count, BufferUsage.WriteOnly);
    sky_vertices_.SetData(sky.ToArray());
    sky_indices_ = new IndexBuffer(GraphicsDevice, IndexElementSize.SixteenBits,
                                   36, BufferUsage.WriteOnly);
    sky_indices_.SetData(QuadIndices(6));

    var cube = new System.Collections.Generic.List<VertexPositionColor>();
    AddCubeFace(cube, new Vector3(1, 0, 0), new Vector3(0, 0, -1), new Vector3(0, -1, 0), kCubeFaces[0]);
    AddCubeFace(cube, new Vector3(-1, 0, 0), new Vector3(0, 0, 1), new Vector3(0, -1, 0), kCubeFaces[1]);
    AddCubeFace(cube, new Vector3(0, 1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, 1), kCubeFaces[2]);
    AddCubeFace(cube, new Vector3(0, -1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, -1), kCubeFaces[3]);
    AddCubeFace(cube, new Vector3(0, 0, 1), new Vector3(1, 0, 0), new Vector3(0, -1, 0), kCubeFaces[4]);
    AddCubeFace(cube, new Vector3(0, 0, -1), new Vector3(-1, 0, 0), new Vector3(0, -1, 0), kCubeFaces[5]);
    cube_vertices_ = new VertexBuffer(GraphicsDevice, typeof(VertexPositionColor),
                                      cube.Count, BufferUsage.WriteOnly);
    cube_vertices_.SetData(cube.ToArray());
    cube_indices_ = new IndexBuffer(GraphicsDevice, IndexElementSize.SixteenBits,
                                    36, BufferUsage.WriteOnly);
    cube_indices_.SetData(QuadIndices(6));
    rt_readback_ = new Color[Math.Max(kSingleW * kSingleH, kMrtW * kMrtH)];
    readback_ = new Color[kWidth * kHeight];

    Report.Line("=== Nexia XNA probe ===");
    Report.Line("adapter      : {0}", Describe(GraphicsAdapter.DefaultAdapter));
    Report.Line("profile      : {0}", GraphicsDevice.GraphicsProfile);
    Report.Line("backbuffer   : {0}x{1} {2}",
                GraphicsDevice.PresentationParameters.BackBufferWidth,
                GraphicsDevice.PresentationParameters.BackBufferHeight,
                GraphicsDevice.PresentationParameters.BackBufferFormat);
    Report.Line("depth format : {0}",
                GraphicsDevice.PresentationParameters.DepthStencilFormat);
    Report.Line("tag scheme   : high nibble = role (0 red, 1 cyan, 2 green, " +
                "3 magenta, 4 yellow, 5 blue - complementary pairs),");
    Report.Line("               low nibble of every channel = case + 1. " +
                "F32323 is case 2 role 0, 23F3F3 is case 2 role 1.");
    Report.Line("");
  }

  private static string Describe(GraphicsAdapter adapter) {
    try {
      return adapter.Description;
    } catch {
      return "<unavailable>";
    }
  }

  // ---- geometry helpers ----------------------------------------------------

  // A clip space rectangle at a stated depth, as two triangles. z is the value
  // written to clip z with w = 1, so it IS the NDC depth the test expects.
  private static VertexPositionColor[] Quad(float x0, float y0, float x1,
                                            float y1, float z, Color c) {
    var a = new Vector3(x0, y1, z);
    var b = new Vector3(x1, y1, z);
    var d = new Vector3(x0, y0, z);
    var e = new Vector3(x1, y0, z);
    return new[] {
      new VertexPositionColor(a, c), new VertexPositionColor(b, c),
      new VertexPositionColor(d, c), new VertexPositionColor(b, c),
      new VertexPositionColor(e, c), new VertexPositionColor(d, c),
    };
  }

  private static VertexPositionTexture[] TexQuad(float x0, float y0, float x1,
                                                 float y1, float z) {
    var a = new Vector3(x0, y1, z);
    var b = new Vector3(x1, y1, z);
    var d = new Vector3(x0, y0, z);
    var e = new Vector3(x1, y0, z);
    return new[] {
      new VertexPositionTexture(a, new Vector2(0, 0)),
      new VertexPositionTexture(b, new Vector2(1, 0)),
      new VertexPositionTexture(d, new Vector2(0, 1)),
      new VertexPositionTexture(b, new Vector2(1, 0)),
      new VertexPositionTexture(e, new Vector2(1, 1)),
      new VertexPositionTexture(d, new Vector2(0, 1)),
    };
  }

  private void DrawQuad(VertexPositionColor[] verts) {
    foreach (var pass in effect_.CurrentTechnique.Passes) {
      pass.Apply();
      GraphicsDevice.DrawUserPrimitives(PrimitiveType.TriangleList, verts, 0, 2);
    }
  }

  private void DrawQuad(VertexPositionTexture[] verts) {
    foreach (var pass in effect_.CurrentTechnique.Passes) {
      pass.Apply();
      GraphicsDevice.DrawUserPrimitives(PrimitiveType.TriangleList, verts, 0, 2);
    }
  }

  // Everything back to a stated baseline, so a case never inherits the last
  // one's state - which is exactly the class of bug this probe exists to find.
  private void Baseline() {
    GraphicsDevice.BlendState = BlendState.Opaque;
    GraphicsDevice.DepthStencilState = DepthStencilState.None;
    GraphicsDevice.RasterizerState = RasterizerState.CullNone;
    GraphicsDevice.SamplerStates[0] = SamplerState.PointClamp;
    effect_.World = Matrix.Identity;
    effect_.View = Matrix.Identity;
    effect_.Projection = Matrix.Identity;
    effect_.VertexColorEnabled = true;
    effect_.TextureEnabled = false;
    effect_.Alpha = 1.0f;
    effect_.DiffuseColor = Vector3.One;
  }

  // ---- readback ------------------------------------------------------------

  private Color At(int x, int y) {
    return readback_[y * kWidth + x];
  }

  // Skipped on the frames that are only holding the picture still - a 3.7 MB
  // readback sixty times a second is not needed to keep a colour on screen.
  private void ReadBackBuffer() {
    if (!Report.Enabled) {
      return;
    }
    GraphicsDevice.GetBackBufferData(readback_);
  }

  // The pixels every case reports, so one line can be compared against another
  // run without deciding case by case what mattered.
  //
  // Sampled at an EIGHTH and seven eighths rather than the quarters: a case
  // that covers the middle half of the screen puts its edge exactly on the
  // quarter, and a sample sitting on an edge reports whichever side rounding
  // fell on rather than anything about the state under test.
  private void SampleGrid(string label) {
    Report.Line(
        "  {0,-14} centre={1} L={2} R={3} T={4} B={5} TL={6} BR={7}", label,
        Hex(At(kWidth / 2, kHeight / 2)), Hex(At(kWidth / 8, kHeight / 2)),
        Hex(At(7 * kWidth / 8, kHeight / 2)), Hex(At(kWidth / 2, kHeight / 8)),
        Hex(At(kWidth / 2, 7 * kHeight / 8)), Hex(At(4, 4)),
        Hex(At(kWidth - 5, kHeight - 5)));
  }

  // ---- the cases -----------------------------------------------------------

  private void Case0_Clear() {
    // ROLE 0 IS WHAT A CASE DRAWS, ROLE 1 IS WHAT IT CLEARS TO - and they are
    // a complementary pair, so the thing under test is red against cyan
    // rather than two shades of the same colour. Roles 2 and up are the
    // "neither of those" colours a case needs on top.
    GraphicsDevice.Clear(Tag(0, 1));
    ReadBackBuffer();
    Report.Line("CASE 0 clear                 expect every sample {0}",
                Hex(Tag(0, 1)));
    SampleGrid("got");
  }

  private void Case1_ClipSpaceQuad() {
    GraphicsDevice.Clear(Tag(1, 1));
    // A left band with identity transforms, so the covered pixels are the clip
    // coordinates and nothing else. Not exactly the left half: an extent this
    // case alone uses cannot be confused with another case's geometry.
    var r = kRects[1];
    DrawQuad(Quad(r[0], r[1], r[2], r[3], Z(1, 1), Tag(1, 0)));
    ReadBackBuffer();
    Report.Line("CASE 1 clip-space quad       expect L={0} R={1} (clear)",
                Hex(Tag(1, 0)), Hex(Tag(1, 1)));
    SampleGrid("got");
  }

  private void Case2_Perspective() {
    GraphicsDevice.Clear(Tag(2, 1));
    float aspect = (float)kWidth / kHeight;
    // Every number here belongs to this case alone - not PiOver4, not a near
    // of 1 and a far of 100, not a camera ten units back. A projection that
    // arrives from somewhere else is then obvious in the coverage rather than
    // producing the same picture by coincidence.
    const float kFov = 0.8901f;      // about 51 degrees
    const float kNear = 0.7f;
    const float kFar = 137.0f;
    const float kEye = 7.3f;
    effect_.Projection =
        Matrix.CreatePerspectiveFieldOfView(kFov, aspect, kNear, kFar);
    effect_.View = Matrix.CreateLookAt(new Vector3(0, 0, kEye), Vector3.Zero,
                                       Vector3.Up);
    // Half the visible half-height at the plane the quad sits on, so the quad
    // covers the middle half: the centre is inside and the eighths are well
    // outside. If W is mishandled the coverage changes shape immediately.
    float h = (float)Math.Tan(kFov / 2.0) * kEye * 0.5f;
    DrawQuad(Quad(-h * aspect, -h, h * aspect, h, 0f, Tag(2, 0)));
    ReadBackBuffer();
    Report.Line(
        "CASE 2 perspective quad      expect centre={0} everything else {1} " +
        "(fov {2:F4} eye {3:F1} half-height {4:F3})", Hex(Tag(2, 0)),
        Hex(Tag(2, 1)), kFov, kEye, h);
    SampleGrid("got");
  }

  private void Case3_Textured() {
    // Blue, so it is none of the four texels.
    GraphicsDevice.Clear(Tag(3, 5));
    effect_.VertexColorEnabled = false;
    effect_.TextureEnabled = true;
    effect_.Texture = checker_;
    GraphicsDevice.SamplerStates[0] = SamplerState.PointClamp;
    DrawQuad(TexQuad(-1f, -1f, 1f, 1f, Z(3, 1)));
    ReadBackBuffer();
    Report.Line("CASE 3 textured fullscreen   expect TL={0} TR={1} BL={2} BR={3}",
                Hex(Tag(3, 0)), Hex(Tag(3, 1)), Hex(Tag(3, 2)), Hex(Tag(3, 3)));
    Report.Line("  quadrants      TL={0} TR={1} BL={2} BR={3}",
                Hex(At(kWidth / 4, kHeight / 4)),
                Hex(At(3 * kWidth / 4, kHeight / 4)),
                Hex(At(kWidth / 4, 3 * kHeight / 4)),
                Hex(At(3 * kWidth / 4, 3 * kHeight / 4)));
  }

  private void Case4_RenderTarget() {
    var rt = rt_single_;
    GraphicsDevice.SetRenderTarget(rt);
    GraphicsDevice.Clear(Tag(4, 1));
    var r4 = kRects[4];
    DrawQuad(Quad(r4[0], r4[1], r4[2], r4[3], Z(4, 1), Tag(4, 0)));
    // Unbound BEFORE it is read: a target still set on the device cannot be
    // read back, and cannot be sampled as a texture either.
    GraphicsDevice.SetRenderTarget(null);

    if (Report.Enabled) {
      rt.GetData(rt_readback_, 0, kSingleW * kSingleH);
      Report.Line(
          "CASE 4 render target         {0}x{1}, expect inRT L={2} R={3}",
          kSingleW, kSingleH, Hex(Tag(4, 0)), Hex(Tag(4, 1)));
      Report.Line("  in the target  L={0} R={1}",
                  Hex(rt_readback_[(kSingleH / 2) * kSingleW + kSingleW / 4]),
                  Hex(rt_readback_[(kSingleH / 2) * kSingleW +
                                   3 * kSingleW / 4]));
    }

    // And back out: the target sampled onto the backbuffer, which is the step
    // that needs the resolve to have actually happened. Cleared to its own tag
    // first, so a composite that draws nothing is distinguishable from one
    // that draws the wrong thing.
    GraphicsDevice.Clear(Tag(4, 2));
    effect_.VertexColorEnabled = false;
    effect_.TextureEnabled = true;
    effect_.Texture = rt;
    DrawQuad(TexQuad(-1f, -1f, 1f, 1f, Z(4, 3)));
    ReadBackBuffer();
    Report.Line("  composited     expect L={0} R={1}, never {2} (that clear " +
                "means the quad drew nothing)",
                Hex(Tag(4, 0)), Hex(Tag(4, 1)), Hex(Tag(4, 2)));
    SampleGrid("got");
  }

  private void Case5_Depth() {
    // Near and far are the pair being compared here, so they take red and
    // cyan and the clear steps aside to green.
    GraphicsDevice.Clear(Tag(5, 2));
    GraphicsDevice.DepthStencilState = DepthStencilState.Default;
    // Cleared to a depth this case alone uses, and above both quads so the
    // first one through cannot fail against it.
    const float kDepthClear = 0.938f;
    GraphicsDevice.Clear(ClearOptions.DepthBuffer, Tag(5, 2), kDepthClear, 0);
    // Near first, then far over the top of it, sharing one rectangle because
    // they have to overlap - the depths are what tell them apart. With
    // LessEqual the far one must lose, so the centre stays the near tag.
    var r5 = kRects[5];
    DrawQuad(Quad(r5[0], r5[1], r5[2], r5[3], Z(5, 1), Tag(5, 0)));
    DrawQuad(Quad(r5[0], r5[1], r5[2], r5[3], Z(5, 2), Tag(5, 1)));
    ReadBackBuffer();
    Report.Line(
        "CASE 5 depth test            expect centre={0} (near z={1:F3} beats " +
        "far z={2:F3}, clear {3:F3}); {4} means inverted, {5} means neither drew",
        Hex(Tag(5, 0)), Z(5, 1), Z(5, 2), kDepthClear, Hex(Tag(5, 1)),
        Hex(Tag(5, 2)));
    SampleGrid("got");
  }

  private void Case6_AlphaBlend() {
    var dst = Tag(6, 1);
    GraphicsDevice.Clear(Tag(6, 3));
    DrawQuad(Quad(-1f, -1f, 1f, 1f, Z(6, 0), dst));
    GraphicsDevice.BlendState = BlendState.AlphaBlend;
    // XNA 4.0's AlphaBlend is PREMULTIPLIED, so the source is supplied already
    // multiplied and the result is src + dst*(1-srcA). Both operands are tags,
    // so a result equal to either one says the blend did not happen rather
    // than that it happened wrongly.
    // Not 0x80: a half that lands on a round number is the one value a broken
    // blend is most likely to produce by accident.
    const int kSrcAlpha = 0x8B;
    var raw = Tag(6, 0);
    var src = new Color((byte)(raw.R * kSrcAlpha / 255),
                        (byte)(raw.G * kSrcAlpha / 255),
                        (byte)(raw.B * kSrcAlpha / 255), (byte)kSrcAlpha);
    DrawQuad(Quad(-1f, -1f, 1f, 1f, Z(6, 1), src));
    ReadBackBuffer();
    // ROUNDED, not truncated. The blend unit rounds to nearest, so an
    // expectation computed with integer division is a byte low on two
    // channels and reports a mismatch that is only in the arithmetic here.
    int inv = 255 - kSrcAlpha;
    var expect = new Color((byte)(src.R + (dst.R * inv + 127) / 255),
                           (byte)(src.G + (dst.G * inv + 127) / 255),
                           (byte)(src.B + (dst.B * inv + 127) / 255),
                           (byte)0xFF);
    Report.Line(
        "CASE 6 alpha blend           expect centre={0}; {1} means the source " +
        "was ignored, {2} means it overwrote", Hex(expect), Hex(dst), Hex(src));
    SampleGrid("got");
  }

  private void Case7_MultipleRenderTargets() {
    var rt0 = rt_mrt0_;
    var rt1 = rt_mrt1_;
    GraphicsDevice.SetRenderTargets(new RenderTargetBinding(rt0),
                                    new RenderTargetBinding(rt1));
    // Distinct clears per target are not possible through one Clear, so the
    // shared clear tag is what target 1 must keep - BasicEffect writes COLOR0
    // only. Target 1 holding the DRAW tag would mean the write went to both;
    // holding case 4's tags would mean it is reading another case's EDRAM.
    GraphicsDevice.Clear(Tag(7, 1));
    DrawQuad(Quad(-1f, -1f, 1f, 1f, Z(7, 1), Tag(7, 0)));
    GraphicsDevice.SetRenderTarget(null);

    if (!Report.Enabled) {
      return;
    }
    Report.Line(
        "CASE 7 two render targets    {0}x{1}, expect rt0={2} (the draw), " +
        "rt1={3} (the clear - one shader writes COLOR0 only)", kMrtW, kMrtH,
        Hex(Tag(7, 0)), Hex(Tag(7, 1)));
    int centre = (kMrtH / 2) * kMrtW + kMrtW / 2;
    int corner = 4 * kMrtW + 4;
    rt0.GetData(rt_readback_, 0, kMrtW * kMrtH);
    Report.Line("  rt0 centre={0} corner={1}", Hex(rt_readback_[centre]),
                Hex(rt_readback_[corner]));
    rt1.GetData(rt_readback_, 0, kMrtW * kMrtH);
    Report.Line("  rt1 centre={0} corner={1}", Hex(rt_readback_[centre]),
                Hex(rt_readback_[corner]));
  }

  private void Case8_Cull() {
    // The two bands are what this case compares, so they take red and cyan
    // and the clear steps aside to green.
    GraphicsDevice.Clear(Tag(8, 2));
    GraphicsDevice.RasterizerState = RasterizerState.CullCounterClockwise;
    // Quad() emits one winding, so one of these two survives and the other is
    // dropped. Which half carries its tag states the convention outright. The
    // two bands differ in extent as well as in tag, so a survivor is
    // identifiable by the pixels it covers alone.
    var r8 = kRects[8];
    DrawQuad(Quad(r8[0], r8[1], r8[2], r8[3], Z(8, 1), Tag(8, 0)));
    GraphicsDevice.RasterizerState = RasterizerState.CullClockwise;
    DrawQuad(Quad(0.09f, -0.79f, 0.96f, 0.83f, Z(8, 2), Tag(8, 1)));
    ReadBackBuffer();
    Report.Line(
        "CASE 8 cull mode             L={0} means CullCounterClockwise kept " +
        "it; R={1} means CullClockwise did; {2} is the clear",
        Hex(Tag(8, 0)), Hex(Tag(8, 1)), Hex(Tag(8, 2)));
    SampleGrid("got");
  }

  private void Case9_LayeredFrame() {
    GraphicsDevice.SetRenderTarget(rt_scene_);
    GraphicsDevice.Clear(ClearOptions.DepthBuffer | ClearOptions.Stencil,
                         Tag(9, 2), 0.961f, 0);
    GraphicsDevice.DepthStencilState = DepthStencilState.None;
    DrawQuad(Quad(-1f, -1f, 1f, 1f, Z(9, 5), Tag(9, 5)));
    GraphicsDevice.DepthStencilState = DepthStencilState.Default;
    DrawQuad(Quad(-0.71f, -0.47f, 0.13f, 0.53f, Z(9, 0), Tag(9, 0)));
    DrawQuad(Quad(-0.17f, -0.61f, 0.69f, 0.39f, Z(9, 1), Tag(9, 1)));
    GraphicsDevice.SetRenderTarget(null);

    GraphicsDevice.Clear(Tag(9, 3));
    GraphicsDevice.DepthStencilState = DepthStencilState.None;
    effect_.VertexColorEnabled = false;
    effect_.TextureEnabled = true;
    effect_.Texture = rt_scene_;
    DrawQuad(TexQuad(-1f, -1f, 1f, 1f, Z(9, 3)));

    sprites_.Begin();
    sprites_.Draw(white_, new Rectangle(560, 300, 200, 90), Tag(9, 4));
    sprites_.End();

    ReadBackBuffer();
    Report.Line(
        "CASE 9 layered frame         BG {0} (depth off), near {1}, far {2} " +
        "(depth on) in a 1280x720 target, composited, sprite {3} on top; " +
        "{4} is the backbuffer clear (composite drew nothing)",
        Hex(Tag(9, 5)), Hex(Tag(9, 0)), Hex(Tag(9, 1)), Hex(Tag(9, 4)),
        Hex(Tag(9, 3)));
    Report.Line("  sprite    expect {0} got {1}", Hex(Tag(9, 4)),
                Hex(At(660, 345)));
    Report.Line("  overlap   expect {0} got {1}", Hex(Tag(9, 0)),
                Hex(At(600, 460)));
    Report.Line("  near only expect {0} got {1}", Hex(Tag(9, 0)),
                Hex(At(300, 250)));
    Report.Line("  far only  expect {0} got {1}", Hex(Tag(9, 1)),
                Hex(At(900, 400)));
    Report.Line("  bg        expect {0} got {1} and {2}", Hex(Tag(9, 5)),
                Hex(At(100, 650)), Hex(At(1200, 80)));
    SampleGrid("got");
  }

  private static readonly Color[] kCubeFaces = {
    new Color(220, 40, 40), new Color(40, 200, 220), new Color(40, 200, 60),
    new Color(210, 50, 210), new Color(230, 210, 40), new Color(50, 70, 230),
  };
  private static readonly Color kHudPanel = new Color(22, 26, 40);
  private static readonly Color kHudBorder = new Color(255, 255, 255);
  private static readonly Color kHudLetters = new Color(255, 200, 40);
  private static readonly Color kHudHealth = new Color(60, 210, 80);
  private static readonly Color kHudEnergy = new Color(70, 140, 250);
  private static readonly Color kHudRed = new Color(230, 40, 40);
  private static readonly Color kHudGreen = new Color(40, 230, 40);
  private static readonly Color kHudBlue = new Color(40, 40, 230);
  private static readonly Color kHudSlot = new Color(220, 180, 50);

  private Texture2D LoadSky() {
    try {
      using (var stream =
                 typeof(ProbeGame).Assembly.GetManifestResourceStream("sky.bgra"))
      using (var reader = new System.IO.BinaryReader(stream)) {
        int width = reader.ReadInt32();
        int height = reader.ReadInt32();
        byte[] bytes = reader.ReadBytes(width * height * 4);
        var pixels = new Color[width * height];
        for (int i = 0; i < pixels.Length; ++i) {
          pixels[i] = new Color(bytes[i * 4 + 2], bytes[i * 4 + 1], bytes[i * 4], 255);
        }
        var texture = new Texture2D(GraphicsDevice, width, height, false,
                                    SurfaceFormat.Color);
        texture.SetData(pixels);
        Report.Line("sky loaded: {0}x{1}, first texel {2}", width, height,
                    Hex(pixels[0]));
        return texture;
      }
    } catch (Exception e) {
      Report.Line("sky FAILED {0}: {1} - sky is flat brown instead",
                  e.GetType().Name, e.Message);
      var texture = new Texture2D(GraphicsDevice, 1, 1, false, SurfaceFormat.Color);
      texture.SetData(new[] { new Color(110, 70, 30) });
      return texture;
    }
  }

  private static void AddSkyFace(
      System.Collections.Generic.List<VertexPositionNormalTexture> list,
      Vector3 normal, Vector3 u, Vector3 v) {
    list.Add(new VertexPositionNormalTexture(normal - u - v, -normal, new Vector2(0, 0)));
    list.Add(new VertexPositionNormalTexture(normal + u - v, -normal, new Vector2(1, 0)));
    list.Add(new VertexPositionNormalTexture(normal - u + v, -normal, new Vector2(0, 1)));
    list.Add(new VertexPositionNormalTexture(normal + u + v, -normal, new Vector2(1, 1)));
  }

  private static void AddCubeFace(
      System.Collections.Generic.List<VertexPositionColor> list, Vector3 normal,
      Vector3 u, Vector3 v, Color color) {
    const float s = 0.8f;
    list.Add(new VertexPositionColor((normal - u - v) * s, color));
    list.Add(new VertexPositionColor((normal + u - v) * s, color));
    list.Add(new VertexPositionColor((normal - u + v) * s, color));
    list.Add(new VertexPositionColor((normal + u + v) * s, color));
  }

  private static short[] QuadIndices(int quads) {
    var indices = new short[quads * 6];
    for (int q = 0; q < quads; ++q) {
      int b = q * 4;
      indices[q * 6 + 0] = (short)(b + 0);
      indices[q * 6 + 1] = (short)(b + 1);
      indices[q * 6 + 2] = (short)(b + 2);
      indices[q * 6 + 3] = (short)(b + 1);
      indices[q * 6 + 4] = (short)(b + 3);
      indices[q * 6 + 5] = (short)(b + 2);
    }
    return indices;
  }

  private void Bar(int x, int y, int w, int h, Color color) {
    sprites_.Draw(white_, new Rectangle(x, y, w, h), color);
  }

  private void DrawHud() {
    sprites_.Begin();
    Bar(0, 540, kWidth, 180, kHudPanel);
    Bar(0, 540, kWidth, 6, kHudBorder);

    const int s = 14;
    const int w = 60;
    const int h = 100;
    const int y = 572;
    Bar(40, y, s, h, kHudLetters);
    Bar(40 + w - s, y, s, h, kHudLetters);
    Bar(40, y + h / 2 - s / 2, w, s, kHudLetters);
    Bar(120, y, s, h, kHudLetters);
    Bar(120 + w - s, y, s, h, kHudLetters);
    Bar(120, y + h - s, w, s, kHudLetters);
    Bar(200, y, s, h, kHudLetters);
    Bar(200, y, w - s, s, kHudLetters);
    Bar(200, y + h - s, w - s, s, kHudLetters);
    Bar(200 + w - s, y + s, s, h - 2 * s, kHudLetters);

    Bar(300, 580, 420, 30, new Color(70, 20, 20));
    Bar(300, 580, 315, 30, kHudHealth);
    Bar(300, 630, 420, 30, new Color(20, 30, 70));
    Bar(300, 630, 210, 30, kHudEnergy);

    Bar(760, 575, 70, 70, kHudRed);
    Bar(845, 575, 70, 70, kHudGreen);
    Bar(930, 575, 70, 70, kHudBlue);
    for (int i = 0; i < 8; ++i) {
      int g = i * 255 / 7;
      Bar(760 + i * 30, 665, 30, 30, new Color(g, g, g));
    }

    for (int i = 0; i < 4; ++i) {
      Bar(1040 + i * 56, 590, 46, 46, kHudSlot);
      Bar(1040 + i * 56, 660, 56, 30, (i & 1) == 0 ? Color.White : Color.Black);
    }
    sprites_.End();
  }

  private void SceneMatrices(out Matrix view, out Matrix projection) {
    view = Matrix.CreateLookAt(new Vector3(0, 0.9f, 4.4f),
                               new Vector3(0, -0.55f, 0), Vector3.Up);
    projection = Matrix.CreatePerspectiveFieldOfView(
        MathHelper.ToRadians(60f), (float)kWidth / kHeight, 0.1f, 100f);
  }

  private void DrawSkyPass(Matrix view, Matrix projection) {
    Matrix sky_view = view;
    sky_view.Translation = Vector3.Zero;
    GraphicsDevice.BlendState = BlendState.Opaque;
    GraphicsDevice.DepthStencilState = DepthStencilState.None;
    GraphicsDevice.RasterizerState = RasterizerState.CullNone;
    GraphicsDevice.SamplerStates[0] = SamplerState.LinearClamp;
    effect_.World = Matrix.CreateScale(10f);
    effect_.View = sky_view;
    effect_.Projection = projection;
    effect_.VertexColorEnabled = false;
    effect_.TextureEnabled = true;
    effect_.Texture = sky_;
    GraphicsDevice.SetVertexBuffer(sky_vertices_);
    GraphicsDevice.Indices = sky_indices_;
    foreach (EffectPass pass in effect_.CurrentTechnique.Passes) {
      pass.Apply();
      GraphicsDevice.DrawIndexedPrimitives(PrimitiveType.TriangleList, 0, 0, 24,
                                           0, 12);
    }
    GraphicsDevice.SetVertexBuffer(null);
    GraphicsDevice.Indices = null;
  }

  private void DrawCubePass(Matrix view, Matrix projection, float t,
                            bool depth) {
    GraphicsDevice.BlendState = BlendState.Opaque;
    GraphicsDevice.DepthStencilState =
        depth ? DepthStencilState.Default : DepthStencilState.None;
    GraphicsDevice.RasterizerState = RasterizerState.CullNone;
    effect_.World = Matrix.CreateRotationY(t * 0.6f) * Matrix.CreateRotationX(t * 0.35f);
    effect_.View = view;
    effect_.Projection = projection;
    effect_.TextureEnabled = false;
    effect_.VertexColorEnabled = true;
    GraphicsDevice.SetVertexBuffer(cube_vertices_);
    GraphicsDevice.Indices = cube_indices_;
    foreach (EffectPass pass in effect_.CurrentTechnique.Passes) {
      pass.Apply();
      GraphicsDevice.DrawIndexedPrimitives(PrimitiveType.TriangleList, 0, 0, 24,
                                           0, 12);
    }
    GraphicsDevice.SetVertexBuffer(null);
    GraphicsDevice.Indices = null;
  }

  private void ReportHud() {
    Report.Line(
        "  hud border={0} H={1} health={2} energy={3} R={4} G={5} B={6} " +
        "ramp0={7} ramp7={8} slot={9} panel={10}",
        Hex(At(640, 542)), Hex(At(47, 600)), Hex(At(400, 595)),
        Hex(At(400, 645)), Hex(At(795, 610)), Hex(At(880, 610)),
        Hex(At(965, 610)), Hex(At(775, 680)), Hex(At(985, 680)),
        Hex(At(1063, 613)), Hex(At(700, 708)));
    Report.Line(
        "  expect border={0} H={1} health={2} energy={3} R={4} G={5} B={6} " +
        "ramp0=000000FF ramp7=FFFFFFFF slot={7} panel={8}",
        Hex(kHudBorder), Hex(kHudLetters), Hex(kHudHealth), Hex(kHudEnergy),
        Hex(kHudRed), Hex(kHudGreen), Hex(kHudBlue), Hex(kHudSlot),
        Hex(kHudPanel));
  }

  private string CubeFaceList() {
    return Hex(kCubeFaces[0]) + " " + Hex(kCubeFaces[1]) + " " +
           Hex(kCubeFaces[2]) + " " + Hex(kCubeFaces[3]) + " " +
           Hex(kCubeFaces[4]) + " " + Hex(kCubeFaces[5]);
  }

  private void Case10_Hud() {
    GraphicsDevice.Clear(ClearOptions.Target | ClearOptions.DepthBuffer,
                         Tag(10, 3), 1f, 0);
    DrawHud();
    if (!Report.Enabled) {
      return;
    }
    ReadBackBuffer();
    Report.Line(
        "CASE 10 hud only             frame {0}: above the hud expect {1} " +
        "(the clear) got {2}", frame_, Hex(Tag(10, 3)), Hex(At(640, 200)));
    ReportHud();
    Report.Flush();
  }

  private void Case11_Cube() {
    GraphicsDevice.Clear(ClearOptions.Target | ClearOptions.DepthBuffer,
                         Tag(11, 3), 1f, 0);
    Matrix view;
    Matrix projection;
    SceneMatrices(out view, out projection);
    bool depth = case_frame_ > kSceneFrames / 2;
    DrawCubePass(view, projection, frame_ / 60f, depth);
    bool halfway = case_frame_ == kSceneFrames / 2;
    if (!Report.Enabled && !halfway) {
      return;
    }
    Report.Enabled = true;
    ReadBackBuffer();
    Vector3 centre = GraphicsDevice.Viewport.Project(Vector3.Zero, projection,
                                                     view, Matrix.Identity);
    int cx = (int)centre.X;
    int cy = (int)centre.Y;
    Report.Line(
        "CASE 11 cube only, depth {0}  frame {1}: cube({2},{3})={4} expect one " +
        "of {5}; background got {6} expect {7}", depth ? "on " : "off", frame_,
        cx, cy, Hex(At(cx, cy)), CubeFaceList(), Hex(At(100, 100)),
        Hex(Tag(11, 3)));
    Report.Flush();
    if (halfway) {
      Report.Enabled = false;
    }
  }

  private void Case12_Sky() {
    GraphicsDevice.Clear(ClearOptions.Target | ClearOptions.DepthBuffer,
                         Tag(12, 3), 1f, 0);
    Matrix view;
    Matrix projection;
    SceneMatrices(out view, out projection);
    DrawSkyPass(view, projection);
    if (!Report.Enabled) {
      return;
    }
    ReadBackBuffer();
    Report.Line(
        "CASE 12 sky only             frame {0}: sky={1} sky2={2} centre={3} " +
        "low={4}; never {5} (the clear)", frame_, Hex(At(200, 150)),
        Hex(At(1100, 150)), Hex(At(640, 360)), Hex(At(640, 650)),
        Hex(Tag(12, 3)));
    Report.Flush();
  }

  private void Case13_Full() {
    float t = frame_ / 60f;

    GraphicsDevice.Clear(ClearOptions.Target | ClearOptions.DepthBuffer,
                         Tag(13, 3), 1f, 0);

    Matrix view = Matrix.CreateLookAt(new Vector3(0, 0.9f, 4.4f),
                                      new Vector3(0, -0.55f, 0), Vector3.Up);
    Matrix projection = Matrix.CreatePerspectiveFieldOfView(
        MathHelper.ToRadians(60f), (float)kWidth / kHeight, 0.1f, 100f);

    Matrix sky_view = view;
    sky_view.Translation = Vector3.Zero;
    GraphicsDevice.BlendState = BlendState.Opaque;
    GraphicsDevice.DepthStencilState = DepthStencilState.None;
    GraphicsDevice.RasterizerState = RasterizerState.CullNone;
    GraphicsDevice.SamplerStates[0] = SamplerState.LinearClamp;
    effect_.World = Matrix.CreateScale(10f);
    effect_.View = sky_view;
    effect_.Projection = projection;
    effect_.VertexColorEnabled = false;
    effect_.TextureEnabled = true;
    effect_.Texture = sky_;
    GraphicsDevice.SetVertexBuffer(sky_vertices_);
    GraphicsDevice.Indices = sky_indices_;
    foreach (EffectPass pass in effect_.CurrentTechnique.Passes) {
      pass.Apply();
      GraphicsDevice.DrawIndexedPrimitives(PrimitiveType.TriangleList, 0, 0, 24,
                                           0, 12);
    }

    GraphicsDevice.DepthStencilState = DepthStencilState.Default;
    effect_.World = Matrix.CreateRotationY(t * 0.6f) * Matrix.CreateRotationX(t * 0.35f);
    effect_.View = view;
    effect_.TextureEnabled = false;
    effect_.VertexColorEnabled = true;
    GraphicsDevice.SetVertexBuffer(cube_vertices_);
    GraphicsDevice.Indices = cube_indices_;
    foreach (EffectPass pass in effect_.CurrentTechnique.Passes) {
      pass.Apply();
      GraphicsDevice.DrawIndexedPrimitives(PrimitiveType.TriangleList, 0, 0, 24,
                                           0, 12);
    }
    GraphicsDevice.SetVertexBuffer(null);
    GraphicsDevice.Indices = null;

    DrawHud();

    if (!Report.Enabled) {
      return;
    }
    ReadBackBuffer();
    Vector3 centre = GraphicsDevice.Viewport.Project(Vector3.Zero, projection, view,
                                                     Matrix.Identity);
    int cx = (int)centre.X;
    int cy = (int)centre.Y;
    Report.Line(
        "CASE 13 full scene frame {0}: sky={1} sky2={2} cube({3},{4})={5} " +
        "clear-if-sky-missing={6}", frame_, Hex(At(200, 150)),
        Hex(At(1100, 150)), cx, cy, Hex(At(cx, cy)), Hex(Tag(13, 3)));
    Report.Line(
        "  hud border={0} H={1} health={2} energy={3} R={4} G={5} B={6} " +
        "ramp0={7} ramp7={8} slot={9} panel={10}",
        Hex(At(640, 542)), Hex(At(47, 600)), Hex(At(400, 595)),
        Hex(At(400, 645)), Hex(At(795, 610)), Hex(At(880, 610)),
        Hex(At(965, 610)), Hex(At(775, 680)), Hex(At(985, 680)),
        Hex(At(1063, 613)), Hex(At(700, 708)));
    Report.Line(
        "  expect border={0} H={1} health={2} energy={3} R={4} G={5} B={6} " +
        "ramp0=000000FF ramp7=FFFFFFFF slot={7} panel={8}; cube is one of " +
        "{9} {10} {11} {12} {13} {14}",
        Hex(kHudBorder), Hex(kHudLetters), Hex(kHudHealth), Hex(kHudEnergy),
        Hex(kHudRed), Hex(kHudGreen), Hex(kHudBlue), Hex(kHudSlot),
        Hex(kHudPanel), Hex(kCubeFaces[0]), Hex(kCubeFaces[1]),
        Hex(kCubeFaces[2]), Hex(kCubeFaces[3]), Hex(kCubeFaces[4]),
        Hex(kCubeFaces[5]));
    Report.Flush();
  }

  // ---- driver --------------------------------------------------------------

  protected override void Draw(GameTime gameTime) {
    if (finished_) {
      return;
    }
    ++frame_;
    if (frame_ <= kWarmupFrames) {
      GraphicsDevice.Clear(Color.Black);
      return;
    }
    // THE CASE IS DRAWN EVERY FRAME, and only the frame that completes it
    // reports and advances. Returning early on the other frames instead would
    // leave Present flipping between two buffers holding different cases,
    // which is a two-colour strobe at half the refresh rate.
    ++case_frame_;
    Report.Enabled =
        case_frame_ % (case_index_ >= 10 ? kSceneFrames : kFramesPerCase) == 0;

    Baseline();
    try {
      switch (case_index_) {
        case 0: Case0_Clear(); break;
        case 1: Case1_ClipSpaceQuad(); break;
        case 2: Case2_Perspective(); break;
        case 3: Case3_Textured(); break;
        case 4: Case4_RenderTarget(); break;
        case 5: Case5_Depth(); break;
        case 6: Case6_AlphaBlend(); break;
        case 7: Case7_MultipleRenderTargets(); break;
        case 8: Case8_Cull(); break;
        case 9: Case9_LayeredFrame(); break;
        case 10: Case10_Hud(); break;
        case 11: Case11_Cube(); break;
        case 12: Case12_Sky(); break;
        case 13: Case13_Full(); break;
        default:
          Report.Enabled = true;
          Report.Line("");
          Report.Line("=== {0} case(s) complete ===", case_index_);
          Report.Flush();
          finished_ = true;
          Exit();
          return;
      }
    } catch (Exception e) {
      // A case that throws is a result too - it says the hosted path is missing
      // something the PC runtime has, and the run continues to the next case.
      Report.Line("CASE {0} THREW {1}: {2}", case_index_, e.GetType().Name,
                  e.Message);
      // A case that died partway may have left a target bound, and Present
      // cannot run against one - which would take the whole run down with it.
      try {
        GraphicsDevice.SetRenderTarget(null);
      } catch {
      }
    }
    if (Report.Enabled && case_index_ < 13) {
      ++case_index_;
      case_frame_ = 0;
    }
  }
}

}
