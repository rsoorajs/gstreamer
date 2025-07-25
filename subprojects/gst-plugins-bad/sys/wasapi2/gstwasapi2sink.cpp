/*
 * Copyright (C) 2008 Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>
 * Copyright (C) 2013 Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 * Copyright (C) 2018 Centricular Ltd.
 *   Author: Nirbheek Chauhan <nirbheek@centricular.com>
 * Copyright (C) 2020 Seungha Yang <seungha@centricular.com>
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

/**
 * SECTION:element-wasapi2sink
 * @title: wasapi2sink
 *
 * Provides audio playback using the Windows Audio Session API available with
 * Windows 10.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 -v audiotestsrc ! wasapi2sink
 * ]| Generate audio test buffers and render to the default audio device.
 *
 * |[
 * gst-launch-1.0 -v audiotestsink samplesperbuffer=160 ! wasapi2sink low-latency=true
 * ]| Same as above, but with the minimum possible latency
 *
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstwasapi2sink.h"
#include "gstwasapi2util.h"
#include "gstwasapi2rbuf.h"
#include <mutex>
#include <atomic>

GST_DEBUG_CATEGORY_STATIC (gst_wasapi2_sink_debug);
#define GST_CAT_DEFAULT gst_wasapi2_sink_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_WASAPI2_STATIC_CAPS));

#define DEFAULT_LOW_LATENCY   FALSE
#define DEFAULT_MUTE          FALSE
#define DEFAULT_VOLUME        1.0
#define DEFAULT_CONTINUE_ON_ERROR FALSE

enum
{
  PROP_0,
  PROP_DEVICE,
  PROP_LOW_LATENCY,
  PROP_MUTE,
  PROP_VOLUME,
  PROP_DISPATCHER,
  PROP_CONTINUE_ON_ERROR,
};

/* *INDENT-OFF* */
struct GstWasapi2SinkPrivate
{
  ~GstWasapi2SinkPrivate ()
  {
    gst_object_unref (rbuf);
    g_free (device_id);
  }

  GstWasapi2Rbuf *rbuf = nullptr;

  std::mutex lock;
  std::atomic<bool> device_invalidated = { false };

  /* properties */
  gchar *device_id = nullptr;;
  gboolean low_latency = DEFAULT_LOW_LATENCY;
  gboolean continue_on_error = DEFAULT_CONTINUE_ON_ERROR;
};
/* *INDENT-ON* */

struct _GstWasapi2Sink
{
  GstAudioBaseSink parent;

  GstWasapi2SinkPrivate *priv;
};

static void gst_wasapi2_sink_finalize (GObject * object);
static void gst_wasapi2_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_wasapi2_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstCaps *gst_wasapi2_sink_get_caps (GstBaseSink * bsink,
    GstCaps * filter);
static GstAudioRingBuffer *gst_wasapi2_sink_create_ringbuffer (GstAudioBaseSink
    * sink);

#define gst_wasapi2_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstWasapi2Sink, gst_wasapi2_sink,
    GST_TYPE_AUDIO_BASE_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_STREAM_VOLUME, nullptr));

static void
gst_wasapi2_sink_class_init (GstWasapi2SinkClass * klass)
{
  auto gobject_class = G_OBJECT_CLASS (klass);
  auto element_class = GST_ELEMENT_CLASS (klass);
  auto basesink_class = GST_BASE_SINK_CLASS (klass);
  auto audiobasesink_class = GST_AUDIO_BASE_SINK_CLASS (klass);

  gobject_class->finalize = gst_wasapi2_sink_finalize;
  gobject_class->set_property = gst_wasapi2_sink_set_property;
  gobject_class->get_property = gst_wasapi2_sink_get_property;

  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_string ("device", "Device",
          "Audio device ID as provided by "
          "WASAPI device endpoint ID as provided by IMMDevice::GetId",
          nullptr, (GParamFlags) (GST_PARAM_MUTABLE_READY | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_LOW_LATENCY,
      g_param_spec_boolean ("low-latency", "Low latency",
          "Optimize all settings for lowest latency. Always safe to enable.",
          DEFAULT_LOW_LATENCY, (GParamFlags) (GST_PARAM_MUTABLE_READY |
              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MUTE,
      g_param_spec_boolean ("mute", "Mute", "Mute state of this stream",
          DEFAULT_MUTE, (GParamFlags) (GST_PARAM_MUTABLE_PLAYING |
              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_VOLUME,
      g_param_spec_double ("volume", "Volume", "Volume of this stream",
          0.0, 1.0, DEFAULT_VOLUME,
          (GParamFlags) (GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS)));

  /**
   * GstWasapi2Sink:dispatcher:
   *
   * ICoreDispatcher COM object used for activating device from UI thread.
   *
   * Since: 1.18
   */
  g_object_class_install_property (gobject_class, PROP_DISPATCHER,
      g_param_spec_pointer ("dispatcher", "Dispatcher",
          "ICoreDispatcher COM object to use. In order for application to ask "
          "permission of audio device, device activation should be running "
          "on UI thread via ICoreDispatcher. This element will increase "
          "the reference count of given ICoreDispatcher and release it after "
          "use. Therefore, caller does not need to consider additional "
          "reference count management",
          (GParamFlags) (GST_PARAM_MUTABLE_READY | G_PARAM_WRITABLE |
              G_PARAM_STATIC_STRINGS)));

  /**
   * GstWasapi2Sink:continue-on-error:
   *
   * If enabled, wasapi2sink will post a warning message instead of an error,
   * when device failures occur, such as open failure, I/O error,
   * or device removal.
   * The element will continue to consume audio buffers and behave as if
   * a render device were active, allowing pipeline to keep running even when
   * no audio endpoint is available
   *
   * Since: 1.28
   */
  g_object_class_install_property (gobject_class, PROP_CONTINUE_ON_ERROR,
      g_param_spec_boolean ("continue-on-error", "Continue On Error",
          "Continue running and consume buffers on device failure",
          DEFAULT_CONTINUE_ON_ERROR, (GParamFlags) (GST_PARAM_MUTABLE_READY |
              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_set_static_metadata (element_class, "Wasapi2Sink",
      "Sink/Audio/Hardware",
      "Stream audio to an audio capture device through WASAPI",
      "Seungha Yang <seungha@centricular.com>");

  basesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_wasapi2_sink_get_caps);

  audiobasesink_class->create_ringbuffer =
      GST_DEBUG_FUNCPTR (gst_wasapi2_sink_create_ringbuffer);

  GST_DEBUG_CATEGORY_INIT (gst_wasapi2_sink_debug, "wasapi2sink",
      0, "Windows audio session API sink");
}

static void
gst_wasapi2_sink_on_invalidated (gpointer elem)
{
  auto self = GST_WASAPI2_SINK (elem);
  auto priv = self->priv;

  GST_WARNING_OBJECT (self, "Device invalidated");

  priv->device_invalidated = true;
}

static void
gst_wasapi2_sink_init (GstWasapi2Sink * self)
{
  auto priv = new GstWasapi2SinkPrivate ();

  priv->rbuf = gst_wasapi2_rbuf_new (self, gst_wasapi2_sink_on_invalidated);
  gst_wasapi2_rbuf_set_device (priv->rbuf, nullptr,
      GST_WASAPI2_ENDPOINT_CLASS_RENDER, 0, DEFAULT_LOW_LATENCY);

  self->priv = priv;
}

static void
gst_wasapi2_sink_finalize (GObject * object)
{
  auto self = GST_WASAPI2_SINK (object);

  GST_LOG_OBJECT (self, "Finalize");

  delete self->priv;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_wasapi2_sink_set_device (GstWasapi2Sink * self, bool updated)
{
  auto priv = self->priv;
  bool expected = true;
  bool set_device = priv->device_invalidated.compare_exchange_strong (expected,
      false);

  if (!set_device && !updated)
    return;

  gst_wasapi2_rbuf_set_device (priv->rbuf, priv->device_id,
      GST_WASAPI2_ENDPOINT_CLASS_RENDER, 0, priv->low_latency);
}

static void
gst_wasapi2_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  auto self = GST_WASAPI2_SINK (object);
  auto priv = self->priv;

  std::lock_guard < std::mutex > lk (priv->lock);

  switch (prop_id) {
    case PROP_DEVICE:
    {
      auto new_val = g_value_get_string (value);
      bool updated = false;
      if (g_strcmp0 (new_val, priv->device_id) != 0) {
        g_free (priv->device_id);
        priv->device_id = g_strdup (new_val);
        updated = true;
      }

      gst_wasapi2_sink_set_device (self, updated);
      break;
    }
    case PROP_LOW_LATENCY:
    {
      auto new_val = g_value_get_boolean (value);
      bool updated = false;
      if (new_val != priv->low_latency) {
        priv->low_latency = new_val;
        updated = true;
      }

      gst_wasapi2_sink_set_device (self, updated);
      break;
    }
    case PROP_MUTE:
      gst_wasapi2_rbuf_set_mute (priv->rbuf, g_value_get_boolean (value));
      break;
    case PROP_VOLUME:
      gst_wasapi2_rbuf_set_volume (priv->rbuf, g_value_get_double (value));
      break;
    case PROP_DISPATCHER:
      /* Unused */
      break;
    case PROP_CONTINUE_ON_ERROR:
      priv->continue_on_error = g_value_get_boolean (value);
      gst_wasapi2_rbuf_set_continue_on_error (priv->rbuf,
          priv->continue_on_error);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wasapi2_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  auto self = GST_WASAPI2_SINK (object);
  auto priv = self->priv;

  std::lock_guard < std::mutex > lk (priv->lock);

  switch (prop_id) {
    case PROP_DEVICE:
      g_value_set_string (value, priv->device_id);
      break;
    case PROP_LOW_LATENCY:
      g_value_set_boolean (value, priv->low_latency);
      break;
    case PROP_MUTE:
      g_value_set_boolean (value, gst_wasapi2_rbuf_get_mute (priv->rbuf));
      break;
    case PROP_VOLUME:
      g_value_set_double (value, gst_wasapi2_rbuf_get_volume (priv->rbuf));
      break;
    case PROP_CONTINUE_ON_ERROR:
      g_value_set_boolean (value, priv->continue_on_error);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_wasapi2_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  auto self = GST_WASAPI2_SINK (bsink);
  auto priv = self->priv;
  auto caps = gst_wasapi2_rbuf_get_caps (priv->rbuf);

  if (!caps)
    caps = gst_pad_get_pad_template_caps (bsink->sinkpad);

  if (filter) {
    GstCaps *filtered =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = filtered;
  }

  GST_DEBUG_OBJECT (self, "returning caps %" GST_PTR_FORMAT, caps);

  return caps;
}

static GstAudioRingBuffer *
gst_wasapi2_sink_create_ringbuffer (GstAudioBaseSink * sink)
{
  auto self = GST_WASAPI2_SINK (sink);
  auto priv = self->priv;

  return GST_AUDIO_RING_BUFFER (priv->rbuf);
}
