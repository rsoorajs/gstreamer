/* GStreamer
 * Copyright (C) 2023 Seungha Yang <seungha@centricular.com>
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

#pragma once

#include "PSMain_checker_luma.hlsl"
#include "PSMain_checker_rgb.hlsl"
#include "PSMain_checker_vuya.hlsl"
#include "PSMain_checker.hlsl"
#include "PSMain_color.hlsl"
#include "PSMain_sample_premul.hlsl"
#include "PSMain_sample.hlsl"
#include "PSMain_sample_scrgb_tonemap.hlsl"
#include "PSMain_sample_scrgb.hlsl"
#include "PSMain_snow.hlsl"
#include "VSMain_color.hlsl"
#include "VSMain_coord.hlsl"
#include "VSMain_pos.hlsl"
#include "CSMain_mipgen.hlsl"
#include "CSMain_mipgen_vuya.hlsl"
#include "CSMain_mipgen_ayuv.hlsl"
#include "CSMain_mipgen_gray.hlsl"
#include "CSMain_yadif_1.hlsl"
#include "CSMain_yadif_1_10.hlsl"
#include "CSMain_yadif_1_12.hlsl"
#include "CSMain_yadif_2.hlsl"
#include "CSMain_yadif_4.hlsl"
#include "CSMain_fisheye_equirect.hlsl"
#include "CSMain_fisheye_panorama.hlsl"
#include "CSMain_fisheye_perspective.hlsl"
