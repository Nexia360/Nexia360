using System;
using System.Collections.Generic;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;

namespace Nexia.XnaProbe {

internal sealed class SceneGame : Game {
  private const int kWidth = 1280;
  private const int kHeight = 720;
  private const int kReportEvery = 300;

  private static readonly Color kHealth = new Color(70, 200, 90);
  private static readonly Color kEnergy = new Color(80, 150, 240);
  private static readonly Color kCross = new Color(255, 255, 255);
  private static readonly Color kSlot = new Color(200, 170, 60);
  private static readonly Color kEdge = new Color(230, 120, 40);

  private readonly GraphicsDeviceManager graphics_;
  private BasicEffect effect_;
  private VertexBuffer sky_vertices_;
  private IndexBuffer sky_indices_;
  private Texture2D[] sky_faces_;
  private VertexBuffer cube_vertices_;
  private IndexBuffer cube_indices_;
  private Texture2D white_;
  private SpriteBatch sprites_;
  private Color[] readback_;
  private int frame_;

  public SceneGame(string[] args) {
    graphics_ = new GraphicsDeviceManager(this);
    graphics_.PreferredBackBufferWidth = kWidth;
    graphics_.PreferredBackBufferHeight = kHeight;
    graphics_.GraphicsProfile = GraphicsProfile.HiDef;
    graphics_.SynchronizeWithVerticalRetrace = true;
    IsFixedTimeStep = true;
  }

  protected override void LoadContent() {
    effect_ = new BasicEffect(GraphicsDevice);
    effect_.LightingEnabled = false;

    var sky = new List<VertexPositionNormalTexture>();
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

    Color[] markers = {
      new Color(210, 70, 70), new Color(70, 190, 90), new Color(220, 200, 70),
      new Color(190, 80, 200)
    };
    sky_faces_ = new Texture2D[6];
    for (int i = 0; i < 4; ++i) {
      sky_faces_[i] = SideTexture(markers[i]);
    }
    sky_faces_[4] = CheckerTexture(new Color(80, 140, 225), new Color(95, 155, 235));
    sky_faces_[5] = CheckerTexture(new Color(60, 80, 60), new Color(70, 92, 70));

    var cube = new List<VertexPositionColor>();
    AddCubeFace(cube, new Vector3(1, 0, 0), new Vector3(0, 0, -1), new Vector3(0, -1, 0), new Color(200, 60, 60));
    AddCubeFace(cube, new Vector3(-1, 0, 0), new Vector3(0, 0, 1), new Vector3(0, -1, 0), new Color(60, 200, 200));
    AddCubeFace(cube, new Vector3(0, 1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, 1), new Color(60, 200, 60));
    AddCubeFace(cube, new Vector3(0, -1, 0), new Vector3(1, 0, 0), new Vector3(0, 0, -1), new Color(200, 60, 200));
    AddCubeFace(cube, new Vector3(0, 0, 1), new Vector3(1, 0, 0), new Vector3(0, -1, 0), new Color(220, 200, 60));
    AddCubeFace(cube, new Vector3(0, 0, -1), new Vector3(-1, 0, 0), new Vector3(0, -1, 0), new Color(60, 60, 220));
    cube_vertices_ = new VertexBuffer(GraphicsDevice, typeof(VertexPositionColor),
                                      cube.Count, BufferUsage.WriteOnly);
    cube_vertices_.SetData(cube.ToArray());
    cube_indices_ = new IndexBuffer(GraphicsDevice, IndexElementSize.SixteenBits,
                                    36, BufferUsage.WriteOnly);
    cube_indices_.SetData(QuadIndices(6));

    white_ = new Texture2D(GraphicsDevice, 1, 1, false, SurfaceFormat.Color);
    white_.SetData(new[] { Color.White });
    sprites_ = new SpriteBatch(GraphicsDevice);
    readback_ = new Color[kWidth * kHeight];

    Report.Line("=== Nexia XNA scene: skybox, spinning cube, HUD ===");
    Report.Line("expect health={0} energy={1} cross={2} slot={3} edge={4}",
                Report.Pixel(kHealth), Report.Pixel(kEnergy), Report.Pixel(kCross),
                Report.Pixel(kSlot), Report.Pixel(kEdge));
    Report.Line("cube faces: C83C3CFF 3CC8C8FF 3CC83CFF C83CC8FF DCC83CFF 3C3CDCFF; " +
                "sky is a blue-to-peach gradient over green ground, never 28282CFF " +
                "(the clear)");
    Report.Flush();
  }

  private static void AddSkyFace(List<VertexPositionNormalTexture> list,
                                 Vector3 normal, Vector3 u, Vector3 v) {
    list.Add(new VertexPositionNormalTexture(normal - u - v, -normal, new Vector2(0, 0)));
    list.Add(new VertexPositionNormalTexture(normal + u - v, -normal, new Vector2(1, 0)));
    list.Add(new VertexPositionNormalTexture(normal - u + v, -normal, new Vector2(0, 1)));
    list.Add(new VertexPositionNormalTexture(normal + u + v, -normal, new Vector2(1, 1)));
  }

  private static void AddCubeFace(List<VertexPositionColor> list, Vector3 normal,
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

  private Texture2D SideTexture(Color marker) {
    const int n = 64;
    var top = new Color(70, 130, 220);
    var horizon = new Color(250, 190, 140);
    var ground = new Color(70, 90, 70);
    var pixels = new Color[n * n];
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        Color c;
        if (y < 40) {
          c = Color.Lerp(top, horizon, y / 40f);
        } else if (y < 44) {
          c = marker;
        } else {
          c = ground;
        }
        if (x % 16 == 0 || y % 16 == 0) {
          c = new Color((byte)(c.R * 0.85f), (byte)(c.G * 0.85f), (byte)(c.B * 0.85f));
        }
        pixels[y * n + x] = c;
      }
    }
    var texture = new Texture2D(GraphicsDevice, n, n, false, SurfaceFormat.Color);
    texture.SetData(pixels);
    return texture;
  }

  private Texture2D CheckerTexture(Color a, Color b) {
    const int n = 64;
    var pixels = new Color[n * n];
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        pixels[y * n + x] = ((x / 8 + y / 8) & 1) == 0 ? a : b;
      }
    }
    var texture = new Texture2D(GraphicsDevice, n, n, false, SurfaceFormat.Color);
    texture.SetData(pixels);
    return texture;
  }

  protected override void Draw(GameTime gameTime) {
    ++frame_;
    float t = frame_ / 60f;

    GraphicsDevice.Clear(ClearOptions.Target | ClearOptions.DepthBuffer |
                             ClearOptions.Stencil,
                         new Color(40, 40, 44), 1f, 0);

    Matrix view = Matrix.CreateLookAt(new Vector3(0, 1.1f, 4.2f), Vector3.Zero,
                                      Vector3.Up);
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
    GraphicsDevice.SetVertexBuffer(sky_vertices_);
    GraphicsDevice.Indices = sky_indices_;
    for (int face = 0; face < 6; ++face) {
      effect_.Texture = sky_faces_[face];
      foreach (EffectPass pass in effect_.CurrentTechnique.Passes) {
        pass.Apply();
        GraphicsDevice.DrawIndexedPrimitives(PrimitiveType.TriangleList, 0, 0,
                                             24, face * 6, 2);
      }
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

    sprites_.Begin();
    sprites_.Draw(white_, new Rectangle(24, 24, 380, 104), new Color(0, 0, 0, 150));
    sprites_.Draw(white_, new Rectangle(40, 44, 340, 22), new Color(60, 20, 20));
    sprites_.Draw(white_, new Rectangle(40, 44, 250, 22), kHealth);
    sprites_.Draw(white_, new Rectangle(40, 82, 340, 22), new Color(20, 30, 60));
    sprites_.Draw(white_, new Rectangle(40, 82, 170, 22), kEnergy);
    sprites_.Draw(white_, new Rectangle(612, 358, 20, 4), kCross);
    sprites_.Draw(white_, new Rectangle(648, 358, 20, 4), kCross);
    sprites_.Draw(white_, new Rectangle(638, 336, 4, 18), kCross);
    sprites_.Draw(white_, new Rectangle(638, 366, 4, 18), kCross);
    for (int i = 0; i < 5; ++i) {
      sprites_.Draw(white_, new Rectangle(900 + i * 68, 620, 56, 56), kSlot);
    }
    sprites_.Draw(white_, new Rectangle(0, 0, kWidth, 4), kEdge);
    sprites_.Draw(white_, new Rectangle(0, kHeight - 4, kWidth, 4), kEdge);
    sprites_.Draw(white_, new Rectangle(0, 0, 4, kHeight), kEdge);
    sprites_.Draw(white_, new Rectangle(kWidth - 4, 0, 4, kHeight), kEdge);
    sprites_.End();

    if (frame_ % kReportEvery == 0) {
      GraphicsDevice.GetBackBufferData(readback_);
      Report.Line(
          "frame {0} sky={1} sky2={2} ground={3} cube={4} panel={5} health={6} " +
          "energy={7} cross={8} slot={9} edge={10}",
          frame_, Sample(200, 200), Sample(1100, 150), Sample(200, 600),
          Sample(640, 360), Sample(30, 32), Sample(100, 55), Sample(100, 93),
          Sample(620, 360), Sample(928, 648), Sample(640, 1));
      Report.Flush();
    }
    base.Draw(gameTime);
  }

  private string Sample(int x, int y) {
    return Report.Pixel(readback_[y * kWidth + x]);
  }
}

}
