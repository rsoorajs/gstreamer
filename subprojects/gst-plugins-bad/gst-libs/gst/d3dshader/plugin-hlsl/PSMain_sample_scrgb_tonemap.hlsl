/* GStreamer
 * Copyright (C) 2025 Seungha Yang <seungha@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef BUILDING_HLSL
Texture2D shaderTexture;
SamplerState samplerState;

cbuffer PsConstBuffer
{
  float sdr_white_level;
};

struct PS_INPUT
{
  float4 Position : SV_POSITION;
  float2 Texture : TEXCOORD;
};

float srgb_encode (float val)
{
  if (val <= 0.0031308)
    return 12.92 * val;
  else
    return 1.055 * pow (val, 1.0 / 2.4) - 0.055;
}

float4 ENTRY_POINT (PS_INPUT input): SV_TARGET
{
  float4 sample = shaderTexture.Sample (samplerState, input.Texture).rgba;

  sample.rgb = sample.rgb / sdr_white_level;
  sample.r = sample.r / (1.0f + sample.r) * 80.f;
  sample.g = sample.g / (1.0f + sample.g) * 80.f;
  sample.b = sample.b / (1.0f + sample.b) * 80.f;

  sample = saturate (sample);
  sample.r = srgb_encode (sample.r);
  sample.g = srgb_encode (sample.g);
  sample.b = srgb_encode (sample.b);

  return sample;
}
#else
static const char str_PSMain_sample_scrgb_tonemap[] =
"Texture2D shaderTexture;\n"
"SamplerState samplerState;\n"
"\n"
"cbuffer PsConstBuffer\n"
"{\n"
"  float sdr_white_level;\n"
"};\n"
"\n"
"struct PS_INPUT\n"
"{\n"
"  float4 Position : SV_POSITION;\n"
"  float2 Texture : TEXCOORD;\n"
"};\n"
"\n"
"float srgb_encode (float val)\n"
"{\n"
"  if (val <= 0.0031308)\n"
"    return 12.92 * val;\n"
"  else\n"
"    return 1.055 * pow (val, 1.0 / 2.4) - 0.055;\n"
"}\n"
"\n"
"float4 ENTRY_POINT (PS_INPUT input): SV_TARGET\n"
"{\n"
"  float4 sample = shaderTexture.Sample (samplerState, input.Texture).rgba;\n"
"\n"
"  sample.rgb = sample.rgb / sdr_white_level;\n"
"  sample.r = sample.r / (1.0f + sample.r) * 80.f;\n"
"  sample.g = sample.g / (1.0f + sample.g) * 80.f;\n"
"  sample.b = sample.b / (1.0f + sample.b) * 80.f;\n"
"\n"
"  sample = saturate (sample);\n"
"  sample.r = srgb_encode (sample.r);\n"
"  sample.g = srgb_encode (sample.g);\n"
"  sample.b = srgb_encode (sample.b);\n"
"\n"
"  return sample;\n"
"}\n";
#endif
