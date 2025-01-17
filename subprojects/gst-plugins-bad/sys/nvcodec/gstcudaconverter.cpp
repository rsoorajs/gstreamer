/* GStreamer
 * Copyright (C) 2022 Seungha Yang <seungha@centricular.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstcudaconverter.h"
#include <string.h>
#include <mutex>

GST_DEBUG_CATEGORY_STATIC (gst_cuda_converter_debug);
#define GST_CAT_DEFAULT gst_cuda_converter_debug

#define CUDA_BLOCK_X 16
#define CUDA_BLOCK_Y 16
#define DIV_UP(size,block) (((size) + ((block) - 1)) / (block))

/* from GstD3D11 */
struct GstCudaColorMatrix
{
  gdouble matrix[3][3];
  gdouble offset[3];
  gdouble min[3];
  gdouble max[3];
};

static gchar *
gst_cuda_dump_color_matrix (GstCudaColorMatrix * matrix)
{
  /* *INDENT-OFF* */
  static const gchar format[] =
      "[MATRIX]\n"
      "|% .6f, % .6f, % .6f|\n"
      "|% .6f, % .6f, % .6f|\n"
      "|% .6f, % .6f, % .6f|\n"
      "[OFFSET]\n"
      "|% .6f, % .6f, % .6f|\n"
      "[MIN]\n"
      "|% .6f, % .6f, % .6f|\n"
      "[MAX]\n"
      "|% .6f, % .6f, % .6f|";
  /* *INDENT-ON* */

  return g_strdup_printf (format,
      matrix->matrix[0][0], matrix->matrix[0][1], matrix->matrix[0][2],
      matrix->matrix[1][0], matrix->matrix[1][1], matrix->matrix[1][2],
      matrix->matrix[2][0], matrix->matrix[2][1], matrix->matrix[2][2],
      matrix->offset[0], matrix->offset[1], matrix->offset[2],
      matrix->min[0], matrix->min[1], matrix->min[2],
      matrix->max[0], matrix->max[1], matrix->max[2]);
}

static void
color_matrix_copy (GstCudaColorMatrix * dst, const GstCudaColorMatrix * src)
{
  for (guint i = 0; i < 3; i++) {
    for (guint j = 0; j < 3; j++) {
      dst->matrix[i][j] = src->matrix[i][j];
    }
  }
}

static void
color_matrix_multiply (GstCudaColorMatrix * dst, GstCudaColorMatrix * a,
    GstCudaColorMatrix * b)
{
  GstCudaColorMatrix tmp;

  for (guint i = 0; i < 3; i++) {
    for (guint j = 0; j < 3; j++) {
      gdouble val = 0;
      for (guint k = 0; k < 3; k++) {
        val += a->matrix[i][k] * b->matrix[k][j];
      }

      tmp.matrix[i][j] = val;
    }
  }

  color_matrix_copy (dst, &tmp);
}

static void
color_matrix_identity (GstCudaColorMatrix * m)
{
  for (guint i = 0; i < 3; i++) {
    for (guint j = 0; j < 3; j++) {
      if (i == j)
        m->matrix[i][j] = 1.0;
      else
        m->matrix[i][j] = 0;
    }
  }
}

/**
 * gst_cuda_color_range_adjust_matrix_unorm:
 * @in_info: a #GstVideoInfo
 * @out_info: a #GstVideoInfo
 * @matrix: a #GstCudaColorMatrix
 *
 * Calculates matrix for color range adjustment. Both input and output
 * signals are in normalized [0.0..1.0] space.
 *
 * Resulting values can be calculated by
 * | Yout |                           | Yin |   | matrix.offset[0] |
 * | Uout | = clamp ( matrix.matrix * | Uin | + | matrix.offset[1] |, matrix.min, matrix.max )
 * | Vout |                           | Vin |   | matrix.offset[2] |
 *
 * Returns: %TRUE if successful
 */
static gboolean
gst_cuda_color_range_adjust_matrix_unorm (const GstVideoInfo * in_info,
    const GstVideoInfo * out_info, GstCudaColorMatrix * matrix)
{
  gboolean in_rgb, out_rgb;
  gint in_offset[GST_VIDEO_MAX_COMPONENTS];
  gint in_scale[GST_VIDEO_MAX_COMPONENTS];
  gint out_offset[GST_VIDEO_MAX_COMPONENTS];
  gint out_scale[GST_VIDEO_MAX_COMPONENTS];
  GstVideoColorRange in_range;
  GstVideoColorRange out_range;
  gdouble src_fullscale, dst_fullscale;

  memset (matrix, 0, sizeof (GstCudaColorMatrix));
  for (guint i = 0; i < 3; i++) {
    matrix->matrix[i][i] = 1.0;
    matrix->matrix[i][i] = 1.0;
    matrix->matrix[i][i] = 1.0;
    matrix->max[i] = 1.0;
  }

  in_rgb = GST_VIDEO_INFO_IS_RGB (in_info);
  out_rgb = GST_VIDEO_INFO_IS_RGB (out_info);

  if (in_rgb != out_rgb) {
    GST_WARNING ("Invalid format conversion");
    return FALSE;
  }

  in_range = in_info->colorimetry.range;
  out_range = out_info->colorimetry.range;

  if (in_range == GST_VIDEO_COLOR_RANGE_UNKNOWN) {
    GST_WARNING ("Unknown input color range");
    if (in_rgb || GST_VIDEO_INFO_IS_GRAY (in_info))
      in_range = GST_VIDEO_COLOR_RANGE_0_255;
    else
      in_range = GST_VIDEO_COLOR_RANGE_16_235;
  }

  if (out_range == GST_VIDEO_COLOR_RANGE_UNKNOWN) {
    GST_WARNING ("Unknown output color range");
    if (out_rgb || GST_VIDEO_INFO_IS_GRAY (out_info))
      out_range = GST_VIDEO_COLOR_RANGE_0_255;
    else
      out_range = GST_VIDEO_COLOR_RANGE_16_235;
  }

  src_fullscale = (gdouble) ((1 << in_info->finfo->depth[0]) - 1);
  dst_fullscale = (gdouble) ((1 << out_info->finfo->depth[0]) - 1);

  gst_video_color_range_offsets (in_range, in_info->finfo, in_offset, in_scale);
  gst_video_color_range_offsets (out_range,
      out_info->finfo, out_offset, out_scale);

  matrix->min[0] = matrix->min[1] = matrix->min[2] =
      (gdouble) out_offset[0] / dst_fullscale;

  matrix->max[0] = (out_scale[0] + out_offset[0]) / dst_fullscale;
  matrix->max[1] = matrix->max[2] =
      (out_scale[1] + out_offset[0]) / dst_fullscale;

  if (in_info->colorimetry.range == out_info->colorimetry.range) {
    GST_DEBUG ("Same color range");
    return TRUE;
  }

  /* Formula
   *
   * 1) Scales and offset compensates input to [0..1] range
   * SRC_NORM[i] = (src[i] * src_fullscale - in_offset[i]) / in_scale[i]
   *             = (src[i] * src_fullscale / in_scale[i]) - in_offset[i] / in_scale[i]
   *
   * 2) Reverse to output UNIT scale
   * DST_UINT[i] = SRC_NORM[i] * out_scale[i] + out_offset[i]
   *             = src[i] * src_fullscale * out_scale[i] / in_scale[i]
   *               - in_offset[i] * out_scale[i] / in_scale[i]
   *               + out_offset[i]
   *
   * 3) Back to [0..1] scale
   * dst[i] = DST_UINT[i] / dst_fullscale
   *        = COEFF[i] * src[i] + OFF[i]
   * where
   *             src_fullscale * out_scale[i]
   * COEFF[i] = ------------------------------
   *             dst_fullscale * in_scale[i]
   *
   *            out_offset[i]     in_offset[i] * out_scale[i]
   * OFF[i] =  -------------- -  ------------------------------
   *            dst_fullscale     dst_fullscale * in_scale[i]
   */
  for (guint i = 0; i < 3; i++) {
    matrix->matrix[i][i] = (src_fullscale * out_scale[i]) /
        (dst_fullscale * in_scale[i]);
    matrix->offset[i] = (out_offset[i] / dst_fullscale) -
        ((gdouble) in_offset[i] * out_scale[i] / (dst_fullscale * in_scale[i]));
  }

  return TRUE;
}

/**
 * gst_cuda_yuv_to_rgb_matrix_unorm:
 * @in_yuv_info: a #GstVideoInfo of input YUV signal
 * @out_rgb_info: a #GstVideoInfo of output RGB signal
 * @matrix: a #GstCudaColorMatrix
 *
 * Calculates transform matrix from YUV to RGB conversion. Both input and output
 * signals are in normalized [0.0..1.0] space and additional gamma decoding
 * or primary/transfer function transform is not performed by this matrix.
 *
 * Resulting non-linear RGB values can be calculated by
 * | R' |                           | Y' |   | matrix.offset[0] |
 * | G' | = clamp ( matrix.matrix * | Cb | + | matrix.offset[1] | matrix.min, matrix.max )
 * | B' |                           | Cr |   | matrix.offset[2] |
 *
 * Returns: %TRUE if successful
 */
static gboolean
gst_cuda_yuv_to_rgb_matrix_unorm (const GstVideoInfo * in_yuv_info,
    const GstVideoInfo * out_rgb_info, GstCudaColorMatrix * matrix)
{
  gint offset[4], scale[4];
  gdouble Kr, Kb, Kg;

  /*
   * <Formula>
   *
   * Input: Unsigned normalized Y'CbCr(unorm), [0.0..1.0] range
   * Output: Unsigned normalized non-linear R'G'B'(unorm), [0.0..1.0] range
   *
   * 1) Y'CbCr(unorm) to scaled Y'CbCr
   * | Y' |     | Y'(unorm) |
   * | Cb | = S | Cb(unorm) |
   * | Cb |     | Cr(unorm) |
   * where S = (2 ^ bitdepth) - 1
   *
   * 2) Y'CbCr to YPbPr
   * Y  = (Y' - offsetY )    / scaleY
   * Pb = [(Cb - offsetCbCr) / scaleCbCr]
   * Pr = [(Cr - offsetCrCr) / scaleCrCr]
   * =>
   * Y  = Y'(unorm) * Sy  + Oy
   * Pb = Cb(unorm) * Suv + Ouv
   * Pb = Cr(unorm) * Suv + Ouv
   * where
   * Sy  = S / scaleY
   * Suv = S / scaleCbCr
   * Oy  = -(offsetY / scaleY)
   * Ouv = -(offsetCbCr / scaleCbCr)
   *
   * 3) YPbPr to R'G'B'
   * | R' |      | Y  |
   * | G' | = M *| Pb |
   * | B' |      | Pr |
   * where
   *     | vecR |
   * M = | vecG |
   *     | vecB |
   * vecR = | 1,         0           ,       2(1 - Kr)      |
   * vecG = | 1, -(Kb/Kg) * 2(1 - Kb), -(Kr/Kg) * 2(1 - Kr) |
   * vecB = | 1,       2(1 - Kb)     ,          0           |
   * =>
   * R' = dot(vecR, (Syuv * Y'CbCr(unorm))) + dot(vecR, Offset)
   * G' = dot(vecG, (Svuy * Y'CbCr(unorm))) + dot(vecG, Offset)
   * B' = dot(vecB, (Syuv * Y'CbCr(unorm)) + dot(vecB, Offset)
   * where
   *        | Sy,   0,   0 |
   * Syuv = |  0, Suv,   0 |
   *        |  0    0, Suv |
   *
   *          | Oy  |
   * Offset = | Ouv |
   *          | Ouv |
   *
   * 4) YUV -> RGB matrix
   * | R' |            | Y'(unorm) |   | offsetA |
   * | G' | = Matrix * | Cb(unorm) | + | offsetB |
   * | B' |            | Cr(unorm) |   | offsetC |
   *
   * where
   *          | vecR |
   * Matrix = | vecG | * Syuv
   *          | vecB |
   *
   * offsetA = dot(vecR, Offset)
   * offsetB = dot(vecG, Offset)
   * offsetC = dot(vecB, Offset)
   *
   * 4) Consider 16-235 scale RGB
   * RGBfull(0..255) -> RGBfull(16..235) matrix is represented by
   * | Rs |      | Rf |   | Or |
   * | Gs | = Ms | Gf | + | Og |
   * | Bs |      | Bf |   | Ob |
   *
   * Combining all matrix into
   * | Rs |                   | Y'(unorm) |   | offsetA |     | Or |
   * | Gs | = Ms * ( Matrix * | Cb(unorm) | + | offsetB | ) + | Og |
   * | Bs |                   | Cr(unorm) |   | offsetC |     | Ob |
   *
   *                        | Y'(unorm) |      | offsetA |   | Or |
   *        = Ms * Matrix * | Cb(unorm) | + Ms | offsetB | + | Og |
   *                        | Cr(unorm) |      | offsetC |   | Ob |
   */

  memset (matrix, 0, sizeof (GstCudaColorMatrix));
  for (guint i = 0; i < 3; i++)
    matrix->max[i] = 1.0;

  gst_video_color_range_offsets (in_yuv_info->colorimetry.range,
      in_yuv_info->finfo, offset, scale);

  if (gst_video_color_matrix_get_Kr_Kb (in_yuv_info->colorimetry.matrix,
          &Kr, &Kb)) {
    guint S;
    gdouble Sy, Suv;
    gdouble Oy, Ouv;
    gdouble vecR[3], vecG[3], vecB[3];

    Kg = 1.0 - Kr - Kb;

    vecR[0] = 1.0;
    vecR[1] = 0;
    vecR[2] = 2 * (1 - Kr);

    vecG[0] = 1.0;
    vecG[1] = -(Kb / Kg) * 2 * (1 - Kb);
    vecG[2] = -(Kr / Kg) * 2 * (1 - Kr);

    vecB[0] = 1.0;
    vecB[1] = 2 * (1 - Kb);
    vecB[2] = 0;

    /* Assume all components has the same bitdepth */
    S = (1 << in_yuv_info->finfo->depth[0]) - 1;
    Sy = (gdouble) S / scale[0];
    Suv = (gdouble) S / scale[1];
    Oy = -((gdouble) offset[0] / scale[0]);
    Ouv = -((gdouble) offset[1] / scale[1]);

    matrix->matrix[0][0] = Sy * vecR[0];
    matrix->matrix[1][0] = Sy * vecG[0];
    matrix->matrix[2][0] = Sy * vecB[0];

    matrix->matrix[0][1] = Suv * vecR[1];
    matrix->matrix[1][1] = Suv * vecG[1];
    matrix->matrix[2][1] = Suv * vecB[1];

    matrix->matrix[0][2] = Suv * vecR[2];
    matrix->matrix[1][2] = Suv * vecG[2];
    matrix->matrix[2][2] = Suv * vecB[2];

    matrix->offset[0] = vecR[0] * Oy + vecR[1] * Ouv + vecR[2] * Ouv;
    matrix->offset[1] = vecG[0] * Oy + vecG[1] * Ouv + vecG[2] * Ouv;
    matrix->offset[2] = vecB[0] * Oy + vecB[1] * Ouv + vecB[2] * Ouv;

    /* Apply RGB range scale matrix */
    if (out_rgb_info->colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235) {
      GstCudaColorMatrix scale_matrix, rst;
      GstVideoInfo full_rgb = *out_rgb_info;

      full_rgb.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;

      if (gst_cuda_color_range_adjust_matrix_unorm (&full_rgb,
              out_rgb_info, &scale_matrix)) {
        /* Ms * Matrix */
        color_matrix_multiply (&rst, &scale_matrix, matrix);

        /* Ms * transform offsets */
        for (guint i = 0; i < 3; i++) {
          gdouble val = 0;
          for (guint j = 0; j < 3; j++) {
            val += scale_matrix.matrix[i][j] * matrix->offset[j];
          }
          rst.offset[i] = val + scale_matrix.offset[i];
        }

        /* copy back to output matrix */
        for (guint i = 0; i < 3; i++) {
          for (guint j = 0; j < 3; j++) {
            matrix->matrix[i][j] = rst.matrix[i][j];
          }
          matrix->offset[i] = rst.offset[i];
          matrix->min[i] = scale_matrix.min[i];
          matrix->max[i] = scale_matrix.max[i];
        }
      }
    }
  } else {
    /* Unknown matrix */
    matrix->matrix[0][0] = 1.0;
    matrix->matrix[1][1] = 1.0;
    matrix->matrix[2][2] = 1.0;
  }

  return TRUE;
}

/**
 * gst_cuda_rgb_to_yuv_matrix_unorm:
 * @in_rgb_info: a #GstVideoInfo of input RGB signal
 * @out_yuv_info: a #GstVideoInfo of output YUV signal
 * @matrix: a #GstCudaColorMatrix
 *
 * Calculates transform matrix from RGB to YUV conversion. Both input and output
 * signals are in normalized [0.0..1.0] space and additional gamma decoding
 * or primary/transfer function transform is not performed by this matrix.
 *
 * Resulting RGB values can be calculated by
 * | Y' |                           | R' |   | matrix.offset[0] |
 * | Cb | = clamp ( matrix.matrix * | G' | + | matrix.offset[1] |, matrix.min, matrix.max )
 * | Cr |                           | B' |   | matrix.offset[2] |
 *
 * Returns: %TRUE if successful
 */
static gboolean
gst_cuda_rgb_to_yuv_matrix_unorm (const GstVideoInfo * in_rgb_info,
    const GstVideoInfo * out_yuv_info, GstCudaColorMatrix * matrix)
{
  gint offset[4], scale[4];
  gdouble Kr, Kb, Kg;

  /*
   * <Formula>
   *
   * Input: Unsigned normalized non-linear R'G'B'(unorm), [0.0..1.0] range
   * Output: Unsigned normalized Y'CbCr(unorm), [0.0..1.0] range
   *
   * 1) R'G'B' to YPbPr
   * | Y  |      | R' |
   * | Pb | = M *| G' |
   * | Pr |      | B' |
   * where
   *     | vecY |
   * M = | vecU |
   *     | vecV |
   * vecY = |       Kr      ,       Kg      ,      Kb       |
   * vecU = | -0.5*Kr/(1-Kb), -0.5*Kg/(1-Kb),     0.5       |
   * vecV = |      0.5      , -0.5*Kg/(1-Kr), -0.5*Kb(1-Kr) |
   *
   * 2) YPbPr to Y'CbCr(unorm)
   * Y'(unorm) = (Y  * scaleY + offsetY)       / S
   * Cb(unorm) = (Pb * scaleCbCr + offsetCbCr) / S
   * Cr(unorm) = (Pr * scaleCbCr + offsetCbCr) / S
   * =>
   * Y'(unorm) = (Y  * scaleY    / S) + (offsetY    / S)
   * Cb(unorm) = (Pb * scaleCbCr / S) + (offsetCbCr / S)
   * Cr(unorm) = (Pb * scaleCbCr / S) + (offsetCbCr / S)
   * where S = (2 ^ bitdepth) - 1
   *
   * 3) RGB -> YUV matrix
   * | Y'(unorm) |            | R' |   | offsetA |
   * | Cb(unorm) | = Matrix * | G' | + | offsetB |
   * | Cr(unorm) |            | B' |   | offsetC |
   *
   * where
   *          | (scaleY/S)    * vecY |
   * Matrix = | (scaleCbCr/S) * vecU |
   *          | (scaleCbCr/S) * vecV |
   *
   * offsetA = offsetY    / S
   * offsetB = offsetCbCr / S
   * offsetC = offsetCbCr / S
   *
   * 4) Consider 16-235 scale RGB
   * RGBstudio(16..235) -> RGBfull(0..255) matrix is represented by
   * | Rf |      | Rs |   | Or |
   * | Gf | = Ms | Gs | + | Og |
   * | Bf |      | Bs |   | Ob |
   *
   * Combining all matrix into
   * | Y'(unorm) |                 | Rs |   | Or |     | offsetA |
   * | Cb(unorm) | = Matrix * ( Ms | Gs | + | Og | ) + | offsetB |
   * | Cr(unorm) |                 | Bs |   | Ob |     | offsetC |
   *
   *                             | Rs |          | Or |   | offsetA |
   *               = Matrix * Ms | Gs | + Matrix | Og | + | offsetB |
   *                             | Bs |          | Ob |   | offsetB |
   */

  memset (matrix, 0, sizeof (GstCudaColorMatrix));
  for (guint i = 0; i < 3; i++)
    matrix->max[i] = 1.0;

  gst_video_color_range_offsets (out_yuv_info->colorimetry.range,
      out_yuv_info->finfo, offset, scale);

  if (gst_video_color_matrix_get_Kr_Kb (out_yuv_info->colorimetry.matrix,
          &Kr, &Kb)) {
    guint S;
    gdouble Sy, Suv;
    gdouble Oy, Ouv;
    gdouble vecY[3], vecU[3], vecV[3];

    Kg = 1.0 - Kr - Kb;

    vecY[0] = Kr;
    vecY[1] = Kg;
    vecY[2] = Kb;

    vecU[0] = -0.5 * Kr / (1 - Kb);
    vecU[1] = -0.5 * Kg / (1 - Kb);
    vecU[2] = 0.5;

    vecV[0] = 0.5;
    vecV[1] = -0.5 * Kg / (1 - Kr);
    vecV[2] = -0.5 * Kb / (1 - Kr);

    /* Assume all components has the same bitdepth */
    S = (1 << out_yuv_info->finfo->depth[0]) - 1;
    Sy = (gdouble) scale[0] / S;
    Suv = (gdouble) scale[1] / S;
    Oy = (gdouble) offset[0] / S;
    Ouv = (gdouble) offset[1] / S;

    for (guint i = 0; i < 3; i++) {
      matrix->matrix[0][i] = Sy * vecY[i];
      matrix->matrix[1][i] = Suv * vecU[i];
      matrix->matrix[2][i] = Suv * vecV[i];
    }

    matrix->offset[0] = Oy;
    matrix->offset[1] = Ouv;
    matrix->offset[2] = Ouv;

    matrix->min[0] = Oy;
    matrix->min[1] = Oy;
    matrix->min[2] = Oy;

    matrix->max[0] = ((gdouble) scale[0] + offset[0]) / S;
    matrix->max[1] = ((gdouble) scale[1] + offset[0]) / S;
    matrix->max[2] = ((gdouble) scale[1] + offset[0]) / S;

    /* Apply RGB range scale matrix */
    if (in_rgb_info->colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235) {
      GstCudaColorMatrix scale_matrix, rst;
      GstVideoInfo full_rgb = *in_rgb_info;

      full_rgb.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;

      if (gst_cuda_color_range_adjust_matrix_unorm (in_rgb_info,
              &full_rgb, &scale_matrix)) {
        /* Matrix * Ms */
        color_matrix_multiply (&rst, matrix, &scale_matrix);

        /* Matrix * scale offsets */
        for (guint i = 0; i < 3; i++) {
          gdouble val = 0;
          for (guint j = 0; j < 3; j++) {
            val += matrix->matrix[i][j] * scale_matrix.offset[j];
          }
          rst.offset[i] = val + matrix->offset[i];
        }

        /* copy back to output matrix */
        for (guint i = 0; i < 3; i++) {
          for (guint j = 0; j < 3; j++) {
            matrix->matrix[i][j] = rst.matrix[i][j];
          }
          matrix->offset[i] = rst.offset[i];
        }
      }
    }
  } else {
    /* Unknown matrix */
    matrix->matrix[0][0] = 1.0;
    matrix->matrix[1][1] = 1.0;
    matrix->matrix[2][2] = 1.0;
  }

  return TRUE;
}

struct ColorMatrix
{
  float coeffX[3];
  float coeffY[3];
  float coeffZ[3];
  float offset[3];
  float min[3];
  float max[3];
};

struct ConstBuffer
{
  ColorMatrix toRGBCoeff;
  ColorMatrix toYuvCoeff;
  int width;
  int height;
  int left;
  int top;
  int right;
  int bottom;
  int view_width;
  int view_height;
  float border_x;
  float border_y;
  float border_z;
  float border_w;
  int fill_border;
  int video_direction;
  float alpha;
  int do_blend;
};

#define COLOR_SPACE_IDENTITY "color_space_identity"
#define COLOR_SPACE_CONVERT "color_space_convert"

#define SAMPLE_YUV_PLANAR "sample_yuv_planar"
#define SAMPLE_YV12 "sample_yv12"
#define SAMPLE_YUV_PLANAR_10BIS "sample_yuv_planar_10bits"
#define SAMPLE_YUV_PLANAR_12BIS "sample_yuv_planar_12bits"
#define SAMPLE_SEMI_PLANAR "sample_semi_planar"
#define SAMPLE_SEMI_PLANAR_SWAP "sample_semi_planar_swap"
#define SAMPLE_RGBA "sample_rgba"
#define SAMPLE_BGRA "sample_bgra"
#define SAMPLE_RGBx "sample_rgbx"
#define SAMPLE_BGRx "sample_bgrx"
#define SAMPLE_ARGB "sample_argb"
/* same as ARGB */
#define SAMPLE_ARGB64 "sample_argb"
#define SAMPLE_AGBR "sample_abgr"
#define SAMPLE_RGBP "sample_rgbp"
#define SAMPLE_BGRP "sample_bgrp"
#define SAMPLE_GBR "sample_gbr"
#define SAMPLE_GBR_10 "sample_gbr_10"
#define SAMPLE_GBR_12 "sample_gbr_12"
#define SAMPLE_GBRA "sample_gbra"
#define SAMPLE_VUYA "sample_vuya"

#define WRITE_I420 "write_i420"
#define BLEND_I420 "blend_i420"
#define WRITE_YV12 "write_yv12"
#define BLEND_YV12 "blend_yv12"
#define WRITE_NV12 "write_nv12"
#define BLEND_NV12 "blend_nv12"
#define WRITE_NV21 "write_nv21"
#define BLEND_NV21 "blend_nv21"
#define WRITE_P010 "write_p010"
#define BLEND_P010 "blend_p010"
#define WRITE_I420_10 "write_i420_10"
#define BLEND_I420_10 "blend_i420_10"
#define WRITE_I420_12 "write_i420_12"
#define BLEND_I420_12 "blend_i420_12"
#define WRITE_Y444 "write_y444"
#define BLEND_Y444 "blend_y444"
#define WRITE_Y444_10 "write_y444_10"
#define BLEND_Y444_10 "blend_y444_10"
#define WRITE_Y444_12 "write_y444_12"
#define BLEND_Y444_12 "blend_y444_12"
#define WRITE_Y444_16 "write_y444_16"
#define BLEND_Y444_16 "blend_y444_16"
#define WRITE_RGBA "write_rgba"
#define BLEND_RGBA "blend_rgba"
#define WRITE_RGBx "write_rgbx"
#define BLEND_RGBx "blend_rgbx"
#define WRITE_BGRA "write_bgra"
#define BLEND_BGRA "blend_bgra"
#define WRITE_BGRx "write_bgrx"
#define BLEND_BGRx "blend_bgrx"
#define WRITE_ARGB "write_argb"
#define BLEND_ARGB "blend_argb"
#define WRITE_ABGR "write_abgr"
#define BLEND_ABGR "blend_abgr"
#define WRITE_RGB "write_rgb"
#define BLEND_RGB "blend_rgb"
#define WRITE_BGR "write_bgr"
#define BLEND_BGR "blend_bgr"
#define WRITE_RGB10A2 "write_rgb10a2"
#define BLEND_RGB10A2 "blend_rgb10a2"
#define WRITE_BGR10A2 "write_bgr10a2"
#define BLEND_BGR10A2 "blend_bgr10a2"
#define WRITE_Y42B "write_y42b"
#define BLEND_Y42B "blend_y42b"
#define WRITE_I422_10 "write_i422_10"
#define BLEND_I422_10 "blend_i422_10"
#define WRITE_I422_12 "write_i422_12"
#define BLEND_I422_12 "blend_i422_12"
#define WRITE_RGBP "write_rgbp"
#define BLEND_RGBP "blend_rgbp"
#define WRITE_BGRP "write_bgrp"
#define BLEND_BGRP "blend_bgrp"
#define WRITE_GBR "write_gbr"
#define BLEND_GBR "blend_gbr"
#define WRITE_GBR_10 "write_gbr_10"
#define BLEND_GBR_10 "blend_gbr_10"
#define WRITE_GBR_12 "write_gbr_12"
#define BLEND_GBR_12 "blend_gbr_12"
#define WRITE_GBR_16 "write_gbr_16"
#define BLEND_GBR_16 "blend_gbr_16"
#define WRITE_GBRA "write_gbra"
#define BLEND_GBRA "blend_gbra"
#define WRITE_VUYA "write_vuya"
#define BLEND_VUYA "blend_vuya"

/* *INDENT-OFF* */
const static gchar KERNEL_COMMON[] =
"struct ColorMatrix\n"
"{\n"
"  float CoeffX[3];\n"
"  float CoeffY[3];\n"
"  float CoeffZ[3];\n"
"  float Offset[3];\n"
"  float Min[3];\n"
"  float Max[3];\n"
"};\n"
"\n"
"__device__ inline float\n"
"dot (const float coeff[3], float3 val)\n"
"{\n"
"  return coeff[0] * val.x + coeff[1] * val.y + coeff[2] * val.z;\n"
"}\n"
"\n"
"__device__ inline float\n"
"clamp (float val, float min_val, float max_val)\n"
"{\n"
"  return max (min_val, min (val, max_val));\n"
"}\n"
"\n"
"__device__ inline float3\n"
"clamp3 (float3 val, const float min_val[3], const float max_val[3])\n"
"{\n"
"  return make_float3 (clamp (val.x, min_val[0], max_val[0]),\n"
"      clamp (val.y, min_val[1], max_val[2]),\n"
"      clamp (val.z, min_val[1], max_val[2]));\n"
"}\n"
"\n"
"__device__ inline unsigned char\n"
"scale_to_2bits (float val)\n"
"{\n"
"  return (unsigned short) __float2int_rz (val * 3.0);\n"
"}\n"
"\n"
"__device__ inline unsigned char\n"
"scale_to_uchar (float val)\n"
"{\n"
"  return (unsigned char) __float2int_rz (val * 255.0);\n"
"}\n"
"\n"
"__device__ inline unsigned short\n"
"scale_to_ushort (float val)\n"
"{\n"
"  return (unsigned short) __float2int_rz (val * 65535.0);\n"
"}\n"
"\n"
"__device__ inline unsigned short\n"
"scale_to_10bits (float val)\n"
"{\n"
"  return (unsigned short) __float2int_rz (val * 1023.0);\n"
"}\n"
"\n"
"__device__ inline unsigned short\n"
"scale_to_12bits (float val)\n"
"{\n"
"  return (unsigned short) __float2int_rz (val * 4095.0);\n"
"}\n"
"\n"
"__device__ inline unsigned char\n"
"blend_uchar (unsigned char dst, float src, float src_alpha)\n"
"{\n"
"  // DstColor' = SrcA * SrcColor + (1 - SrcA) DstColor\n"
"  float src_val = src * src_alpha;\n"
"  float dst_val = __int2float_rz (dst) / 255.0 * (1.0 - src_alpha);\n"
"  return scale_to_uchar(clamp(src_val + dst_val, 0, 1.0));\n"
"}\n"
"\n"
"__device__ inline unsigned short\n"
"blend_ushort (unsigned short dst, float src, float src_alpha)\n"
"{\n"
"  // DstColor' = SrcA * SrcColor + (1 - SrcA) DstColor\n"
"  float src_val = src * src_alpha;\n"
"  float dst_val = __int2float_rz (dst) / 65535.0 * (1.0 - src_alpha);\n"
"  return scale_to_ushort(clamp(src_val + dst_val, 0, 1.0));\n"
"}\n"
"\n"
"__device__ inline unsigned short\n"
"blend_10bits (unsigned short dst, float src, float src_alpha)\n"
"{\n"
"  // DstColor' = SrcA * SrcColor + (1 - SrcA) DstColor\n"
"  float src_val = src * src_alpha;\n"
"  float dst_val = __int2float_rz (dst) / 1023.0 * (1.0 - src_alpha);\n"
"  return scale_to_10bits(clamp(src_val + dst_val, 0, 1.0));\n"
"}\n"
"\n"
"__device__ inline unsigned short\n"
"blend_12bits (unsigned short dst, float src, float src_alpha)\n"
"{\n"
"  // DstColor' = SrcA * SrcColor + (1 - SrcA) DstColor\n"
"  float src_val = src * src_alpha;\n"
"  float dst_val = __int2float_rz (dst) / 4095.0 * (1.0 - src_alpha);\n"
"  return scale_to_12bits(clamp(src_val + dst_val, 0, 1.0));\n"
"}\n"
"\n"
"__device__ inline float3\n"
COLOR_SPACE_IDENTITY "(float3 sample, const ColorMatrix * matrix)\n"
"{\n"
"  return sample;\n"
"}\n"
"\n"
"__device__ inline float3\n"
COLOR_SPACE_CONVERT "(float3 sample, const ColorMatrix * matrix)\n"
"{\n"
"  float3 out;\n"
"  out.x = dot (matrix->CoeffX, sample);\n"
"  out.y = dot (matrix->CoeffY, sample);\n"
"  out.z = dot (matrix->CoeffZ, sample);\n"
"  out.x += matrix->Offset[0];\n"
"  out.y += matrix->Offset[1];\n"
"  out.z += matrix->Offset[2];\n"
"  return clamp3 (out, matrix->Min, matrix->Max);\n"
"}\n"
"/* All 8bits yuv planar except for yv12 */\n"
"__device__ inline float4\n"
SAMPLE_YUV_PLANAR "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float luma = tex2D<float>(tex0, x, y);\n"
"  float u = tex2D<float>(tex1, x, y);\n"
"  float v = tex2D<float>(tex2, x, y);\n"
"  return make_float4 (luma, u, v, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_YV12 "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float luma = tex2D<float>(tex0, x, y);\n"
"  float u = tex2D<float>(tex2, x, y);\n"
"  float v = tex2D<float>(tex1, x, y);\n"
"  return make_float4 (luma, u, v, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_YUV_PLANAR_10BIS "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float luma = tex2D<float>(tex0, x, y);\n"
"  float u = tex2D<float>(tex1, x, y);\n"
"  float v = tex2D<float>(tex2, x, y);\n"
"  /* (1 << 6) to scale [0, 1.0) range */\n"
"  return make_float4 (luma * 64, u * 64, v * 64, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_YUV_PLANAR_12BIS "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float luma = tex2D<float>(tex0, x, y);\n"
"  float u = tex2D<float>(tex1, x, y);\n"
"  float v = tex2D<float>(tex2, x, y);\n"
"  /* (1 << 4) to scale [0, 1.0) range */\n"
"  return make_float4 (luma * 16, u * 16, v * 16, 1);\n"
"}\n"
"\n"
"/* NV12, P010, and P016 */\n"
"__device__ inline float4\n"
SAMPLE_SEMI_PLANAR "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float luma = tex2D<float>(tex0, x, y);\n"
"  float2 uv = tex2D<float2>(tex1, x, y);\n"
"  return make_float4 (luma, uv.x, uv.y, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_SEMI_PLANAR_SWAP "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float luma = tex2D<float>(tex0, x, y);\n"
"  float2 vu = tex2D<float2>(tex1, x, y);\n"
"  return make_float4 (luma, vu.y, vu.x, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_RGBA "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  return tex2D<float4>(tex0, x, y);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_BGRA "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float4 bgra = tex2D<float4>(tex0, x, y);\n"
"  return make_float4 (bgra.z, bgra.y, bgra.x, bgra.w);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_RGBx "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float4 rgbx = tex2D<float4>(tex0, x, y);\n"
"  rgbx.w = 1;\n"
"  return rgbx;\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_BGRx "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float4 bgrx = tex2D<float4>(tex0, x, y);\n"
"  return make_float4 (bgrx.z, bgrx.y, bgrx.x, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_ARGB "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float4 argb = tex2D<float4>(tex0, x, y);\n"
"  return make_float4 (argb.y, argb.z, argb.w, argb.x);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_AGBR "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float4 abgr = tex2D<float4>(tex0, x, y);\n"
"  return make_float4 (abgr.w, abgr.z, abgr.y, abgr.x);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_RGBP "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float r = tex2D<float>(tex0, x, y);\n"
"  float g = tex2D<float>(tex1, x, y);\n"
"  float b = tex2D<float>(tex2, x, y);\n"
"  return make_float4 (r, g, b, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_BGRP "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float b = tex2D<float>(tex0, x, y);\n"
"  float g = tex2D<float>(tex1, x, y);\n"
"  float r = tex2D<float>(tex2, x, y);\n"
"  return make_float4 (r, g, b, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_GBR "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float g = tex2D<float>(tex0, x, y);\n"
"  float b = tex2D<float>(tex1, x, y);\n"
"  float r = tex2D<float>(tex2, x, y);\n"
"  return make_float4 (r, g, b, 1);\n"
"}\n"
"__device__ inline float4\n"
SAMPLE_GBR_10 "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float g = tex2D<float>(tex0, x, y);\n"
"  float b = tex2D<float>(tex1, x, y);\n"
"  float r = tex2D<float>(tex2, x, y);\n"
"  /* (1 << 6) to scale [0, 1.0) range */\n"
"  return make_float4 (r * 64, g * 64, b * 64, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_GBR_12 "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float g = tex2D<float>(tex0, x, y);\n"
"  float b = tex2D<float>(tex1, x, y);\n"
"  float r = tex2D<float>(tex2, x, y);\n"
"  /* (1 << 4) to scale [0, 1.0) range */\n"
"  return make_float4 (r * 16, g * 16, b * 16, 1);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_GBRA "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float g = tex2D<float>(tex0, x, y);\n"
"  float b = tex2D<float>(tex1, x, y);\n"
"  float r = tex2D<float>(tex2, x, y);\n"
"  float a = tex2D<float>(tex3, x, y);\n"
"  return make_float4 (r, g, b, a);\n"
"}\n"
"\n"
"__device__ inline float4\n"
SAMPLE_VUYA "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, float x, float y)\n"
"{\n"
"  float4 vuya = tex2D<float4>(tex0, x, y);\n"
"  return make_float4 (vuya.z, vuya.y, vuya.x, vuya.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_I420 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  dst0[x + y * stride0] = scale_to_uchar (sample.x);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    unsigned int pos = x / 2 + (y / 2) * stride1;\n"
"    dst1[pos] = scale_to_uchar (sample.y);\n"
"    dst2[pos] = scale_to_uchar (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_I420 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    pos = x / 2 + (y / 2) * stride1;\n"
"    dst1[pos] = blend_uchar (dst1[pos], sample.y, sample.w);\n"
"    dst2[pos] = blend_uchar (dst2[pos], sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_YV12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  dst0[x + y * stride0] = scale_to_uchar (sample.x);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    unsigned int pos = x / 2 + (y / 2) * stride1;\n"
"    dst1[pos] = scale_to_uchar (sample.z);\n"
"    dst2[pos] = scale_to_uchar (sample.y);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_YV12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    pos = x / 2 + (y / 2) * stride1;\n"
"    dst1[pos] = blend_uchar (dst1[pos], sample.z, sample.w);\n"
"    dst2[pos] = blend_uchar (dst2[pos], sample.y, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_NV12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  dst0[x + y * stride0] = scale_to_uchar (sample.x);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    unsigned int pos = x + (y / 2) * stride1;\n"
"    dst1[pos] = scale_to_uchar (sample.y);\n"
"    dst1[pos + 1] = scale_to_uchar (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_NV12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    pos = x + (y / 2) * stride1;\n"
"    dst1[pos] = blend_uchar (dst1[pos], sample.y, sample.w);\n"
"    dst1[pos + 1] = blend_uchar (dst1[pos + 1], sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_NV21 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  dst0[x + y * stride0] = scale_to_uchar (sample.x);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    unsigned int pos = x + (y / 2) * stride1;\n"
"    dst1[pos] = scale_to_uchar (sample.z);\n"
"    dst1[pos + 1] = scale_to_uchar (sample.y);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_NV21 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    pos = x + (y / 2) * stride1;\n"
"    dst1[pos] = blend_uchar (dst1[pos], sample.z, sample.w);\n"
"    dst1[pos + 1] = blend_uchar (dst1[pos + 1], sample.y, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_P010 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  *(unsigned short *) &dst0[x * 2 + y * stride0] = scale_to_ushort (sample.x);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    unsigned int pos = x * 2 + (y / 2) * stride1;\n"
"    *(unsigned short *) &dst1[pos] = scale_to_ushort (sample.y);\n"
"    *(unsigned short *) &dst1[pos + 2] = scale_to_ushort (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_P010 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_ushort (*target, sample.x, sample.w);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    pos = x * 2 + (y / 2) * stride1;\n"
"    target = (unsigned short *) &dst1[pos];\n"
"    *target = blend_ushort (*target, sample.y, sample.w);\n"
"    target = (unsigned short *) &dst1[pos + 2];\n"
"    *target = blend_ushort (*target, sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_I420_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  *(unsigned short *) &dst0[x * 2 + y * stride0] = scale_to_10bits (sample.x);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    unsigned int pos = x + (y / 2) * stride1;\n"
"    *(unsigned short *) &dst1[pos] = scale_to_10bits (sample.y);\n"
"    *(unsigned short *) &dst2[pos] = scale_to_10bits (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_I420_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_10bits (*target, sample.x, sample.w);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    pos = x * 2 + (y / 2) * stride1;\n"
"    target = (unsigned short *) &dst1[pos];\n"
"    *target = blend_10bits (*target, sample.y, sample.w);\n"
"    target = (unsigned short *) &dst2[pos];\n"
"    *target = blend_10bits (*target, sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_I420_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  *(unsigned short *) &dst0[x * 2 + y * stride0] = scale_to_12bits (sample.x);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    unsigned int pos = x + (y / 2) * stride1;\n"
"    *(unsigned short *) &dst1[pos] = scale_to_12bits (sample.y);\n"
"    *(unsigned short *) &dst2[pos] = scale_to_12bits (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_I420_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_12bits (*target, sample.x, sample.w);\n"
"  if (x % 2 == 0 && y % 2 == 0) {\n"
"    pos = x * 2 + (y / 2) * stride1;\n"
"    target = (unsigned short *) &dst1[pos];\n"
"    *target = blend_12bits (*target, sample.y, sample.w);\n"
"    target = (unsigned short *) &dst2[pos];\n"
"    *target = blend_12bits (*target, sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_Y444 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.x);\n"
"  dst1[pos] = scale_to_uchar (sample.y);\n"
"  dst2[pos] = scale_to_uchar (sample.z);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_Y444 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  dst1[pos] = blend_uchar (dst1[pos], sample.y, sample.w);\n"
"  dst2[pos] = blend_uchar (dst2[pos], sample.z, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_Y444_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  *(unsigned short *) &dst0[pos] = scale_to_10bits (sample.x);\n"
"  *(unsigned short *) &dst1[pos] = scale_to_10bits (sample.y);\n"
"  *(unsigned short *) &dst2[pos] = scale_to_10bits (sample.z);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_Y444_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_10bits (*target, sample.x, sample.w);\n"
"  target = (unsigned short *) &dst1[pos];\n"
"  *target = blend_10bits (*target, sample.y, sample.w);\n"
"  target = (unsigned short *) &dst2[pos];\n"
"  *target = blend_10bits (*target, sample.z, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_Y444_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  *(unsigned short *) &dst0[pos] = scale_to_12bits (sample.x);\n"
"  *(unsigned short *) &dst1[pos] = scale_to_12bits (sample.y);\n"
"  *(unsigned short *) &dst2[pos] = scale_to_12bits (sample.z);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_Y444_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_12bits (*target, sample.x, sample.w);\n"
"  target = (unsigned short *) &dst1[pos];\n"
"  *target = blend_12bits (*target, sample.y, sample.w);\n"
"  target = (unsigned short *) &dst2[pos];\n"
"  *target = blend_12bits (*target, sample.z, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_Y444_16 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  *(unsigned short *) &dst0[pos] = scale_to_ushort (sample.x);\n"
"  *(unsigned short *) &dst1[pos] = scale_to_ushort (sample.y);\n"
"  *(unsigned short *) &dst2[pos] = scale_to_ushort (sample.z);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_Y444_16 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_ushort (*target, sample.x, sample.w);\n"
"  target = (unsigned short *) &dst1[pos];\n"
"  *target = blend_ushort (*target, sample.y, sample.w);\n"
"  target = (unsigned short *) &dst2[pos];\n"
"  *target = blend_ushort (*target, sample.z, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_RGBA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.x);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.z);\n"
"  dst0[pos + 3] = scale_to_uchar (sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_RGBA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.y, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.z, sample.w);\n"
"  dst0[pos + 3] = blend_uchar (dst0[pos + 3], 1.0, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_RGBx "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.x);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.z);\n"
"  dst0[pos + 3] = 255;\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_RGBx "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.y, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.z, sample.w);\n"
"  dst0[pos + 3] = 255;\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_BGRA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.z);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.x);\n"
"  dst0[pos + 3] = scale_to_uchar (sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_BGRA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.z, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.y, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.x, sample.w);\n"
"  dst0[pos + 3] = blend_uchar (dst0[pos + 3], 1.0, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_BGRx "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.z);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.x);\n"
"  dst0[pos + 3] = 255;\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_BGRx "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.z, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.y, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.x, sample.w);\n"
"  dst0[pos + 3] = 255;\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_ARGB "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.w);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.x);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 3] = scale_to_uchar (sample.z);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_ARGB "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], 1.0, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.x, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.y, sample.w);\n"
"  dst0[pos + 3] = blend_uchar (dst0[pos + 3], sample.z, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_ABGR "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.w);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.z);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 3] = scale_to_uchar (sample.x);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_ABGR "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], 1.0, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.z, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.y, sample.w);\n"
"  dst0[pos + 3] = blend_uchar (dst0[pos + 3], sample.x, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_RGB "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 3 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.x);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.z);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_RGB "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 3 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.y, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.z, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_BGR "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 3 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.z);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.x);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_BGR "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 3 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.z, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.y, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.x, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_RGB10A2 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int alpha = (unsigned int) scale_to_2bits (sample.w);\n"
"  unsigned int packed_rgb = alpha << 30;\n"
"  packed_rgb |= ((unsigned int) scale_to_10bits (sample.x));\n"
"  packed_rgb |= ((unsigned int) scale_to_10bits (sample.y)) << 10;\n"
"  packed_rgb |= ((unsigned int) scale_to_10bits (sample.z)) << 20;\n"
"  *(unsigned int *) &dst0[x * 4 + y * stride0] = packed_rgb;\n"
"}\n"
"\n"
"__device__ inline ushort3\n"
"unpack_rgb10a2 (unsigned int val)\n"
"{\n"
"  unsigned short r, g, b;\n"
"  r = (val & 0x3ff);\n"
"  r = (r << 6) | (r >> 4);\n"
"  g = ((val >> 10) & 0x3ff);\n"
"  g = (g << 6) | (g >> 4);\n"
"  b = ((val >> 20) & 0x3ff);\n"
"  b = (b << 6) | (b >> 4);\n"
"  return make_ushort3 (r, g, b);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_RGB10A2 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int * target = (unsigned int *) &dst0[x * 4 + y * stride0];\n"
"  ushort3 val = unpack_rgb10a2 (*target);\n"
"  unsigned int alpha = (unsigned int) scale_to_2bits (sample.w);\n"
"  unsigned int packed_rgb = alpha << 30;\n"
"  packed_rgb |= ((unsigned int) blend_10bits (val.x, sample.x, sample.w));\n"
"  packed_rgb |= ((unsigned int) blend_10bits (val.y, sample.y, sample.w)) << 10;\n"
"  packed_rgb |= ((unsigned int) blend_10bits (val.z, sample.z, sample.w)) << 20;\n"
"  *target = packed_rgb;\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_BGR10A2 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int alpha = (unsigned int) scale_to_2bits (sample.x);\n"
"  unsigned int packed_rgb = alpha << 30;\n"
"  packed_rgb |= ((unsigned int) scale_to_10bits (sample.x)) << 20;\n"
"  packed_rgb |= ((unsigned int) scale_to_10bits (sample.y)) << 10;\n"
"  packed_rgb |= ((unsigned int) scale_to_10bits (sample.z));\n"
"  *(unsigned int *) &dst0[x * 4 + y * stride0] = packed_rgb;\n"
"}\n"
"\n"
"__device__ inline ushort3\n"
"unpack_bgr10a2 (unsigned int val)\n"
"{\n"
"  unsigned short r, g, b;\n"
"  b = (val & 0x3ff);\n"
"  b = (b << 6) | (b >> 4);\n"
"  g = ((val >> 10) & 0x3ff);\n"
"  g = (g << 6) | (g >> 4);\n"
"  r = ((val >> 20) & 0x3ff);\n"
"  r = (r << 6) | (r >> 4);\n"
"  return make_ushort3 (r, g, b);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_BGR10A2 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int * target = (unsigned int *) &dst0[x * 4 + y * stride0];\n"
"  ushort3 val = unpack_bgr10a2 (*target);\n"
"  unsigned int alpha = (unsigned int) scale_to_2bits (sample.w);\n"
"  unsigned int packed_rgb = alpha << 30;\n"
"  packed_rgb |= ((unsigned int) blend_10bits (val.x, sample.x, sample.w)) << 20;\n"
"  packed_rgb |= ((unsigned int) blend_10bits (val.y, sample.y, sample.w)) << 10;\n"
"  packed_rgb |= ((unsigned int) blend_10bits (val.z, sample.z, sample.w));\n"
"  *target = packed_rgb;\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_Y42B "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  dst0[x + y * stride0] = scale_to_uchar (sample.x);\n"
"  if (x % 2 == 0) {\n"
"    unsigned int pos = x / 2 + y * stride1;\n"
"    dst1[pos] = scale_to_uchar (sample.y);\n"
"    dst2[pos] = scale_to_uchar (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_Y42B "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  if (x % 2 == 0) {\n"
"    pos = x / 2 + y * stride1;\n"
"    dst1[pos] = blend_uchar (dst1[pos], sample.y, sample.w);\n"
"    dst2[pos] = blend_uchar (dst2[pos], sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_I422_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  *(unsigned short *) &dst0[x * 2 + y * stride0] = scale_to_10bits (sample.x);\n"
"  if (x % 2 == 0) {\n"
"    unsigned int pos = x + y * stride1;\n"
"    *(unsigned short *) &dst1[pos] = scale_to_10bits (sample.y);\n"
"    *(unsigned short *) &dst2[pos] = scale_to_10bits (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_I422_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_10bits (*target, sample.x, sample.w);\n"
"  if (x % 2 == 0) {\n"
"    pos = x / 2 + y * stride1;\n"
"    target = (unsigned short *) &dst1[pos];\n"
"    *target = blend_10bits (*target, sample.y, sample.w);\n"
"    target = (unsigned short *) &dst2[pos];\n"
"    *target = blend_10bits (*target, sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_I422_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  *(unsigned short *) &dst0[x * 2 + y * stride0] = scale_to_12bits (sample.x);\n"
"  if (x % 2 == 0) {\n"
"    unsigned int pos = x + y * stride1;\n"
"    *(unsigned short *) &dst1[pos] = scale_to_12bits (sample.y);\n"
"    *(unsigned short *) &dst2[pos] = scale_to_12bits (sample.z);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_I422_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  unsigned int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_12bits (*target, sample.x, sample.w);\n"
"  if (x % 2 == 0) {\n"
"    pos = x / 2 + y * stride1;\n"
"    target = (unsigned short *) &dst1[pos];\n"
"    *target = blend_12bits (*target, sample.y, sample.w);\n"
"    target = (unsigned short *) &dst2[pos];\n"
"    *target = blend_12bits (*target, sample.z, sample.w);\n"
"  }\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_RGBP "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.x);\n"
"  dst1[pos] = scale_to_uchar (sample.y);\n"
"  dst2[pos] = scale_to_uchar (sample.z);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_RGBP "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.x, sample.w);\n"
"  dst1[pos] = blend_uchar (dst1[pos], sample.y, sample.w);\n"
"  dst2[pos] = blend_uchar (dst2[pos], sample.z, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_BGRP "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.z);\n"
"  dst1[pos] = scale_to_uchar (sample.y);\n"
"  dst2[pos] = scale_to_uchar (sample.x);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_BGRP "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.z, sample.w);\n"
"  dst1[pos] = blend_uchar (dst1[pos], sample.y, sample.w);\n"
"  dst2[pos] = blend_uchar (dst2[pos], sample.x, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_GBR "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.y);\n"
"  dst1[pos] = scale_to_uchar (sample.z);\n"
"  dst2[pos] = scale_to_uchar (sample.x);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_GBR "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.y, sample.w);\n"
"  dst1[pos] = blend_uchar (dst1[pos], sample.z, sample.w);\n"
"  dst2[pos] = blend_uchar (dst2[pos], sample.x, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_GBR_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  *(unsigned short *) &dst0[pos] = scale_to_10bits (sample.y);\n"
"  *(unsigned short *) &dst1[pos] = scale_to_10bits (sample.z);\n"
"  *(unsigned short *) &dst2[pos] = scale_to_10bits (sample.x);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_GBR_10 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_10bits (*target, sample.y, sample.w);\n"
"  target = (unsigned short *) &dst1[pos];\n"
"  *target = blend_10bits (*target, sample.z, sample.w);\n"
"  target = (unsigned short *) &dst2[pos];\n"
"  *target = blend_10bits (*target, sample.x, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_GBR_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  *(unsigned short *) &dst0[pos] = scale_to_12bits (sample.y);\n"
"  *(unsigned short *) &dst1[pos] = scale_to_12bits (sample.z);\n"
"  *(unsigned short *) &dst2[pos] = scale_to_12bits (sample.x);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_GBR_12 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_12bits (*target, sample.y, sample.w);\n"
"  target = (unsigned short *) &dst1[pos];\n"
"  *target = blend_12bits (*target, sample.z, sample.w);\n"
"  target = (unsigned short *) &dst2[pos];\n"
"  *target = blend_12bits (*target, sample.x, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_GBR_16 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  *(unsigned short *) &dst0[pos] = scale_to_ushort (sample.y);\n"
"  *(unsigned short *) &dst1[pos] = scale_to_ushort (sample.z);\n"
"  *(unsigned short *) &dst2[pos] = scale_to_ushort (sample.x);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_GBR_16 "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 2 + y * stride0;\n"
"  unsigned short * target = (unsigned short *) &dst0[pos];\n"
"  *target = blend_ushort (*target, sample.y, sample.w);\n"
"  target = (unsigned short *) &dst1[pos];\n"
"  *target = blend_ushort (*target, sample.z, sample.w);\n"
"  target = (unsigned short *) &dst2[pos];\n"
"  *target = blend_ushort (*target, sample.x, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_GBRA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.y);\n"
"  dst1[pos] = scale_to_uchar (sample.z);\n"
"  dst2[pos] = scale_to_uchar (sample.x);\n"
"  dst3[pos] = scale_to_uchar (sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_GBRA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.y, sample.w);\n"
"  dst1[pos] = blend_uchar (dst1[pos], sample.z, sample.w);\n"
"  dst2[pos] = blend_uchar (dst2[pos], sample.x, sample.w);\n"
"  dst3[pos] = blend_uchar (dst3[pos], 1.0, sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
WRITE_VUYA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = scale_to_uchar (sample.z);\n"
"  dst0[pos + 1] = scale_to_uchar (sample.y);\n"
"  dst0[pos + 2] = scale_to_uchar (sample.x);\n"
"  dst0[pos + 3] = scale_to_uchar (sample.w);\n"
"}\n"
"\n"
"__device__ inline void\n"
BLEND_VUYA "(unsigned char * dst0, unsigned char * dst1, unsigned char * dst2,\n"
"    unsigned char * dst3, float4 sample, int x, int y, int stride0, int stride1)\n"
"{\n"
"  int pos = x * 4 + y * stride0;\n"
"  dst0[pos] = blend_uchar (dst0[pos], sample.z, sample.w);\n"
"  dst0[pos + 1] = blend_uchar (dst0[pos + 1], sample.y, sample.w);\n"
"  dst0[pos + 2] = blend_uchar (dst0[pos + 2], sample.x, sample.w);\n"
"  dst0[pos + 3] = blend_uchar (dst0[pos + 3], 1.0, sample.w);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_identity (float x, float y)\n"
"{\n"
"  return make_float2(x, y);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_90r (float x, float y)\n"
"{\n"
"  return make_float2(y, 1.0 - x);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_180 (float x, float y)\n"
"{\n"
"  return make_float2(1.0 - x, 1.0 - y);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_90l (float x, float y)\n"
"{\n"
"  return make_float2(1.0 - y, x);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_horiz (float x, float y)\n"
"{\n"
"  return make_float2(1.0 - x, y);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_vert (float x, float y)\n"
"{\n"
"  return make_float2(x, 1.0 - y);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_ul_lr (float x, float y)\n"
"{\n"
"  return make_float2(y, x);\n"
"}\n"
"\n"
"__device__ inline float2\n"
"rotate_ur_ll (float x, float y)\n"
"{\n"
"  return make_float2(1.0 - y, 1.0 - x);\n"
"}\n"
"__device__ inline float2\n"
"do_rotate (float x, float y, int direction)"
"{\n"
"  switch (direction) {\n"
"    case 1:\n"
"      return rotate_90r (x, y);\n"
"    case 2:\n"
"      return rotate_180 (x, y);\n"
"    case 3:\n"
"      return rotate_90l (x, y);\n"
"    case 4:\n"
"      return rotate_horiz (x, y);\n"
"    case 5:\n"
"      return rotate_vert (x, y);\n"
"    case 6:\n"
"      return rotate_ul_lr (x, y);\n"
"    case 7:\n"
"      return rotate_ur_ll (x, y);\n"
"    default:\n"
"      return rotate_identity (x, y);\n"
"  }\n"
"}\n"
"\n";

#define GST_CUDA_KERNEL_UNPACK_FUNC "gst_cuda_kernel_unpack_func"
static const gchar RGB_TO_RGBx[] =
"extern \"C\" {\n"
"__global__ void\n"
GST_CUDA_KERNEL_UNPACK_FUNC
"(unsigned char *src, unsigned char *dst, int width, int height,\n"
"    int src_stride, int dst_stride)\n"
"{\n"
"  int x_pos = blockIdx.x * blockDim.x + threadIdx.x;\n"
"  int y_pos = blockIdx.y * blockDim.y + threadIdx.y;\n"
"  if (x_pos < width && y_pos < height) {\n"
"    int dst_pos = x_pos * 4 + y_pos * dst_stride;\n"
"    int src_pos = x_pos * 3 + y_pos * src_stride;\n"
"    dst[dst_pos] = src[src_pos];\n"
"    dst[dst_pos + 1] = src[src_pos + 1];\n"
"    dst[dst_pos + 2] = src[src_pos + 2];\n"
"    dst[dst_pos + 3] = 0xff;\n"
"  }\n"
"}\n"
"}\n";

static const gchar RGB10A2_TO_ARGB64[] =
"extern \"C\" {\n"
"__global__ void\n"
GST_CUDA_KERNEL_UNPACK_FUNC
"(unsigned char *src, unsigned char *dst, int width, int height,\n"
"    int src_stride, int dst_stride)\n"
"{\n"
"  int x_pos = blockIdx.x * blockDim.x + threadIdx.x;\n"
"  int y_pos = blockIdx.y * blockDim.y + threadIdx.y;\n"
"  if (x_pos < width && y_pos < height) {\n"
"    unsigned short a, r, g, b;\n"
"    unsigned int val;\n"
"    int dst_pos = x_pos * 8 + y_pos * dst_stride;\n"
"    val = *(unsigned int *)&src[x_pos * 4 + y_pos * src_stride];\n"
"    a = (val >> 30) & 0x03;\n"
"    a = (a << 14) | (a << 12) | (a << 10) | (a << 8) | (a << 6) | (a << 4) | (a << 2) | (a << 0);\n"
"    r = (val & 0x3ff);\n"
"    r = (r << 6) | (r >> 4);\n"
"    g = ((val >> 10) & 0x3ff);\n"
"    g = (g << 6) | (g >> 4);\n"
"    b = ((val >> 20) & 0x3ff);\n"
"    b = (b << 6) | (b >> 4);\n"
"    *(unsigned short *) &dst[dst_pos] = a;\n"
"    *(unsigned short *) &dst[dst_pos + 2] = r;\n"
"    *(unsigned short *) &dst[dst_pos + 4] = g;\n"
"    *(unsigned short *) &dst[dst_pos + 6] = b;\n"
"  }\n"
"}\n"
"}\n";

static const gchar BGR10A2_TO_ARGB64[] =
"extern \"C\" {\n"
"__global__ void\n"
GST_CUDA_KERNEL_UNPACK_FUNC
"(unsigned char *src, unsigned char *dst, int width, int height,\n"
"    int src_stride, int dst_stride)\n"
"{\n"
"  int x_pos = blockIdx.x * blockDim.x + threadIdx.x;\n"
"  int y_pos = blockIdx.y * blockDim.y + threadIdx.y;\n"
"  if (x_pos < width && y_pos < height) {\n"
"    unsigned short a, r, g, b;\n"
"    unsigned int val;\n"
"    int dst_pos = x_pos * 8 + y_pos * dst_stride;\n"
"    val = *(unsigned int *)&src[x_pos * 4 + y_pos * src_stride];\n"
"    a = (val >> 30) & 0x03;\n"
"    a = (a << 14) | (a << 12) | (a << 10) | (a << 8) | (a << 6) | (a << 4) | (a << 2) | (a << 0);\n"
"    b = (val & 0x3ff);\n"
"    b = (b << 6) | (b >> 4);\n"
"    g = ((val >> 10) & 0x3ff);\n"
"    g = (g << 6) | (g >> 4);\n"
"    r = ((val >> 20) & 0x3ff);\n"
"    r = (r << 6) | (r >> 4);\n"
"    *(unsigned short *) &dst[dst_pos] = a;\n"
"    *(unsigned short *) &dst[dst_pos + 2] = r;\n"
"    *(unsigned short *) &dst[dst_pos + 4] = g;\n"
"    *(unsigned short *) &dst[dst_pos + 6] = b;\n"
"  }\n"
"}\n"
"}\n";

#define GST_CUDA_KERNEL_MAIN_FUNC "gst_cuda_converter_main"

static const gchar TEMPLATE_KERNEL[] =
/* KERNEL_COMMON */
"%s\n"
/* UNPACK FUNCTION */
"%s\n"
"struct ConstBuffer\n"
"{\n"
"  ColorMatrix toRGBCoeff;\n"
"  ColorMatrix toYuvCoeff;\n"
"  int width;\n"
"  int height;\n"
"  int left;\n"
"  int top;\n"
"  int right;\n"
"  int bottom;\n"
"  int view_width;\n"
"  int view_height;\n"
"  float border_x;\n"
"  float border_y;\n"
"  float border_z;\n"
"  float border_w;\n"
"  int fill_border;\n"
"  int video_direction;\n"
"  float alpha;\n"
"  int do_blend;\n"
"};\n"
"\n"
"extern \"C\" {\n"
"__global__ void\n"
GST_CUDA_KERNEL_MAIN_FUNC "(cudaTextureObject_t tex0, cudaTextureObject_t tex1,\n"
"    cudaTextureObject_t tex2, cudaTextureObject_t tex3, unsigned char * dst0,\n"
"    unsigned char * dst1, unsigned char * dst2, unsigned char * dst3,\n"
"    int stride0, int stride1, ConstBuffer * const_buf, int off_x, int off_y)\n"
"{\n"
"  int x_pos = blockIdx.x * blockDim.x + threadIdx.x + off_x;\n"
"  int y_pos = blockIdx.y * blockDim.y + threadIdx.y + off_y;\n"
"  float4 sample;\n"
"  if (x_pos >= const_buf->width || y_pos >= const_buf->height ||\n"
"      const_buf->view_width <= 0 || const_buf->view_height <= 0)\n"
"    return;\n"
"  if (x_pos < const_buf->left || x_pos >= const_buf->right ||\n"
"      y_pos < const_buf->top || y_pos >= const_buf->bottom) {\n"
"    if (!const_buf->fill_border)\n"
"      return;\n"
"    sample = make_float4 (const_buf->border_x, const_buf->border_y,\n"
"       const_buf->border_z, const_buf->border_w);\n"
"  } else {\n"
"    float x = (__int2float_rz (x_pos - const_buf->left) + 0.5) / const_buf->view_width;\n"
"    if (x < 0.0 || x > 1.0)\n"
"      return;\n"
"    float y = (__int2float_rz (y_pos - const_buf->top) + 0.5) / const_buf->view_height;\n"
"    if (y < 0.0 || y > 1.0)\n"
"      return;\n"
"    float2 rotated = do_rotate (x, y, const_buf->video_direction);\n"
"    float4 s = %s (tex0, tex1, tex2, tex3, rotated.x, rotated.y);\n"
"    float3 xyz = make_float3 (s.x, s.y, s.z);\n"
"    float3 rgb = %s (xyz, &const_buf->toRGBCoeff);\n"
"    float3 yuv = %s (rgb, &const_buf->toYuvCoeff);\n"
"    sample = make_float4 (yuv.x, yuv.y, yuv.z, s.w);\n"
"  }\n"
"  sample.w = sample.w * const_buf->alpha;\n"
"  if (!const_buf->do_blend) {\n"
"    %s (dst0, dst1, dst2, dst3, sample, x_pos, y_pos, stride0, stride1);\n"
"  } else {\n"
"    %s (dst0, dst1, dst2, dst3, sample, x_pos, y_pos, stride0, stride1);\n"
"   }"
"}\n"
"}\n";
/* *INDENT-ON* */

typedef struct _TextureFormat
{
  GstVideoFormat format;
  CUarray_format array_format[GST_VIDEO_MAX_COMPONENTS];
  guint channels[GST_VIDEO_MAX_COMPONENTS];
  const gchar *sample_func;
} TextureFormat;

#define CU_AD_FORMAT_NONE ((CUarray_format)0)
#define MAKE_FORMAT_YUV_PLANAR(f,cf,sample_func) \
  { GST_VIDEO_FORMAT_ ##f,  { CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_ ##cf, \
      CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_NONE },  {1, 1, 1, 0}, sample_func }
#define MAKE_FORMAT_YUV_SEMI_PLANAR(f,cf,sample_func) \
  { GST_VIDEO_FORMAT_ ##f,  { CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_ ##cf, \
      CU_AD_FORMAT_NONE, CU_AD_FORMAT_NONE }, {1, 2, 0, 0}, sample_func }
#define MAKE_FORMAT_RGB(f,cf,sample_func) \
  { GST_VIDEO_FORMAT_ ##f,  { CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_NONE, \
      CU_AD_FORMAT_NONE, CU_AD_FORMAT_NONE }, {4, 0, 0, 0}, sample_func }
#define MAKE_FORMAT_RGBP(f,cf,sample_func) \
  { GST_VIDEO_FORMAT_ ##f,  { CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_ ##cf, \
      CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_NONE }, {1, 1, 1, 0}, sample_func }
#define MAKE_FORMAT_RGBAP(f,cf,sample_func) \
  { GST_VIDEO_FORMAT_ ##f,  { CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_ ##cf, \
      CU_AD_FORMAT_ ##cf, CU_AD_FORMAT_ ##cf }, {1, 1, 1, 1}, sample_func }

static const TextureFormat format_map[] = {
  MAKE_FORMAT_YUV_PLANAR (I420, UNSIGNED_INT8, SAMPLE_YUV_PLANAR),
  MAKE_FORMAT_YUV_PLANAR (YV12, UNSIGNED_INT8, SAMPLE_YV12),
  MAKE_FORMAT_YUV_SEMI_PLANAR (NV12, UNSIGNED_INT8, SAMPLE_SEMI_PLANAR),
  MAKE_FORMAT_YUV_SEMI_PLANAR (NV21, UNSIGNED_INT8, SAMPLE_SEMI_PLANAR_SWAP),
  MAKE_FORMAT_YUV_SEMI_PLANAR (P010_10LE, UNSIGNED_INT16, SAMPLE_SEMI_PLANAR),
  MAKE_FORMAT_YUV_SEMI_PLANAR (P012_LE, UNSIGNED_INT16, SAMPLE_SEMI_PLANAR),
  MAKE_FORMAT_YUV_SEMI_PLANAR (P016_LE, UNSIGNED_INT16, SAMPLE_SEMI_PLANAR),
  MAKE_FORMAT_YUV_PLANAR (I420_10LE, UNSIGNED_INT16, SAMPLE_YUV_PLANAR_10BIS),
  MAKE_FORMAT_YUV_PLANAR (I420_12LE, UNSIGNED_INT16, SAMPLE_YUV_PLANAR_12BIS),
  MAKE_FORMAT_YUV_PLANAR (Y444, UNSIGNED_INT8, SAMPLE_YUV_PLANAR),
  MAKE_FORMAT_YUV_PLANAR (Y444_10LE, UNSIGNED_INT16, SAMPLE_YUV_PLANAR_10BIS),
  MAKE_FORMAT_YUV_PLANAR (Y444_12LE, UNSIGNED_INT16, SAMPLE_YUV_PLANAR_12BIS),
  MAKE_FORMAT_YUV_PLANAR (Y444_16LE, UNSIGNED_INT16, SAMPLE_YUV_PLANAR),
  MAKE_FORMAT_RGB (RGBA, UNSIGNED_INT8, SAMPLE_RGBA),
  MAKE_FORMAT_RGB (BGRA, UNSIGNED_INT8, SAMPLE_BGRA),
  MAKE_FORMAT_RGB (RGBx, UNSIGNED_INT8, SAMPLE_RGBx),
  MAKE_FORMAT_RGB (BGRx, UNSIGNED_INT8, SAMPLE_BGRx),
  MAKE_FORMAT_RGB (ARGB, UNSIGNED_INT8, SAMPLE_ARGB),
  MAKE_FORMAT_RGB (ARGB64, UNSIGNED_INT16, SAMPLE_ARGB64),
  MAKE_FORMAT_RGB (ABGR, UNSIGNED_INT8, SAMPLE_AGBR),
  MAKE_FORMAT_YUV_PLANAR (Y42B, UNSIGNED_INT8, SAMPLE_YUV_PLANAR),
  MAKE_FORMAT_YUV_PLANAR (I422_10LE, UNSIGNED_INT16, SAMPLE_YUV_PLANAR_10BIS),
  MAKE_FORMAT_YUV_PLANAR (I422_12LE, UNSIGNED_INT16, SAMPLE_YUV_PLANAR_12BIS),
  MAKE_FORMAT_RGBP (RGBP, UNSIGNED_INT8, SAMPLE_RGBP),
  MAKE_FORMAT_RGBP (BGRP, UNSIGNED_INT8, SAMPLE_BGRP),
  MAKE_FORMAT_RGBP (GBR, UNSIGNED_INT8, SAMPLE_GBR),
  MAKE_FORMAT_RGBP (GBR_10LE, UNSIGNED_INT16, SAMPLE_GBR_10),
  MAKE_FORMAT_RGBP (GBR_12LE, UNSIGNED_INT16, SAMPLE_GBR_12),
  MAKE_FORMAT_RGBP (GBR_16LE, UNSIGNED_INT16, SAMPLE_GBR),
  MAKE_FORMAT_RGBAP (GBRA, UNSIGNED_INT8, SAMPLE_GBRA),
  MAKE_FORMAT_RGB (VUYA, UNSIGNED_INT8, SAMPLE_VUYA),
};

struct TextureBuffer
{
  CUdeviceptr ptr = 0;
  gsize stride = 0;
  CUtexObject texture = 0;
};

enum
{
  PROP_0,
  PROP_DEST_X,
  PROP_DEST_Y,
  PROP_DEST_WIDTH,
  PROP_DEST_HEIGHT,
  PROP_FILL_BORDER,
  PROP_VIDEO_DIRECTION,
  PROP_ALPHA,
  PROP_BLEND,
};

struct _GstCudaConverterPrivate
{
  _GstCudaConverterPrivate ()
  {
    config = gst_structure_new_empty ("converter-config");
  }

   ~_GstCudaConverterPrivate ()
  {
    if (config)
      gst_structure_free (config);
  }

  std::mutex lock;

  GstVideoInfo in_info;
  GstVideoInfo out_info;

  GstStructure *config = nullptr;

  GstVideoInfo texture_info;
  const TextureFormat *texture_fmt;
  gint texture_align;

  TextureBuffer fallback_buffer[GST_VIDEO_MAX_COMPONENTS];
  TextureBuffer unpack_buffer;
  ConstBuffer *const_buf_staging = nullptr;
  CUdeviceptr const_buf = 0;

  CUmodule module = nullptr;
  CUfunction main_func = nullptr;
  CUfunction unpack_func = nullptr;

  gboolean update_const_buf = TRUE;

  /* properties */
  gint dest_x = 0;
  gint dest_y = 0;
  gint dest_width = 0;
  gint dest_height = 0;
  GstVideoOrientationMethod video_direction = GST_VIDEO_ORIENTATION_IDENTITY;
  gboolean fill_border = FALSE;
  CUfilter_mode filter_mode = CU_TR_FILTER_MODE_LINEAR;
  gdouble alpha = 1.0;
  gboolean blend = FALSE;
};

static void gst_cuda_converter_dispose (GObject * object);
static void gst_cuda_converter_finalize (GObject * object);
static void gst_cuda_converter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_cuda_converter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

#define gst_cuda_converter_parent_class parent_class
G_DEFINE_TYPE (GstCudaConverter, gst_cuda_converter, GST_TYPE_OBJECT);

static void
gst_cuda_converter_class_init (GstCudaConverterClass * klass)
{
  auto object_class = G_OBJECT_CLASS (klass);
  auto param_flags = (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  object_class->dispose = gst_cuda_converter_dispose;
  object_class->finalize = gst_cuda_converter_finalize;
  object_class->set_property = gst_cuda_converter_set_property;
  object_class->get_property = gst_cuda_converter_get_property;

  g_object_class_install_property (object_class, PROP_DEST_X,
      g_param_spec_int ("dest-x", "Dest-X",
          "x poisition in the destination frame", G_MININT, G_MAXINT, 0,
          param_flags));
  g_object_class_install_property (object_class, PROP_DEST_Y,
      g_param_spec_int ("dest-y", "Dest-Y",
          "y poisition in the destination frame", G_MININT, G_MAXINT, 0,
          param_flags));
  g_object_class_install_property (object_class, PROP_DEST_WIDTH,
      g_param_spec_int ("dest-width", "Dest-Width",
          "Width in the destination frame", 0, G_MAXINT, 0, param_flags));
  g_object_class_install_property (object_class, PROP_DEST_HEIGHT,
      g_param_spec_int ("dest-height", "Dest-Height",
          "Height in the destination frame", 0, G_MAXINT, 0, param_flags));
  g_object_class_install_property (object_class, PROP_FILL_BORDER,
      g_param_spec_boolean ("fill-border", "Fill border",
          "Fill border", FALSE, param_flags));
  g_object_class_install_property (object_class, PROP_VIDEO_DIRECTION,
      g_param_spec_enum ("video-direction", "Video Direction",
          "Video direction", GST_TYPE_VIDEO_ORIENTATION_METHOD,
          GST_VIDEO_ORIENTATION_IDENTITY, param_flags));
  g_object_class_install_property (object_class, PROP_ALPHA,
      g_param_spec_double ("alpha", "Alpha",
          "The alpha color value to use", 0, 1.0, 1.0, param_flags));
  g_object_class_install_property (object_class, PROP_BLEND,
      g_param_spec_boolean ("blend", "Blend",
          "Enable alpha blending", FALSE, param_flags));

  GST_DEBUG_CATEGORY_INIT (gst_cuda_converter_debug,
      "cudaconverter", 0, "cudaconverter");
}

static void
gst_cuda_converter_init (GstCudaConverter * self)
{
  self->priv = new GstCudaConverterPrivate ();
}

static void
gst_cuda_converter_dispose (GObject * object)
{
  auto self = GST_CUDA_CONVERTER (object);
  auto priv = self->priv;

  if (self->context && gst_cuda_context_push (self->context)) {
    if (priv->module) {
      CuModuleUnload (priv->module);
      priv->module = nullptr;
    }

    for (guint i = 0; i < G_N_ELEMENTS (priv->fallback_buffer); i++) {
      if (priv->fallback_buffer[i].ptr) {
        if (priv->fallback_buffer[i].texture) {
          CuTexObjectDestroy (priv->fallback_buffer[i].texture);
          priv->fallback_buffer[i].texture = 0;
        }

        CuMemFree (priv->fallback_buffer[i].ptr);
        priv->fallback_buffer[i].ptr = 0;
      }
    }

    if (priv->unpack_buffer.ptr) {
      if (priv->unpack_buffer.texture) {
        CuTexObjectDestroy (priv->unpack_buffer.texture);
        priv->unpack_buffer.texture = 0;
      }

      CuMemFree (priv->unpack_buffer.ptr);
      priv->unpack_buffer.ptr = 0;
    }

    if (priv->const_buf_staging) {
      CuMemFreeHost (priv->const_buf_staging);
      priv->const_buf_staging = nullptr;
    }

    if (priv->const_buf) {
      CuMemFree (priv->const_buf);
      priv->const_buf = 0;
    }

    gst_cuda_context_pop (nullptr);
  }

  gst_clear_object (&self->context);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_cuda_converter_finalize (GObject * object)
{
  auto self = GST_CUDA_CONVERTER (object);

  delete self->priv;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_cuda_converter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  auto self = GST_CUDA_CONVERTER (object);
  auto priv = self->priv;

  std::lock_guard < std::mutex > lk (priv->lock);
  switch (prop_id) {
    case PROP_DEST_X:
    {
      auto dest_x = g_value_get_int (value);
      if (priv->dest_x != dest_x) {
        priv->update_const_buf = TRUE;
        priv->dest_x = dest_x;
        priv->const_buf_staging->left = dest_x;
        priv->const_buf_staging->right = priv->dest_x + priv->dest_width;
      }
      break;
    }
    case PROP_DEST_Y:
    {
      auto dest_y = g_value_get_int (value);
      if (priv->dest_y != dest_y) {
        priv->update_const_buf = TRUE;
        priv->dest_y = dest_y;
        priv->const_buf_staging->top = dest_y;
        priv->const_buf_staging->bottom = priv->dest_y + priv->dest_height;
      }
      break;
    }
    case PROP_DEST_WIDTH:
    {
      auto dest_width = g_value_get_int (value);
      if (priv->dest_width != dest_width) {
        priv->update_const_buf = TRUE;
        priv->dest_width = dest_width;
        priv->const_buf_staging->right = priv->dest_x + dest_width;
        priv->const_buf_staging->view_width = dest_width;
      }
      break;
    }
    case PROP_DEST_HEIGHT:
    {
      auto dest_height = g_value_get_int (value);
      if (priv->dest_height != dest_height) {
        priv->update_const_buf = TRUE;
        priv->dest_height = dest_height;
        priv->const_buf_staging->bottom = priv->dest_y + dest_height;
        priv->const_buf_staging->view_height = dest_height;
      }
      break;
    }
    case PROP_FILL_BORDER:
    {
      auto fill_border = g_value_get_boolean (value);
      if (priv->fill_border != fill_border) {
        priv->update_const_buf = TRUE;
        priv->fill_border = fill_border;
        priv->const_buf_staging->fill_border = fill_border;
      }
      break;
    }
    case PROP_VIDEO_DIRECTION:
    {
      auto video_direction =
          (GstVideoOrientationMethod) g_value_get_enum (value);
      if (priv->video_direction != video_direction) {
        priv->update_const_buf = TRUE;
        priv->video_direction = video_direction;
        priv->const_buf_staging->video_direction = video_direction;
      }
      break;
    }
    case PROP_ALPHA:
    {
      auto alpha = g_value_get_double (value);
      if (priv->alpha != alpha) {
        priv->update_const_buf = TRUE;
        priv->const_buf_staging->alpha = (float) alpha;
      }
      break;
    }
    case PROP_BLEND:
    {
      auto blend = g_value_get_boolean (value);
      if (priv->blend != blend) {
        priv->update_const_buf = TRUE;
        priv->const_buf_staging->do_blend = blend;
      }
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cuda_converter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  auto self = GST_CUDA_CONVERTER (object);
  auto priv = self->priv;

  std::lock_guard < std::mutex > lk (priv->lock);
  switch (prop_id) {
    case PROP_DEST_X:
      g_value_set_int (value, priv->dest_x);
      break;
    case PROP_DEST_Y:
      g_value_set_int (value, priv->dest_y);
      break;
    case PROP_DEST_WIDTH:
      g_value_set_int (value, priv->dest_width);
      break;
    case PROP_DEST_HEIGHT:
      g_value_set_int (value, priv->dest_height);
      break;
    case PROP_FILL_BORDER:
      g_value_set_boolean (value, priv->fill_border);
      break;
    case PROP_VIDEO_DIRECTION:
      g_value_set_enum (value, priv->video_direction);
      break;
    case PROP_ALPHA:
      g_value_set_double (value, priv->alpha);
      break;
    case PROP_BLEND:
      g_value_set_boolean (value, priv->blend);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static const gchar *
get_color_range_name (GstVideoColorRange range)
{
  switch (range) {
    case GST_VIDEO_COLOR_RANGE_0_255:
      return "FULL";
    case GST_VIDEO_COLOR_RANGE_16_235:
      return "STUDIO";
    default:
      break;
  }

  return "UNKNOWN";
}

static gboolean
gst_cuda_converter_setup (GstCudaConverter * self)
{
  GstCudaConverterPrivate *priv = self->priv;
  const GstVideoInfo *in_info;
  const GstVideoInfo *out_info;
  const GstVideoInfo *texture_info;
  GstCudaColorMatrix to_rgb_matrix;
  GstCudaColorMatrix to_yuv_matrix;
  GstCudaColorMatrix border_color_matrix;
  gdouble border_color[4];
  guint i, j;
  const gchar *unpack_function = nullptr;
  const gchar *write_func = nullptr;
  const gchar *blend_func = nullptr;
  const gchar *to_rgb_func = COLOR_SPACE_IDENTITY;
  const gchar *to_yuv_func = COLOR_SPACE_IDENTITY;
  const GstVideoColorimetry *in_color;
  const GstVideoColorimetry *out_color;
  gchar *str;
  gchar *program = nullptr;
  CUresult ret;

  in_info = &priv->in_info;
  out_info = &priv->out_info;
  texture_info = &priv->texture_info;
  in_color = &in_info->colorimetry;
  out_color = &out_info->colorimetry;

  memset (&to_rgb_matrix, 0, sizeof (GstCudaColorMatrix));
  color_matrix_identity (&to_rgb_matrix);

  memset (&to_yuv_matrix, 0, sizeof (GstCudaColorMatrix));
  color_matrix_identity (&to_yuv_matrix);

  switch (GST_VIDEO_INFO_FORMAT (out_info)) {
    case GST_VIDEO_FORMAT_I420:
      write_func = WRITE_I420;
      blend_func = BLEND_I420;
      break;
    case GST_VIDEO_FORMAT_YV12:
      write_func = WRITE_YV12;
      blend_func = BLEND_YV12;
      break;
    case GST_VIDEO_FORMAT_NV12:
      write_func = WRITE_NV12;
      blend_func = BLEND_NV12;
      break;
    case GST_VIDEO_FORMAT_NV21:
      write_func = WRITE_NV21;
      blend_func = BLEND_NV21;
      break;
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P012_LE:
    case GST_VIDEO_FORMAT_P016_LE:
      write_func = WRITE_P010;
      blend_func = BLEND_P010;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      write_func = WRITE_I420_10;
      blend_func = BLEND_I420_10;
      break;
    case GST_VIDEO_FORMAT_I420_12LE:
      write_func = WRITE_I420_12;
      blend_func = BLEND_I420_12;
      break;
    case GST_VIDEO_FORMAT_Y444:
      write_func = WRITE_Y444;
      blend_func = BLEND_Y444;
      break;
    case GST_VIDEO_FORMAT_Y444_10LE:
      write_func = WRITE_Y444_10;
      blend_func = BLEND_Y444_10;
      break;
    case GST_VIDEO_FORMAT_Y444_12LE:
      write_func = WRITE_Y444_12;
      blend_func = BLEND_Y444_12;
      break;
    case GST_VIDEO_FORMAT_Y444_16LE:
      write_func = WRITE_Y444_16;
      blend_func = BLEND_Y444_16;
      break;
    case GST_VIDEO_FORMAT_RGBA:
      write_func = WRITE_RGBA;
      blend_func = BLEND_RGBA;
      break;
    case GST_VIDEO_FORMAT_RGBx:
      write_func = WRITE_RGBx;
      blend_func = BLEND_RGBx;
      break;
    case GST_VIDEO_FORMAT_BGRA:
      write_func = WRITE_BGRA;
      blend_func = BLEND_BGRA;
      break;
    case GST_VIDEO_FORMAT_BGRx:
      write_func = WRITE_BGRx;
      blend_func = BLEND_BGRx;
      break;
    case GST_VIDEO_FORMAT_ARGB:
      write_func = WRITE_ARGB;
      blend_func = BLEND_ARGB;
      break;
    case GST_VIDEO_FORMAT_ABGR:
      write_func = WRITE_ABGR;
      blend_func = BLEND_ABGR;
      break;
    case GST_VIDEO_FORMAT_RGB:
      write_func = WRITE_RGB;
      blend_func = BLEND_RGB;
      break;
    case GST_VIDEO_FORMAT_BGR:
      write_func = WRITE_BGR;
      blend_func = BLEND_BGR;
      break;
    case GST_VIDEO_FORMAT_RGB10A2_LE:
      write_func = WRITE_RGB10A2;
      blend_func = BLEND_RGB10A2;
      break;
    case GST_VIDEO_FORMAT_BGR10A2_LE:
      write_func = WRITE_BGR10A2;
      blend_func = BLEND_BGR10A2;
      break;
    case GST_VIDEO_FORMAT_Y42B:
      write_func = WRITE_Y42B;
      blend_func = BLEND_Y42B;
      break;
    case GST_VIDEO_FORMAT_I422_10LE:
      write_func = WRITE_I422_10;
      blend_func = BLEND_I422_10;
      break;
    case GST_VIDEO_FORMAT_I422_12LE:
      write_func = WRITE_I422_12;
      blend_func = BLEND_I422_12;
      break;
    case GST_VIDEO_FORMAT_RGBP:
      write_func = WRITE_RGBP;
      blend_func = BLEND_RGBP;
      break;
    case GST_VIDEO_FORMAT_BGRP:
      write_func = WRITE_BGRP;
      blend_func = BLEND_BGRP;
      break;
    case GST_VIDEO_FORMAT_GBR:
      write_func = WRITE_GBR;
      blend_func = BLEND_GBR;
      break;
    case GST_VIDEO_FORMAT_GBR_10LE:
      write_func = WRITE_GBR_10;
      blend_func = BLEND_GBR_10;
      break;
    case GST_VIDEO_FORMAT_GBR_12LE:
      write_func = WRITE_GBR_12;
      blend_func = BLEND_GBR_12;
      break;
    case GST_VIDEO_FORMAT_GBR_16LE:
      write_func = WRITE_GBR_16;
      blend_func = BLEND_GBR_16;
      break;
    case GST_VIDEO_FORMAT_GBRA:
      write_func = WRITE_GBRA;
      blend_func = BLEND_GBRA;
      break;
    case GST_VIDEO_FORMAT_VUYA:
      write_func = WRITE_VUYA;
      blend_func = BLEND_VUYA;
      break;
    default:
      break;
  }

  if (!write_func) {
    GST_ERROR_OBJECT (self, "Unknown write function for format %s",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (out_info)));
    return FALSE;
  }

  /* Decide texture info to use, 3 channel RGB or 10bits packed RGB
   * need be converted to other format */
  priv->texture_info = priv->in_info;
  switch (GST_VIDEO_INFO_FORMAT (in_info)) {
    case GST_VIDEO_FORMAT_RGB:
      gst_video_info_set_format (&priv->texture_info,
          GST_VIDEO_FORMAT_RGBx, GST_VIDEO_INFO_WIDTH (in_info),
          GST_VIDEO_INFO_HEIGHT (in_info));
      unpack_function = RGB_TO_RGBx;
      break;
    case GST_VIDEO_FORMAT_BGR:
      gst_video_info_set_format (&priv->texture_info,
          GST_VIDEO_FORMAT_BGRx, GST_VIDEO_INFO_WIDTH (in_info),
          GST_VIDEO_INFO_HEIGHT (in_info));
      unpack_function = RGB_TO_RGBx;
      break;
    case GST_VIDEO_FORMAT_RGB10A2_LE:
      gst_video_info_set_format (&priv->texture_info,
          GST_VIDEO_FORMAT_ARGB64, GST_VIDEO_INFO_WIDTH (in_info),
          GST_VIDEO_INFO_HEIGHT (in_info));
      unpack_function = RGB10A2_TO_ARGB64;
      break;
    case GST_VIDEO_FORMAT_BGR10A2_LE:
      gst_video_info_set_format (&priv->texture_info,
          GST_VIDEO_FORMAT_ARGB64, GST_VIDEO_INFO_WIDTH (in_info),
          GST_VIDEO_INFO_HEIGHT (in_info));
      unpack_function = BGR10A2_TO_ARGB64;
      break;
    default:
      break;
  }

  for (i = 0; i < G_N_ELEMENTS (format_map); i++) {
    if (format_map[i].format == GST_VIDEO_INFO_FORMAT (texture_info)) {
      priv->texture_fmt = &format_map[i];
      break;
    }
  }

  if (!priv->texture_fmt) {
    GST_ERROR_OBJECT (self, "Couldn't find texture format for %s (%s)",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (in_info)),
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (texture_info)));
    return FALSE;
  }

  /* calculate black color
   * TODO: add support border color */
  if (GST_VIDEO_INFO_IS_RGB (out_info)) {
    GstVideoInfo rgb_info = *out_info;
    rgb_info.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
    gst_cuda_color_range_adjust_matrix_unorm (&rgb_info, out_info,
        &border_color_matrix);
  } else {
    GstVideoInfo rgb_info;

    gst_video_info_set_format (&rgb_info, GST_VIDEO_FORMAT_RGBA64_LE,
        out_info->width, out_info->height);

    gst_cuda_rgb_to_yuv_matrix_unorm (&rgb_info,
        out_info, &border_color_matrix);
  }

  for (i = 0; i < 3; i++) {
    /* TODO: property */
    gdouble border_rgba[4] = { 0, 0, 0 };
    border_color[i] = 0;
    for (j = 0; j < 3; j++)
      border_color[i] += border_color_matrix.matrix[i][j] * border_rgba[i];
    border_color[i] = border_color_matrix.offset[i];
    border_color[i] = CLAMP (border_color[i],
        border_color_matrix.min[i], border_color_matrix.max[i]);
  }

  /* FIXME: handle primaries and transfer functions */
  if (GST_VIDEO_INFO_IS_RGB (texture_info)) {
    if (GST_VIDEO_INFO_IS_RGB (out_info)) {
      /* RGB -> RGB */
      if (in_color->range == out_color->range) {
        GST_DEBUG_OBJECT (self, "RGB -> RGB conversion without matrix");
      } else {
        if (!gst_cuda_color_range_adjust_matrix_unorm (in_info, out_info,
                &to_rgb_matrix)) {
          GST_ERROR_OBJECT (self, "Failed to get RGB range adjust matrix");
          return FALSE;
        }

        str = gst_cuda_dump_color_matrix (&to_rgb_matrix);
        GST_DEBUG_OBJECT (self, "RGB range adjust %s -> %s\n%s",
            get_color_range_name (in_color->range),
            get_color_range_name (out_color->range), str);
        g_free (str);

        to_rgb_func = COLOR_SPACE_CONVERT;
      }
    } else {
      /* RGB -> YUV */
      if (!gst_cuda_rgb_to_yuv_matrix_unorm (in_info, out_info, &to_yuv_matrix)) {
        GST_ERROR_OBJECT (self, "Failed to get RGB -> YUV transform matrix");
        return FALSE;
      }

      str = gst_cuda_dump_color_matrix (&to_yuv_matrix);
      GST_DEBUG_OBJECT (self, "RGB -> YUV matrix:\n%s", str);
      g_free (str);

      to_yuv_func = COLOR_SPACE_CONVERT;
    }
  } else {
    if (GST_VIDEO_INFO_IS_RGB (out_info)) {
      /* YUV -> RGB */
      if (!gst_cuda_yuv_to_rgb_matrix_unorm (in_info, out_info, &to_rgb_matrix)) {
        GST_ERROR_OBJECT (self, "Failed to get YUV -> RGB transform matrix");
        return FALSE;
      }

      str = gst_cuda_dump_color_matrix (&to_rgb_matrix);
      GST_DEBUG_OBJECT (self, "YUV -> RGB matrix:\n%s", str);
      g_free (str);

      to_rgb_func = COLOR_SPACE_CONVERT;
    } else {
      /* YUV -> YUV */
      if (in_color->range == out_color->range) {
        GST_DEBUG_OBJECT (self, "YUV -> YU conversion without matrix");
      } else {
        if (!gst_cuda_color_range_adjust_matrix_unorm (in_info, out_info,
                &to_yuv_matrix)) {
          GST_ERROR_OBJECT (self, "Failed to get GRAY range adjust matrix");
          return FALSE;
        }

        str = gst_cuda_dump_color_matrix (&to_yuv_matrix);
        GST_DEBUG_OBJECT (self, "YUV range adjust matrix:\n%s", str);
        g_free (str);

        to_yuv_func = COLOR_SPACE_CONVERT;
      }
    }
  }

  for (i = 0; i < 3; i++) {
    priv->const_buf_staging->toRGBCoeff.coeffX[i] = to_rgb_matrix.matrix[0][i];
    priv->const_buf_staging->toRGBCoeff.coeffY[i] = to_rgb_matrix.matrix[1][i];
    priv->const_buf_staging->toRGBCoeff.coeffZ[i] = to_rgb_matrix.matrix[2][i];
    priv->const_buf_staging->toRGBCoeff.offset[i] = to_rgb_matrix.offset[i];
    priv->const_buf_staging->toRGBCoeff.min[i] = to_rgb_matrix.min[i];
    priv->const_buf_staging->toRGBCoeff.max[i] = to_rgb_matrix.max[i];

    priv->const_buf_staging->toYuvCoeff.coeffX[i] = to_yuv_matrix.matrix[0][i];
    priv->const_buf_staging->toYuvCoeff.coeffY[i] = to_yuv_matrix.matrix[1][i];
    priv->const_buf_staging->toYuvCoeff.coeffZ[i] = to_yuv_matrix.matrix[2][i];
    priv->const_buf_staging->toYuvCoeff.offset[i] = to_yuv_matrix.offset[i];
    priv->const_buf_staging->toYuvCoeff.min[i] = to_yuv_matrix.min[i];
    priv->const_buf_staging->toYuvCoeff.max[i] = to_yuv_matrix.max[i];
  }

  priv->const_buf_staging->width = out_info->width;
  priv->const_buf_staging->height = out_info->height;
  priv->const_buf_staging->left = 0;
  priv->const_buf_staging->top = 0;
  priv->const_buf_staging->right = out_info->width;
  priv->const_buf_staging->bottom = out_info->height;
  priv->const_buf_staging->view_width = out_info->width;
  priv->const_buf_staging->view_height = out_info->height;
  priv->const_buf_staging->border_x = border_color[0];
  priv->const_buf_staging->border_y = border_color[1];
  priv->const_buf_staging->border_z = border_color[2];
  priv->const_buf_staging->border_w = border_color[3];
  priv->const_buf_staging->fill_border = 0;
  priv->const_buf_staging->video_direction = 0;
  priv->const_buf_staging->alpha = 1;
  priv->const_buf_staging->do_blend = 0;

  str = g_strdup_printf (TEMPLATE_KERNEL, KERNEL_COMMON,
      unpack_function ? unpack_function : "",
      /* sampler function name */
      priv->texture_fmt->sample_func,
      /* TO RGB conversion function name */
      to_rgb_func,
      /* TO YUV conversion function name */
      to_yuv_func,
      /* write function name */
      write_func,
      /* blend function name */
      blend_func);

  GST_LOG_OBJECT (self, "kernel code:\n%s\n", str);
  gint cuda_device;
  g_object_get (self->context, "cuda-device-id", &cuda_device, nullptr);
  program = gst_cuda_nvrtc_compile_cubin (str, cuda_device);
  if (!program) {
    GST_WARNING_OBJECT (self, "Couldn't compile to cubin, trying ptx");
    program = gst_cuda_nvrtc_compile (str);
  }
  g_free (str);

  if (!program) {
    GST_ERROR_OBJECT (self, "Could not compile code");
    return FALSE;
  }

  if (!gst_cuda_context_push (self->context)) {
    GST_ERROR_OBJECT (self, "Couldn't push context");
    g_free (program);
    return FALSE;
  }

  /* Allocates intermediate memory for texture */
  if (unpack_function) {
    CUDA_TEXTURE_DESC texture_desc;
    CUDA_RESOURCE_DESC resource_desc;
    CUtexObject texture = 0;

    memset (&texture_desc, 0, sizeof (CUDA_TEXTURE_DESC));
    memset (&resource_desc, 0, sizeof (CUDA_RESOURCE_DESC));

    ret = CuMemAllocPitch (&priv->unpack_buffer.ptr,
        &priv->unpack_buffer.stride,
        GST_VIDEO_INFO_COMP_WIDTH (texture_info, 0) *
        GST_VIDEO_INFO_COMP_PSTRIDE (texture_info, 0),
        GST_VIDEO_INFO_HEIGHT (texture_info), 16);
    if (!gst_cuda_result (ret)) {
      GST_ERROR_OBJECT (self, "Couldn't allocate unpack buffer");
      goto error;
    }

    resource_desc.resType = CU_RESOURCE_TYPE_PITCH2D;
    resource_desc.res.pitch2D.format = priv->texture_fmt->array_format[0];
    resource_desc.res.pitch2D.numChannels = 4;
    resource_desc.res.pitch2D.width = in_info->width;
    resource_desc.res.pitch2D.height = in_info->height;
    resource_desc.res.pitch2D.pitchInBytes = priv->unpack_buffer.stride;
    resource_desc.res.pitch2D.devPtr = priv->unpack_buffer.ptr;

    texture_desc.filterMode = priv->filter_mode;
    texture_desc.flags = 0x2;
    texture_desc.addressMode[0] = (CUaddress_mode) 1;
    texture_desc.addressMode[1] = (CUaddress_mode) 1;
    texture_desc.addressMode[2] = (CUaddress_mode) 1;

    ret = CuTexObjectCreate (&texture, &resource_desc, &texture_desc, nullptr);
    if (!gst_cuda_result (ret)) {
      GST_ERROR_OBJECT (self, "Couldn't create unpack texture");
      goto error;
    }

    priv->unpack_buffer.texture = texture;
  }

  ret = CuModuleLoadData (&priv->module, program);
  g_clear_pointer (&program, g_free);
  if (!gst_cuda_result (ret)) {
    GST_ERROR_OBJECT (self, "Could not load module");
    priv->module = nullptr;
    goto error;
  }

  ret = CuModuleGetFunction (&priv->main_func,
      priv->module, GST_CUDA_KERNEL_MAIN_FUNC);
  if (!gst_cuda_result (ret)) {
    GST_ERROR_OBJECT (self, "Could not get main function");
    goto error;
  }

  if (unpack_function) {
    ret = CuModuleGetFunction (&priv->unpack_func,
        priv->module, GST_CUDA_KERNEL_UNPACK_FUNC);
    if (!gst_cuda_result (ret)) {
      GST_ERROR_OBJECT (self, "Could not get unpack function");
      goto error;
    }
  }

  ret = CuMemcpyHtoD (priv->const_buf,
      priv->const_buf_staging, sizeof (ConstBuffer));
  if (!gst_cuda_result (ret)) {
    GST_ERROR_OBJECT (self, "Could upload const buf");
    goto error;
  }

  gst_cuda_context_pop (nullptr);

  return TRUE;

error:
  gst_cuda_context_pop (nullptr);
  g_free (program);

  return FALSE;
}

static gboolean
copy_config (const GstIdStr * fieldname, const GValue * value,
    gpointer user_data)
{
  GstCudaConverter *self = (GstCudaConverter *) user_data;

  gst_structure_id_str_set_value (self->priv->config, fieldname, value);

  return TRUE;
}

static void
gst_cuda_converter_set_config (GstCudaConverter * self, GstStructure * config)
{
  gst_structure_foreach_id_str (config, copy_config, self);
  gst_structure_free (config);
}

GstCudaConverter *
gst_cuda_converter_new (const GstVideoInfo * in_info,
    const GstVideoInfo * out_info, GstCudaContext * context,
    GstStructure * config)
{
  GstCudaConverter *self;
  GstCudaConverterPrivate *priv;
  CUresult cuda_ret;

  g_return_val_if_fail (in_info != nullptr, nullptr);
  g_return_val_if_fail (out_info != nullptr, nullptr);
  g_return_val_if_fail (GST_IS_CUDA_CONTEXT (context), nullptr);

  self = (GstCudaConverter *) g_object_new (GST_TYPE_CUDA_CONVERTER, nullptr);

  if (!GST_IS_CUDA_CONTEXT (context)) {
    GST_WARNING_OBJECT (self, "Not a valid cuda context object");
    goto error;
  }

  self->context = (GstCudaContext *) gst_object_ref (context);
  priv = self->priv;
  priv->in_info = *in_info;
  priv->out_info = *out_info;
  priv->dest_width = out_info->width;
  priv->dest_height = out_info->height;

  if (config)
    gst_cuda_converter_set_config (self, config);

  if (!gst_cuda_context_push (context)) {
    GST_ERROR_OBJECT (self, "Couldn't push context");
    goto error;
  }

  cuda_ret = CuMemAllocHost ((void **) &priv->const_buf_staging,
      sizeof (ConstBuffer));
  if (!gst_cuda_result (cuda_ret)) {
    GST_ERROR_OBJECT (self, "Couldn't allocate staging const buf");
    gst_cuda_context_pop (nullptr);
    goto error;
  }

  cuda_ret = CuMemAlloc (&priv->const_buf, sizeof (ConstBuffer));
  gst_cuda_context_pop (nullptr);
  if (!gst_cuda_result (cuda_ret)) {
    GST_ERROR_OBJECT (self, "Couldn't allocate const buf");
    goto error;
  }

  if (!gst_cuda_converter_setup (self))
    goto error;

  priv->texture_align = gst_cuda_context_get_texture_alignment (context);

  gst_object_ref_sink (self);
  return self;

error:
  gst_object_unref (self);
  return nullptr;
}

static CUtexObject
gst_cuda_converter_create_texture_unchecked (GstCudaConverter * self,
    CUdeviceptr src, gint width, gint height, CUarray_format format,
    guint channels, gint stride, gint plane, CUfilter_mode mode)
{
  CUDA_TEXTURE_DESC texture_desc;
  CUDA_RESOURCE_DESC resource_desc;
  CUtexObject texture = 0;
  CUresult cuda_ret;

  memset (&texture_desc, 0, sizeof (CUDA_TEXTURE_DESC));
  memset (&resource_desc, 0, sizeof (CUDA_RESOURCE_DESC));

  resource_desc.resType = CU_RESOURCE_TYPE_PITCH2D;
  resource_desc.res.pitch2D.format = format;
  resource_desc.res.pitch2D.numChannels = channels;
  resource_desc.res.pitch2D.width = width;
  resource_desc.res.pitch2D.height = height;
  resource_desc.res.pitch2D.pitchInBytes = stride;
  resource_desc.res.pitch2D.devPtr = src;

  texture_desc.filterMode = mode;
  /* Will read texture value as a normalized [0, 1] float value
   * with [0, 1) coordinates */
  /* CU_TRSF_NORMALIZED_COORDINATES */
  texture_desc.flags = 0x2;
  /* CU_TR_ADDRESS_MODE_CLAMP */
  texture_desc.addressMode[0] = (CUaddress_mode) 1;
  texture_desc.addressMode[1] = (CUaddress_mode) 1;
  texture_desc.addressMode[2] = (CUaddress_mode) 1;

  cuda_ret =
      CuTexObjectCreate (&texture, &resource_desc, &texture_desc, nullptr);

  if (!gst_cuda_result (cuda_ret)) {
    GST_ERROR_OBJECT (self, "Could not create texture");
    return 0;
  }

  return texture;
}

static gboolean
ensure_fallback_buffer (GstCudaConverter * self, gint width_in_bytes,
    gint height, guint plane)
{
  GstCudaConverterPrivate *priv = self->priv;
  CUresult ret;

  if (priv->fallback_buffer[plane].ptr)
    return TRUE;

  ret = CuMemAllocPitch (&priv->fallback_buffer[plane].ptr,
      &priv->fallback_buffer[plane].stride, width_in_bytes, height, 16);

  if (!gst_cuda_result (ret)) {
    GST_ERROR_OBJECT (self, "Couldn't allocate fallback buffer");
    return FALSE;
  }

  return TRUE;
}

static CUtexObject
gst_cuda_converter_create_texture (GstCudaConverter * self,
    CUdeviceptr src, gint width, gint height, gint stride, CUfilter_mode mode,
    CUarray_format format, guint channles, gint plane, CUstream stream)
{
  GstCudaConverterPrivate *priv = self->priv;
  CUresult ret;
  CUdeviceptr src_ptr;
  CUDA_MEMCPY2D params = { 0, };

  if (!ensure_fallback_buffer (self, stride, height, plane))
    return 0;

  params.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  params.srcPitch = stride;
  params.srcDevice = (CUdeviceptr) src;

  params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  params.dstPitch = priv->fallback_buffer[plane].stride;
  params.dstDevice = priv->fallback_buffer[plane].ptr;
  params.WidthInBytes = GST_VIDEO_INFO_COMP_WIDTH (&priv->in_info, plane)
      * GST_VIDEO_INFO_COMP_PSTRIDE (&priv->in_info, plane),
      params.Height = GST_VIDEO_INFO_COMP_HEIGHT (&priv->in_info, plane);

  ret = CuMemcpy2DAsync (&params, stream);
  if (!gst_cuda_result (ret)) {
    GST_ERROR_OBJECT (self, "Couldn't copy to fallback buffer");
    return 0;
  }

  if (!priv->fallback_buffer[plane].texture) {
    src_ptr = priv->fallback_buffer[plane].ptr;
    stride = priv->fallback_buffer[plane].stride;

    priv->fallback_buffer[plane].texture =
        gst_cuda_converter_create_texture_unchecked (self, src_ptr, width,
        height, format, channles, stride, plane, mode);
  }

  return priv->fallback_buffer[plane].texture;
}

static gboolean
gst_cuda_converter_unpack_rgb (GstCudaConverter * self,
    GstVideoFrame * src_frame, CUstream stream)
{
  GstCudaConverterPrivate *priv = self->priv;
  CUdeviceptr src;
  gint width, height, src_stride, dst_stride;
  CUresult ret;
  gpointer args[] = { &src, &priv->unpack_buffer.ptr,
    &width, &height, &src_stride, &dst_stride
  };

  g_assert (priv->unpack_buffer.ptr);
  g_assert (priv->unpack_buffer.stride > 0);

  src = (CUdeviceptr) GST_VIDEO_FRAME_PLANE_DATA (src_frame, 0);
  width = GST_VIDEO_FRAME_WIDTH (src_frame);
  height = GST_VIDEO_FRAME_HEIGHT (src_frame);
  src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (src_frame, 0);
  dst_stride = (gint) priv->unpack_buffer.stride;

  ret = CuLaunchKernel (priv->unpack_func, DIV_UP (width, CUDA_BLOCK_X),
      DIV_UP (height, CUDA_BLOCK_Y), 1, CUDA_BLOCK_X, CUDA_BLOCK_Y, 1, 0,
      stream, args, nullptr);

  if (!gst_cuda_result (ret)) {
    GST_ERROR_OBJECT (self, "Couldn't unpack source RGB");
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_cuda_converter_convert_frame (GstCudaConverter * converter,
    GstVideoFrame * src_frame, GstVideoFrame * dst_frame, CUstream stream,
    gboolean * synchronized)
{
  GstCudaConverterPrivate *priv;
  const TextureFormat *format;
  CUtexObject texture[GST_VIDEO_MAX_COMPONENTS] = { 0, };
  guint8 *dst[GST_VIDEO_MAX_COMPONENTS] = { nullptr, };
  gint stride[2] = { 0, };
  guint i;
  gboolean ret = FALSE;
  CUresult cuda_ret;
  gint width, height;
  gboolean need_sync = FALSE;
  GstCudaMemory *cmem;
  gint off_x = 0;
  gint off_y = 0;

  g_return_val_if_fail (GST_IS_CUDA_CONVERTER (converter), FALSE);
  g_return_val_if_fail (src_frame != nullptr, FALSE);
  g_return_val_if_fail (dst_frame != nullptr, FALSE);

  priv = converter->priv;
  format = priv->texture_fmt;

  g_assert (format);

  std::lock_guard < std::mutex > lk (priv->lock);
  if (!priv->fill_border && (priv->dest_width <= 0 || priv->dest_height <= 0))
    return TRUE;

  if (priv->update_const_buf) {
    priv->update_const_buf = FALSE;
    cuda_ret = CuMemcpyHtoDAsync (priv->const_buf, priv->const_buf_staging,
        sizeof (ConstBuffer), stream);

    if (!gst_cuda_result (cuda_ret)) {
      GST_ERROR_OBJECT (converter, "Couldn't upload const buffer");
      return FALSE;
    }
  }

  gpointer args[] = { &texture[0], &texture[1], &texture[2], &texture[3],
    &dst[0], &dst[1], &dst[2], &dst[3], &stride[0], &stride[1],
    &priv->const_buf, &off_x, &off_y
  };

  cmem = (GstCudaMemory *) gst_buffer_peek_memory (src_frame->buffer, 0);
  g_return_val_if_fail (gst_is_cuda_memory (GST_MEMORY_CAST (cmem)), FALSE);

  if (!gst_cuda_context_push (converter->context)) {
    GST_ERROR_OBJECT (converter, "Couldn't push context");
    return FALSE;
  }

  if (priv->unpack_func) {
    if (!gst_cuda_converter_unpack_rgb (converter, src_frame, stream))
      goto out;

    texture[0] = priv->unpack_buffer.texture;
    if (!texture[0]) {
      GST_ERROR_OBJECT (converter, "Unpack texture is unavailable");
      goto out;
    }
  } else {
    for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (src_frame); i++) {
      if (!gst_cuda_memory_get_texture (cmem,
              i, priv->filter_mode, &texture[i])) {
        CUdeviceptr src;
        src = (CUdeviceptr) GST_VIDEO_FRAME_PLANE_DATA (src_frame, i);
        texture[i] = gst_cuda_converter_create_texture (converter,
            src, GST_VIDEO_FRAME_COMP_WIDTH (src_frame, i),
            GST_VIDEO_FRAME_COMP_HEIGHT (src_frame, i),
            GST_VIDEO_FRAME_PLANE_STRIDE (src_frame, i),
            priv->filter_mode, format->array_format[i], format->channels[i],
            i, stream);
        need_sync = TRUE;
      }

      if (!texture[i]) {
        GST_ERROR_OBJECT (converter, "Couldn't create texture %d", i);
        goto out;
      }
    }
  }

  width = GST_VIDEO_FRAME_WIDTH (dst_frame);
  height = GST_VIDEO_FRAME_HEIGHT (dst_frame);

  if (!priv->fill_border) {
    if (priv->dest_width < width) {
      off_x = priv->dest_x;
      width = priv->dest_width;
    }

    if (priv->dest_height < height) {
      off_y = priv->dest_y;
      height = priv->dest_height;
    }
  }

  for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (dst_frame); i++)
    dst[i] = (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (dst_frame, i);

  stride[0] = stride[1] = GST_VIDEO_FRAME_PLANE_STRIDE (dst_frame, 0);
  if (GST_VIDEO_FRAME_N_PLANES (dst_frame) > 1)
    stride[1] = GST_VIDEO_FRAME_PLANE_STRIDE (dst_frame, 1);

  cuda_ret = CuLaunchKernel (priv->main_func, DIV_UP (width, CUDA_BLOCK_X),
      DIV_UP (height, CUDA_BLOCK_Y), 1, CUDA_BLOCK_X, CUDA_BLOCK_Y, 1, 0,
      stream, args, nullptr);

  if (!gst_cuda_result (cuda_ret)) {
    GST_ERROR_OBJECT (converter, "Couldn't convert frame");
    goto out;
  }

  if (need_sync)
    CuStreamSynchronize (stream);

  if (synchronized)
    *synchronized = need_sync;

  ret = TRUE;

out:
  gst_cuda_context_pop (nullptr);
  return ret;
}
