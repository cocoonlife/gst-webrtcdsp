/*
 * WebRTC Audio Processing Elements
 *
 *  Copyright 2016 Collabora Ltd
 *    @author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

/**
 * SECTION:element-webrtcdsp
 * @short_description: Audio Filter using WebRTC Audio Processing library
 *
 * A voice enhancement filter based on WebRTC Audio Processing library. This
 * library provides a whide variety of enhancement algorithms. This element
 * tries to enable as much as possible. The currently enabled enhancements are
 * High Pass Filter, Echo Canceller, Noise Suppression, Automatic Gain Control,
 * and some extended filters.
 *
 * While webrtcdsp element can be used alone, there is an exception for the
 * echo canceller. The audio canceller need to be aware of the far end streams
 * that are played to loud speakers. For this, you must place a webrtcechoprobe
 * element at that far end. Note that the sample rate must match between
 * webrtcdsp and the webrtechoprobe. Though, the number of channels can differ.
 * The probe is found by the DSP element using it's object name. By default,
 * webrtcdsp looks for webrtcechoprobe0, which means it just work if you have
 * a single probe and DSP.
 *
 * The probe can only be used within the same top level GstPipeline.
 * Additonally, to simplify the code, the probe element must be created
 * before the DSP sink pad is activated. It does not need to be in any
 * particular state and does not even need to be added to the pipeline yet.
 *
 * # Example launch line
 *
 * As a conveniance, the echo canceller can be tested using an echo loop. In
 * this configuration, one would expect a single echo to be heard.
 *
 * |[
 * gst-launch-1.0 pulsesrc ! webrtcdsp ! webrtcechoprobe ! pulsesink
 * ]|
 *
 * In real environment, you'll place the probe before the playback, but only
 * process the far end streams. The DSP should be placed as close as possible
 * to the audio capture. The following pipeline is astracted and does not
 * represent a real pipeline.
 *
 * |[
 * gst-launch-1.0 far-end-src ! audio/x-raw,rate=48000 ! webrtcechoprobe ! pulsesink \
 *                pulsesrc ! audio/x-raw,rate=48000 ! webrtcdsp ! far-end-sink
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstwebrtcdsp.h"
#include "gstwebrtcechoprobe.h"

#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
#include <webrtc/system_wrappers/include/trace.h>

GST_DEBUG_CATEGORY (webrtc_dsp_debug);
#define GST_CAT_DEFAULT (webrtc_dsp_debug)

static GstStaticPadTemplate gst_webrtc_dsp_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) { 48000, 32000, 16000, 8000 }, "
        "channels = (int) [1, MAX]")
    );

static GstStaticPadTemplate gst_webrtc_dsp_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) { 48000, 32000, 16000, 8000 }, "
        "channels = (int) [1, MAX]")
    );

typedef webrtc::EchoCancellation::SuppressionLevel GstWebrtcEchoSuppressionLevel;
#define GST_TYPE_WEBRTC_ECHO_SUPPRESSION_LEVEL \
    (gst_webrtc_echo_suppression_level_get_type ())
static GType
gst_webrtc_echo_suppression_level_get_type (void)
{
  static GType suppression_level_type = 0;
  static const GEnumValue level_types[] = {
    {webrtc::EchoCancellation::kLowSuppression, "Low Suppression", "low"},
    {webrtc::EchoCancellation::kModerateSuppression,
      "Moderate Suppression", "moderate"},
    {webrtc::EchoCancellation::kHighSuppression, "high Suppression", "high"},
    {0, NULL, NULL}
  };

  if (!suppression_level_type) {
    suppression_level_type =
        g_enum_register_static ("GstWebrtcEchoSuppressionLevel", level_types);
  }
  return suppression_level_type;
}

typedef webrtc::NoiseSuppression::Level GstWebrtcNoiseSuppressionLevel;
#define GST_TYPE_WEBRTC_NOISE_SUPPRESSION_LEVEL \
    (gst_webrtc_noise_suppression_level_get_type ())
static GType
gst_webrtc_noise_suppression_level_get_type (void)
{
  static GType suppression_level_type = 0;
  static const GEnumValue level_types[] = {
    {webrtc::NoiseSuppression::kLow, "Low Suppression", "low"},
    {webrtc::NoiseSuppression::kModerate, "Moderate Suppression", "moderate"},
    {webrtc::NoiseSuppression::kHigh, "High Suppression", "high"},
    {webrtc::NoiseSuppression::kVeryHigh, "Very High Suppression",
      "very-high"},
    {0, NULL, NULL}
  };

  if (!suppression_level_type) {
    suppression_level_type =
        g_enum_register_static ("GstWebrtcNoiseSuppressionLevel", level_types);
  }
  return suppression_level_type;
}

enum
{
  PROP_0,
  PROP_PROBE,
  PROP_HIGH_PASS_FILTER,
  PROP_ECHO_CANCEL,
  PROP_ECHO_SUPPRESSION_LEVEL,
  PROP_NOISE_SUPPRESSION,
  PROP_NOISE_SUPPRESSION_LEVEL,
  PROP_GAIN_CONTROL,
  PROP_EXPERIMENTAL_AGC,
  PROP_EXTENDED_FILTER,
  PROP_DELAY_AGNOSTIC
};

/**
 * GstWebrtcDSP:
 *
 * The adder object structure.
 */
struct _GstWebrtcDsp
{
  GstAudioFilter element;

  /* Protected by the object lock */
  GstAudioInfo info;
  guint period_size;

  /* Protected by the stream lock */
  GstAdapter *adapter;
  webrtc::AudioProcessing * apm;

  /* Protected by the object lock */
  gchar *probe_name;
  GstWebrtcEchoProbe *probe;

  /* Properties */
  gboolean high_pass_filter;
  gboolean echo_cancel;
  webrtc::EchoCancellation::SuppressionLevel echo_suppression_level;
  gboolean noise_suppression;
  webrtc::NoiseSuppression::Level noise_suppression_level;
  gboolean gain_control;
  gboolean experimental_agc;
  gboolean extended_filter;
  gboolean delay_agnostic;
};

G_DEFINE_TYPE (GstWebrtcDsp, gst_webrtc_dsp, GST_TYPE_AUDIO_FILTER);

static const gchar *
webrtc_error_to_string (gint err)
{
  const gchar *str = "unkown error";

  switch (err) {
    case webrtc::AudioProcessing::kNoError:
      str = "success";
      break;
    case webrtc::AudioProcessing::kUnspecifiedError:
      str = "unspecified error";
      break;
    case webrtc::AudioProcessing::kCreationFailedError:
      str = "creating failed";
      break;
    case webrtc::AudioProcessing::kUnsupportedComponentError:
      str = "unsupported component";
      break;
    case webrtc::AudioProcessing::kUnsupportedFunctionError:
      str = "unsupported function";
      break;
    case webrtc::AudioProcessing::kNullPointerError:
      str = "null pointer";
      break;
    case webrtc::AudioProcessing::kBadParameterError:
      str = "bad parameter";
      break;
    case webrtc::AudioProcessing::kBadSampleRateError:
      str = "bad sample rate";
      break;
    case webrtc::AudioProcessing::kBadDataLengthError:
      str = "bad data length";
      break;
    case webrtc::AudioProcessing::kBadNumberChannelsError:
      str = "bad number of channels";
      break;
    case webrtc::AudioProcessing::kFileError:
      str = "file IO error";
      break;
    case webrtc::AudioProcessing::kStreamParameterNotSetError:
      str = "stream parameter not set";
      break;
    case webrtc::AudioProcessing::kNotEnabledError:
      str = "not enabled";
      break;
    default:
      break;
  }

  return str;
}

static GstBuffer *
gst_webrtc_dsp_take_buffer (GstWebrtcDsp * self)
{
  GstBuffer *buffer;
  GstClockTime timestamp;
  guint64 distance;

  timestamp = gst_adapter_prev_pts (self->adapter, &distance);
  timestamp += gst_util_uint64_scale_int (distance / self->info.bpf,
      GST_SECOND, self->info.rate);

  buffer = gst_adapter_take_buffer (self->adapter, self->period_size);

  GST_BUFFER_PTS (buffer) = timestamp;
  GST_BUFFER_DURATION (buffer) = 10 * GST_MSECOND;

  if (0 && distance == 0) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
  } else
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_DISCONT);

  return buffer;
}

static GstFlowReturn
gst_webrtc_dsp_analyze_reverse_stream (GstWebrtcDsp * self,
    GstClockTime rec_time)
{
  GstWebrtcEchoProbe *probe = NULL;
  webrtc::AudioProcessing * apm;
  webrtc::AudioFrame frame;
  GstFlowReturn ret = GST_FLOW_OK;
  gint err, delay;

  GST_OBJECT_LOCK (self);
  if (self->echo_cancel)
    probe = GST_WEBRTC_ECHO_PROBE (g_object_ref (self->probe));
  GST_OBJECT_UNLOCK (self);

  /* If echo cancellation is disabled */
  if (!probe)
    return GST_FLOW_OK;

  apm = self->apm;

  if (self->delay_agnostic)
    rec_time = GST_CLOCK_TIME_NONE;

again:
  delay = gst_webrtc_echo_probe_read (probe, rec_time, (gpointer) &frame);
  apm->set_stream_delay_ms (delay);

  if (delay < 0)
    goto done;

  if (frame.sample_rate_hz_ != self->info.rate) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT,
        ("Echo Probe has rate %i , while the DSP is running at rate %i,"
         " use a caps filter to ensure those are the same.",
         frame.sample_rate_hz_, self->info.rate), (NULL));
    ret = GST_FLOW_ERROR;
    goto done;
  }

  if ((err = apm->AnalyzeReverseStream (&frame)) < 0)
    GST_WARNING_OBJECT (self, "Reverse stream analyses failed: %s.",
        webrtc_error_to_string (err));

  if (self->delay_agnostic)
      goto again;

done:
  gst_object_unref (probe);

  return ret;
}

static GstFlowReturn
gst_webrtc_dsp_process_stream (GstWebrtcDsp * self,
    GstBuffer * buffer)
{
  GstMapInfo info;
  webrtc::AudioProcessing * apm = self->apm;
  webrtc::AudioFrame frame;
  gint err;

  frame.num_channels_ = self->info.channels;
  frame.sample_rate_hz_ = self->info.rate;
  frame.samples_per_channel_ = self->period_size / self->info.bpf;

  if (!gst_buffer_map (buffer, &info, (GstMapFlags) GST_MAP_READWRITE)) {
    gst_buffer_unref (buffer);
    return GST_FLOW_ERROR;
  }

  memcpy (frame.data_, info.data, self->period_size);

  if ((err = apm->ProcessStream (&frame)) < 0) {
    GST_WARNING_OBJECT (self, "Failed to filter the audio: %s.",
        webrtc_error_to_string (err));
  } else {
    memcpy (info.data, frame.data_, self->period_size);
  }

  gst_buffer_unmap (buffer, &info);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_webrtc_dsp_submit_input_buffer (GstBaseTransform * btrans,
    gboolean is_discont, GstBuffer * buffer)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (btrans);

  buffer = gst_buffer_make_writable (buffer);
  GST_BUFFER_PTS (buffer) = gst_segment_to_running_time (&btrans->segment,
      GST_FORMAT_TIME, GST_BUFFER_PTS (buffer));

  if (is_discont) {
    GST_DEBUG_OBJECT (self,
        "Received discont, clearing adapter.");
    gst_adapter_clear (self->adapter);
  }

  gst_adapter_push (self->adapter, buffer);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_webrtc_dsp_generate_output (GstBaseTransform * btrans, GstBuffer ** outbuf)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (btrans);
  GstFlowReturn ret;

  if (gst_adapter_available (self->adapter) < self->period_size) {
    *outbuf = NULL;
    return GST_FLOW_OK;
  }

  *outbuf = gst_webrtc_dsp_take_buffer (self);
  ret = gst_webrtc_dsp_analyze_reverse_stream (self, GST_BUFFER_PTS (*outbuf));

  if (ret == GST_FLOW_OK)
    ret = gst_webrtc_dsp_process_stream (self, *outbuf);

  return ret;
}

static gboolean
gst_webrtc_dsp_start (GstBaseTransform * btrans)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (btrans);
  webrtc::Config config;

  GST_OBJECT_LOCK (self);

  config.Set < webrtc::ExtendedFilter >
      (new webrtc::ExtendedFilter (self->extended_filter));
  config.Set < webrtc::ExperimentalAgc >
      (new webrtc::ExperimentalAgc (self->experimental_agc));
  config.Set < webrtc::DelayAgnostic >
      (new webrtc::DelayAgnostic (self->delay_agnostic));

  /* TODO Intelligibility enhancer, Beamforming, etc. */

  self->apm = webrtc::AudioProcessing::Create (config);

  if (self->echo_cancel) {
    self->probe = gst_webrtc_acquire_echo_probe (self->probe_name);

    if (self->probe == NULL) {
      GST_OBJECT_UNLOCK (self);
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
          ("No echo probe with name %s found.", self->probe_name), (NULL));
      return FALSE;
    }
  }

  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_webrtc_dsp_setup (GstAudioFilter * filter, const GstAudioInfo * info)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (filter);
  webrtc::AudioProcessing * apm;
  webrtc::ProcessingConfig pconfig;
  GstAudioInfo probe_info = *info;
  gint err = 0;

  GST_LOG_OBJECT (self, "setting format to %s with %i Hz and %i channels",
      info->finfo->description, info->rate, info->channels);

  GST_OBJECT_LOCK (self);

  gst_adapter_clear (self->adapter);
  self->info = *info;
  apm = self->apm;

  /* WebRTC library works with 10ms buffers, compute once this size */
  self->period_size = info->bpf * info->rate / 100;

  if ((webrtc::AudioFrame::kMaxDataSizeSamples * 2) < self->period_size)
    goto period_too_big;

  if (self->probe) {
    GST_WEBRTC_ECHO_PROBE_LOCK (self->probe);

    if (self->probe->info.rate != 0) {
      if (self->probe->info.rate != info->rate)
        goto probe_has_wrong_rate;
      probe_info = self->probe->info;
    }

    GST_WEBRTC_ECHO_PROBE_UNLOCK (self->probe);
  }

  /* input stream */
  pconfig.streams[webrtc::ProcessingConfig::kInputStream] =
      webrtc::StreamConfig (info->rate, info->channels, false);
  /* output stream */
  pconfig.streams[webrtc::ProcessingConfig::kOutputStream] =
      webrtc::StreamConfig (info->rate, info->channels, false);
  /* reverse input stream */
  pconfig.streams[webrtc::ProcessingConfig::kReverseInputStream] =
      webrtc::StreamConfig (probe_info.rate, probe_info.channels, false);
  /* reverse output stream */
  pconfig.streams[webrtc::ProcessingConfig::kReverseOutputStream] =
      webrtc::StreamConfig (probe_info.rate, probe_info.channels, false);

  if ((err = apm->Initialize (pconfig)) < 0)
    goto initialize_failed;

  /* Setup Filters */
  if (self->high_pass_filter) {
    GST_DEBUG_OBJECT (self, "Enabling High Pass filter");
    apm->high_pass_filter ()->Enable (true);
  }

  if (self->echo_cancel) {
    GST_DEBUG_OBJECT (self, "Enabling Echo Cancellation");
    apm->echo_cancellation ()->enable_drift_compensation (false);
    apm->echo_cancellation ()
        ->set_suppression_level (self->echo_suppression_level);
    apm->echo_cancellation ()->Enable (true);
  }

  if (self->noise_suppression) {
    GST_DEBUG_OBJECT (self, "Enabling Noise Suppression");
    apm->noise_suppression ()->set_level (self->noise_suppression_level);
    apm->noise_suppression ()->Enable (true);
  }

  if (self->gain_control) {
    GST_DEBUG_OBJECT (self, "Enabling Digital Gain Control");
    apm->gain_control ()->set_mode (webrtc::GainControl::kAdaptiveDigital);
    apm->gain_control ()->Enable (true);
  }

  GST_OBJECT_UNLOCK (self);

  return TRUE;

period_too_big:
  GST_OBJECT_UNLOCK (self);
  GST_WARNING_OBJECT (self, "webrtcdsp format produce too big period "
      "(maximum is %" G_GSIZE_FORMAT " samples and we have %u samples), "
      "reduce the number of channels or the rate.",
      webrtc::AudioFrame::kMaxDataSizeSamples, self->period_size / 2);
  return FALSE;

probe_has_wrong_rate:
  GST_WEBRTC_ECHO_PROBE_UNLOCK (self->probe);
  GST_OBJECT_UNLOCK (self);
  GST_ELEMENT_ERROR (self, STREAM, FORMAT,
      ("Echo Probe has rate %i , while the DSP is running at rate %i,"
          " use a caps filter to ensure those are the same.",
          probe_info.rate, info->rate), (NULL));
  return FALSE;

initialize_failed:
  GST_OBJECT_UNLOCK (self);
  GST_ELEMENT_ERROR (self, LIBRARY, INIT,
      ("Failed to initialize WebRTC Audio Processing library"),
      ("webrtc::AudioProcessing::Initialize() failed: %s",
          webrtc_error_to_string (err)));
  return FALSE;
}

static gboolean
gst_webrtc_dsp_stop (GstBaseTransform * btrans)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (btrans);

  GST_OBJECT_LOCK (self);

  gst_adapter_clear (self->adapter);

  if (self->probe) {
    gst_webrtc_release_echo_probe (self->probe);
    self->probe = NULL;
  }

  delete self->apm;
  self->apm = NULL;

  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static void
gst_webrtc_dsp_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_PROBE:
      g_free (self->probe_name);
      self->probe_name = g_value_dup_string (value);
      break;
    case PROP_HIGH_PASS_FILTER:
      self->high_pass_filter = g_value_get_boolean (value);
      break;
    case PROP_ECHO_CANCEL:
      self->echo_cancel = g_value_get_boolean (value);
      break;
    case PROP_ECHO_SUPPRESSION_LEVEL:
      self->echo_suppression_level =
          (GstWebrtcEchoSuppressionLevel) g_value_get_enum (value);
      break;
    case PROP_NOISE_SUPPRESSION:
      self->noise_suppression = g_value_get_boolean (value);
      break;
    case PROP_NOISE_SUPPRESSION_LEVEL:
      self->noise_suppression_level =
          (GstWebrtcNoiseSuppressionLevel) g_value_get_enum (value);
      break;
    case PROP_GAIN_CONTROL:
      self->gain_control = g_value_get_boolean (value);
      break;
    case PROP_EXPERIMENTAL_AGC:
      self->experimental_agc = g_value_get_boolean (value);
      break;
    case PROP_EXTENDED_FILTER:
      self->extended_filter = g_value_get_boolean (value);
      break;
    case PROP_DELAY_AGNOSTIC:
      self->delay_agnostic = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_webrtc_dsp_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_PROBE:
      g_value_set_string (value, self->probe_name);
      break;
    case PROP_HIGH_PASS_FILTER:
      g_value_set_boolean (value, self->high_pass_filter);
      break;
    case PROP_ECHO_CANCEL:
      g_value_set_boolean (value, self->echo_cancel);
      break;
    case PROP_ECHO_SUPPRESSION_LEVEL:
      g_value_set_enum (value, self->echo_suppression_level);
      break;
    case PROP_NOISE_SUPPRESSION:
      g_value_set_boolean (value, self->noise_suppression);
      break;
    case PROP_NOISE_SUPPRESSION_LEVEL:
      g_value_set_enum (value, self->noise_suppression_level);
      break;
    case PROP_GAIN_CONTROL:
      g_value_set_boolean (value, self->gain_control);
      break;
    case PROP_EXPERIMENTAL_AGC:
      g_value_set_boolean (value, self->experimental_agc);
      break;
    case PROP_EXTENDED_FILTER:
      g_value_set_boolean (value, self->extended_filter);
      break;
    case PROP_DELAY_AGNOSTIC:
      g_value_set_boolean (value, self->delay_agnostic);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}


static void
gst_webrtc_dsp_finalize (GObject * object)
{
  GstWebrtcDsp *self = GST_WEBRTC_DSP (object);

  gst_object_unref (self->adapter);
  g_free (self->probe_name);

  G_OBJECT_CLASS (gst_webrtc_dsp_parent_class)->finalize (object);
}

static void
gst_webrtc_dsp_init (GstWebrtcDsp * self)
{
  self->adapter = gst_adapter_new ();
  gst_audio_info_init (&self->info);
}

static void
gst_webrtc_dsp_class_init (GstWebrtcDspClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *btrans_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstAudioFilterClass *audiofilter_class = GST_AUDIO_FILTER_CLASS (klass);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_webrtc_dsp_finalize);
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_webrtc_dsp_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_webrtc_dsp_get_property);

  btrans_class->passthrough_on_same_caps = FALSE;
  btrans_class->start = GST_DEBUG_FUNCPTR (gst_webrtc_dsp_start);
  btrans_class->stop = GST_DEBUG_FUNCPTR (gst_webrtc_dsp_stop);
  btrans_class->submit_input_buffer =
      GST_DEBUG_FUNCPTR (gst_webrtc_dsp_submit_input_buffer);
  btrans_class->generate_output =
      GST_DEBUG_FUNCPTR (gst_webrtc_dsp_generate_output);

  audiofilter_class->setup = GST_DEBUG_FUNCPTR (gst_webrtc_dsp_setup);

  gst_element_class_add_static_pad_template (element_class,
      &gst_webrtc_dsp_src_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_webrtc_dsp_sink_template);
  gst_element_class_set_static_metadata (element_class,
      "Voice Processor (AGC, AEC, filters, etc.)",
      "Generic/Audio",
      "Pre-processes voice with WebRTC Audio Processing Library",
      "Nicolas Dufresne <nicolas.dufresne@collabora.com>");

  g_object_class_install_property (gobject_class,
      PROP_PROBE,
      g_param_spec_string ("probe", "Echo Probe",
          "The name of the webrtcechoprobe element that record the audio being "
          "played through loud speakers. Must be set before PAUSED state.",
          "webrtcechoprobe0",
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_HIGH_PASS_FILTER,
      g_param_spec_boolean ("high-pass-filter", "High Pass Filter",
          "Enable or disable high pass filtering", TRUE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_ECHO_CANCEL,
      g_param_spec_boolean ("echo-cancel", "Echo Cancel",
          "Enable or disable echo canceller, note that it will be disabled if "
          "no webrtcechoprobe has been found", TRUE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_ECHO_SUPPRESSION_LEVEL,
      g_param_spec_enum ("echo-suppression-level", "Echo Suppression Level",
          "Controls the aggressiveness of the suppressor. A higher level "
          "trades off double-talk performance for increased echo suppression.",
          GST_TYPE_WEBRTC_ECHO_SUPPRESSION_LEVEL,
          webrtc::EchoCancellation::kModerateSuppression,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_NOISE_SUPPRESSION,
      g_param_spec_boolean ("noise-suppression", "Noise Suppression",
          "Enable or disable noise suppression", TRUE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_NOISE_SUPPRESSION_LEVEL,
      g_param_spec_enum ("noise-suppression-level", "Noise Suppression Level",
          "Controls the aggressiveness of the suppression. Increasing the "
          "level will reduce the noise level at the expense of a higher "
          "speech distortion.", GST_TYPE_WEBRTC_NOISE_SUPPRESSION_LEVEL,
          webrtc::EchoCancellation::kModerateSuppression,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_GAIN_CONTROL,
      g_param_spec_boolean ("gain-control", "Gain Control",
          "Enable or disable automatic digital gain control",
          TRUE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_EXPERIMENTAL_AGC,
      g_param_spec_boolean ("experimental-agc", "Experimental AGC",
          "Enable or disable experimental automatic gain control.",
          FALSE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_EXTENDED_FILTER,
      g_param_spec_boolean ("extended-filter", "Extended Filter",
          "Enable or disable the extended filter.",
          TRUE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_DELAY_AGNOSTIC,
      g_param_spec_boolean ("delay-agnostic", "Delay Agnostic",
          "Enable or disable the delay agnostic mode.",
          FALSE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT
      (webrtc_dsp_debug, "webrtcdsp", 0, "libwebrtcdsp wrapping elements");

  if (!gst_element_register (plugin, "webrtcdsp", GST_RANK_NONE,
          GST_TYPE_WEBRTC_DSP)) {
    return FALSE;
  }
  if (!gst_element_register (plugin, "webrtcechoprobe", GST_RANK_NONE,
          GST_TYPE_WEBRTC_ECHO_PROBE)) {
    return FALSE;
  }

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    webrtcdsp,
    "Voice pre-processing using WebRTC Audio Processing Library",
    plugin_init, VERSION, "LGPL", "WebRTCDsp", "http://git.collabora.com")
