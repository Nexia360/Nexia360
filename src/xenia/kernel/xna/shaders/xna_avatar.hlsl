cbuffer AvatarConstants : register(b0) {
  row_major float4x4 world_view_projection;
  row_major float4x4 world;
  float4 light_direction;
  float4 light_color;
  float4 ambient_color;
  float4 base_color;
  float4 custom_color[3];
  float4 layer_tint[6];
  uint4 layer_info[6];
};

Texture2DArray layer_texture0 : register(t0);
Texture2DArray layer_texture1 : register(t1);
Texture2DArray layer_texture2 : register(t2);
Texture2DArray layer_texture3 : register(t3);
Texture2DArray layer_texture4 : register(t4);
Texture2DArray layer_texture5 : register(t5);
SamplerState wrap_sampler : register(s0);
SamplerState clamp_sampler : register(s1);

struct VSInput {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float2 uv0 : TEXCOORD0;
  float2 uv1 : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
  float2 uv3 : TEXCOORD3;
  float2 uv4 : TEXCOORD4;
  float2 uv5 : TEXCOORD5;
};

struct VSOutput {
  float4 position : SV_Position;
  float3 normal : NORMAL;
  float2 uv0 : TEXCOORD0;
  float2 uv1 : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
  float2 uv3 : TEXCOORD3;
  float2 uv4 : TEXCOORD4;
  float2 uv5 : TEXCOORD5;
};

VSOutput vs_main(VSInput input) {
  VSOutput output;
  output.position = mul(float4(input.position, 1.0f), world_view_projection);
  output.normal = mul(input.normal, (float3x3)world);
  output.uv0 = input.uv0;
  output.uv1 = input.uv1;
  output.uv2 = input.uv2;
  output.uv3 = input.uv3;
  output.uv4 = input.uv4;
  output.uv5 = input.uv5;
  return output;
}

float4 SampleLayer(Texture2DArray layer_texture, uint4 info, float2 uv[6]) {
  float3 coord = float3(uv[min(info.y, 5u)], float(info.z));
  if (info.w != 0) {
    return layer_texture.Sample(clamp_sampler, coord);
  }
  return layer_texture.Sample(wrap_sampler, coord);
}

float3 ApplyLayer(float3 color, inout float alpha, uint4 info, float4 tint,
                  float4 s) {
  if (info.x == 1) {
    alpha = s.a;
    return s.rgb;
  }
  if (info.x == 2) {
    float3 mixed = custom_color[0].rgb * s.r + custom_color[1].rgb * s.g +
                   custom_color[2].rgb * s.b;
    return lerp(color, mixed, s.a);
  }
  if (info.x == 3) {
    return lerp(color, s.rgb, s.a);
  }
  if (info.x == 4) {
    float3 feature = min(1.0f, tint.rgb * s.r + s.g + s.b);
    return lerp(color, feature, s.a);
  }
  return color;
}

float4 ps_main(VSOutput input) : SV_Target {
  float2 uv[6] = {input.uv0, input.uv1, input.uv2,
                  input.uv3, input.uv4, input.uv5};
  float3 color = base_color.rgb;
  float alpha = 1.0f;
  if (layer_info[0].x != 0) {
    color = ApplyLayer(color, alpha, layer_info[0], layer_tint[0],
                       SampleLayer(layer_texture0, layer_info[0], uv));
  }
  if (layer_info[1].x != 0) {
    color = ApplyLayer(color, alpha, layer_info[1], layer_tint[1],
                       SampleLayer(layer_texture1, layer_info[1], uv));
  }
  if (layer_info[2].x != 0) {
    color = ApplyLayer(color, alpha, layer_info[2], layer_tint[2],
                       SampleLayer(layer_texture2, layer_info[2], uv));
  }
  if (layer_info[3].x != 0) {
    color = ApplyLayer(color, alpha, layer_info[3], layer_tint[3],
                       SampleLayer(layer_texture3, layer_info[3], uv));
  }
  if (layer_info[4].x != 0) {
    color = ApplyLayer(color, alpha, layer_info[4], layer_tint[4],
                       SampleLayer(layer_texture4, layer_info[4], uv));
  }
  if (layer_info[5].x != 0) {
    color = ApplyLayer(color, alpha, layer_info[5], layer_tint[5],
                       SampleLayer(layer_texture5, layer_info[5], uv));
  }
  clip(alpha - 0.5f);
  float3 n = normalize(input.normal);
  float diffuse = saturate(-dot(n, normalize(light_direction.xyz)));
  return float4(saturate(color * (ambient_color.rgb +
                                  light_color.rgb * diffuse)),
                1.0f);
}
