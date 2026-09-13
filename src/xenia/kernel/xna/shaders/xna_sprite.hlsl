// The one pipeline a hosted XNA title needs to put its 2D output on screen.
//
// SpriteBatch is how these titles draw: a texture, a source rectangle and a
// destination rectangle per sprite. Positions arrive in SCREEN PIXELS, because
// that is what the command stream carries, and are turned into clip space here
// rather than on the CPU - so the same vertex data works whatever the output
// size turns out to be.
//
// Compiled offline with fxc and embedded as bytecode. Nothing loads
// D3DCompiler_47.dll at runtime: it is an optional redistributable, and a title
// should not fail to draw because a machine happens not to have it.

cbuffer Constants : register(b0) {
  // 1 / viewport size, so pixels become the 0..1 range clip space is built from.
  float2 inverse_viewport;
  float2 padding;
};

Texture2D sprite_texture : register(t0);
SamplerState sprite_sampler : register(s0);

struct VSInput {
  float2 position : POSITION;   // screen pixels
  float2 texcoord : TEXCOORD0;  // 0..1
  float4 color : COLOR0;
};

struct VSOutput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

VSOutput vs_main(VSInput input) {
  VSOutput output;
  // Pixels to clip space. Y is flipped because screen coordinates grow
  // downwards and clip space grows upwards.
  float2 unit = input.position * inverse_viewport;
  output.position = float4(unit.x * 2.0f - 1.0f, 1.0f - unit.y * 2.0f, 0.0f,
                           1.0f);
  output.texcoord = input.texcoord;
  output.color = input.color;
  return output;
}

float4 ps_main(VSOutput input) : SV_Target {
  return sprite_texture.Sample(sprite_sampler, input.texcoord) * input.color;
}
