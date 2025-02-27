/*
 * GStreamer - GStreamer SRTP encoder
 *
 * Copyright 2011-2013 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
#ifndef __GST_SRTP_H__
#define __GST_SRTP_H__

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstsrtpenums.h"
#include "gstsrtp-enumtypes.h"

#include <gst/gst.h>

#include <srtp2/srtp.h>
#include <srtp2/crypto_types.h>

void     gst_srtp_init_event_reporter    (void);
gboolean gst_srtp_get_soft_limit_reached (void);

gboolean rtcp_buffer_get_ssrc (GstBuffer * buf, guint32 * ssrc);

const gchar *enum_nick_from_value (GType enum_gtype, gint value);
gint enum_value_from_nick (GType enum_gtype, const gchar *nick);

void set_crypto_policy_cipher_auth (GstSrtpCipherType cipher,
    GstSrtpAuthType auth, srtp_crypto_policy_t * policy);

guint cipher_key_size (GstSrtpCipherType cipher);

#endif /* __GST_SRTP_H__ */
