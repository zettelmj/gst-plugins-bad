/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>

#include "gstspectrum.h"

#define SPECTRUM_DEFAULT_WIDTH 75
#define SPECTRUM_MAX_WIDTH     1024     /* limited by the current FFT code */

/* elementfactory information */
static GstElementDetails gst_spectrum_details =
GST_ELEMENT_DETAILS ("Spectrum analyzer",
    "Filter/Analyzer/Audio",
    "Run an FFT on the audio signal, output spectrum data",
    "Erik Walthinsen <omega@cse.ogi.edu>");

static GstStaticPadTemplate spectrum_sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw-int, "
        "endianness = (int) " G_STRINGIFY (G_BYTE_ORDER) ", "
        "signed = (boolean) true, "
        "width = (int) 16, "
        "depth = (int) 16, "
        "rate = (int) { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 }, "
        "channels = (int) [ 1, 2 ]")
    );


enum
{
  ARG_0,
  ARG_WIDTH
};


static void gst_spectrum_base_init (gpointer g_class);
static void gst_spectrum_class_init (GstSpectrumClass * klass);
static void gst_spectrum_init (GstSpectrum * spectrum);

static void gst_spectrum_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static GstPadLinkReturn gst_spectrum_link (GstPad * pad, const GstCaps * caps);
static void gst_spectrum_chain (GstPad * pad, GstData * _data);

#define fixed gint16
int gst_spectrum_fix_fft (fixed fr[], fixed fi[], int m, int inverse);
void gst_spectrum_fix_loud (fixed loud[], fixed fr[], fixed fi[], int n,
    int scale_shift);
void gst_spectrum_window (fixed fr[], int n);


static GstElementClass *parent_class = NULL;

GType
gst_spectrum_get_type (void)
{
  static GType spectrum_type = 0;

  if (!spectrum_type) {
    static const GTypeInfo spectrum_info = {
      sizeof (GstSpectrumClass),
      gst_spectrum_base_init,
      NULL,
      (GClassInitFunc) gst_spectrum_class_init,
      NULL,
      NULL,
      sizeof (GstSpectrum),
      0,
      (GInstanceInitFunc) gst_spectrum_init,
    };

    spectrum_type =
        g_type_register_static (GST_TYPE_ELEMENT, "GstSpectrum", &spectrum_info,
        0);
  }
  return spectrum_type;
}

static void
gst_spectrum_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&spectrum_sink_template_factory));
  gst_element_class_set_details (element_class, &gst_spectrum_details);
}

static void
gst_spectrum_class_init (GstSpectrumClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = gst_spectrum_set_property;

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_WIDTH,
      g_param_spec_int ("width", "width", "width", 1, SPECTRUM_MAX_WIDTH,
          SPECTRUM_DEFAULT_WIDTH, G_PARAM_WRITABLE));
}

static void
gst_spectrum_init (GstSpectrum * spectrum)
{
  spectrum->sinkpad = gst_pad_new ("sink", GST_PAD_SINK);
  gst_element_add_pad (GST_ELEMENT (spectrum), spectrum->sinkpad);
  gst_pad_set_link_function (spectrum->sinkpad, gst_spectrum_link);
  gst_pad_set_chain_function (spectrum->sinkpad, gst_spectrum_chain);
  spectrum->srcpad = gst_pad_new ("src", GST_PAD_SRC);
  gst_element_add_pad (GST_ELEMENT (spectrum), spectrum->srcpad);

  spectrum->width = SPECTRUM_DEFAULT_WIDTH;
}

static void
gst_spectrum_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSpectrum *spectrum = (GstSpectrum *) object;

  g_return_if_fail (GST_IS_SPECTRUM (object));

  switch (prop_id) {
    case ARG_WIDTH:
      spectrum->width = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstPadLinkReturn
gst_spectrum_link (GstPad * pad, const GstCaps * caps)
{
  GstSpectrum *spectrum = GST_SPECTRUM (gst_pad_get_parent (pad));
  GstStructure *structure;

  structure = gst_caps_get_structure (caps, 0);

  if (!gst_structure_get_int (structure, "channels", &spectrum->channels))
    return GST_PAD_LINK_REFUSED;

  return GST_PAD_LINK_OK;
}

static void
gst_spectrum_chain (GstPad * pad, GstData * _data)
{
  GstBuffer *buf = GST_BUFFER (_data);
  GstSpectrum *spectrum;
  gint spec_base, spec_len;
  gint16 *re, *im, *loud;
  gint16 *samples;
  gint samples_n;
  gint step, pos, i;
  guint8 *spect;
  GstBuffer *newbuf;

  g_return_if_fail (pad != NULL);
  g_return_if_fail (GST_IS_PAD (pad));
  g_return_if_fail (buf != NULL);

  spectrum = GST_SPECTRUM (GST_OBJECT_PARENT (pad));

  samples = (gint16 *) GST_BUFFER_DATA (buf);
  samples_n = GST_BUFFER_SIZE (buf) / (spectrum->channels * sizeof (gint16));

  spec_base = 10;               /* 2^10 = 1024 */
  spec_len = 1024;

  /* zero pad if samples_n < spec_len */
  re = g_new0 (gint16, spec_len);
  im = g_new0 (gint16, spec_len);

  loud = g_new0 (gint16, spec_len);

  if (spectrum->channels == 2) {
    for (i = 0; i < MIN (spec_len, samples_n); i++)
      re[i] = (samples[(i * 2)] + samples[(i * 2) + 1]) >> 1;
  } else {
    re = memcpy (re, samples, MIN (spec_len, samples_n) * sizeof (gint16));
  }

  /* do not apply the window to the zero padding */
  gst_spectrum_window (re, MIN (spec_len, samples_n));

  gst_spectrum_fix_fft (re, im, spec_base, FALSE);
  gst_spectrum_fix_loud (loud, re, im, spec_len, 0);

  g_free (re);
  g_free (im);

  step = spec_len / spectrum->width;
  spect = g_new (guint8, spectrum->width);
  for (i = 0, pos = 0; i < spectrum->width; i++, pos += step) {
    if (loud[pos] > -60)
      spect[i] = (loud[pos] + 60) / 2;
    else
      spect[i] = 0;
/*    if (spect[i] > 15); */
/*      spect[i] = 15; */
  }
  g_free (loud);
  gst_buffer_unref (buf);

  newbuf = gst_buffer_new ();
  GST_BUFFER_DATA (newbuf) = spect;
  GST_BUFFER_SIZE (newbuf) = spectrum->width;

  gst_pad_push (spectrum->srcpad, GST_DATA (newbuf));
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "spectrum", GST_RANK_NONE,
      GST_TYPE_SPECTRUM);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "spectrum",
    "Run an FFT on the audio signal, output spectrum data",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE, GST_ORIGIN)
