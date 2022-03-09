/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *               2000,2005 Wim Taymans <wim@fluendo.com>
 *
 * gstbasesrc.c:
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
 * SECTION:gstbasesrc
 * @title: GstBaseIdleSrc
 * @short_description: Base class for getrange based source elements
 * @see_also: #GstPushSrc, #GstBaseTransform, #GstBaseSink
 *
 
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <gst/gst_private.h>
#include <gst/glib-compat-private.h>

#include "gstbasesrc.h"
#include <gst/gst-i18n-lib.h>

GST_DEBUG_CATEGORY_STATIC (gst_base_idle_src_debug);
#define GST_CAT_DEFAULT gst_base_idle_src_debug

#define GST_LIVE_GET_LOCK(elem)               (&GST_BASE_IDLE_SRC_CAST(elem)->live_lock)
#define GST_LIVE_LOCK(elem)                   g_mutex_lock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_TRYLOCK(elem)                g_mutex_trylock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_UNLOCK(elem)                 g_mutex_unlock(GST_LIVE_GET_LOCK(elem))
#define GST_LIVE_GET_COND(elem)               (&GST_BASE_IDLE_SRC_CAST(elem)->live_cond)
#define GST_LIVE_WAIT(elem)                   g_cond_wait (GST_LIVE_GET_COND (elem), GST_LIVE_GET_LOCK (elem))
#define GST_LIVE_WAIT_UNTIL(elem, end_time)   g_cond_timed_wait (GST_LIVE_GET_COND (elem), GST_LIVE_GET_LOCK (elem), end_time)
#define GST_LIVE_SIGNAL(elem)                 g_cond_signal (GST_LIVE_GET_COND (elem));
#define GST_LIVE_BROADCAST(elem)              g_cond_broadcast (GST_LIVE_GET_COND (elem));


#define GST_ASYNC_GET_COND(elem)              (&GST_BASE_IDLE_SRC_CAST(elem)->priv->async_cond)
#define GST_ASYNC_WAIT(elem)                  g_cond_wait (GST_ASYNC_GET_COND (elem), GST_OBJECT_GET_LOCK (elem))
#define GST_ASYNC_SIGNAL(elem)                g_cond_signal (GST_ASYNC_GET_COND (elem));

#define CLEAR_PENDING_EOS(bsrc) \
  G_STMT_START { \
    g_atomic_int_set (&bsrc->priv->has_pending_eos, FALSE); \
    gst_event_replace (&bsrc->priv->pending_eos, NULL); \
  } G_STMT_END


/* BaseIdleSrc signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

#define DEFAULT_DO_TIMESTAMP    FALSE

enum
{
  PROP_0,
  PROP_DO_TIMESTAMP
};

/* The basesrc implementation need to respect the following locking order:
 *   1. STREAM_LOCK
 *   2. LIVE_LOCK
 *   3. OBJECT_LOCK
 */
struct _GstBaseIdleSrcPrivate
{
  gboolean discont;             /* STREAM_LOCK */
  gboolean flushing;            /* LIVE_LOCK */

  GstFlowReturn start_result;   /* OBJECT_LOCK */
  gboolean async;               /* OBJECT_LOCK */

  /* if a stream-start event should be sent */
  gboolean stream_start_pending;        /* STREAM_LOCK */

  /* if segment should be sent and a
   * seqnum if it was originated by a seek */
  gboolean segment_pending;     /* OBJECT_LOCK */
  guint32 segment_seqnum;       /* OBJECT_LOCK */

  /* if EOS is pending (atomic) */
  GstEvent *pending_eos;        /* OBJECT_LOCK */
  gint has_pending_eos;         /* atomic */

  /* if the eos was caused by a forced eos from the application */
  gboolean forced_eos;          /* LIVE_LOCK */

  /* startup latency is the time it takes between going to PLAYING and producing
   * the first BUFFER with running_time 0. This value is included in the latency
   * reporting. */
  GstClockTime latency;         /* OBJECT_LOCK */
  /* timestamp offset, this is the offset add to the values of gst_times for
   * pseudo live sources */
  GstClockTimeDiff ts_offset;   /* OBJECT_LOCK */

  gboolean do_timestamp;        /* OBJECT_LOCK */
  gint dynamic_size;            /* atomic */
  gint automatic_eos;           /* atomic */

  /* stream sequence number */
  guint32 seqnum;               /* STREAM_LOCK */

  /* pending events (TAG, CUSTOM_BOTH, CUSTOM_DOWNSTREAM) to be
   * pushed in the data stream */
  GList *pending_events;        /* OBJECT_LOCK */
  gint have_events;             /* OBJECT_LOCK */

  /* QoS *//* with LOCK */
  gdouble proportion;           /* OBJECT_LOCK */
  GstClockTime earliest_time;   /* OBJECT_LOCK */

  GstBufferPool *pool;          /* OBJECT_LOCK */
  GstAllocator *allocator;      /* OBJECT_LOCK */
  GstAllocationParams params;   /* OBJECT_LOCK */

  GCond async_cond;             /* OBJECT_LOCK */

  /* for _submit_buffer_list() */
  GstBufferList *pending_bufferlist;
};

#define BASE_IDLE_SRC_HAS_PENDING_BUFFER_LIST(src) \
    ((src)->priv->pending_bufferlist != NULL)

static GstElementClass *parent_class = NULL;
static gint private_offset = 0;

static void gst_base_idle_src_class_init (GstBaseIdleSrcClass * klass);
static void gst_base_idle_src_init (GstBaseIdleSrc * src, gpointer g_class);
static void gst_base_idle_src_finalize (GObject * object);


GType
gst_base_idle_src_get_type (void)
{
  static gsize base_idle_src_type = 0;

  if (g_once_init_enter (&base_idle_src_type)) {
    GType _type;
    static const GTypeInfo base_idle_src_info = {
      sizeof (GstBaseIdleSrcClass),
      NULL,
      NULL,
      (GClassInitFunc) gst_base_idle_src_class_init,
      NULL,
      NULL,
      sizeof (GstBaseIdleSrc),
      0,
      (GInstanceInitFunc) gst_base_idle_src_init,
    };

    _type = g_type_register_static (GST_TYPE_ELEMENT,
        "GstBaseIdleSrc", &base_idle_src_info, G_TYPE_FLAG_ABSTRACT);

    private_offset =
        g_type_add_instance_private (_type, sizeof (GstBaseIdleSrcPrivate));

    g_once_init_leave (&base_idle_src_type, _type);
  }
  return base_idle_src_type;
}

static inline GstBaseIdleSrcPrivate *
gst_base_idle_src_get_instance_private (GstBaseIdleSrc * self)
{
  return (G_STRUCT_MEMBER_P (self, private_offset));
}

static GstCaps *gst_base_idle_src_default_get_caps (GstBaseIdleSrc * bsrc,
    GstCaps * filter);
static GstCaps *gst_base_idle_src_default_fixate (GstBaseIdleSrc * src,
    GstCaps * caps);
static GstCaps *gst_base_idle_src_fixate (GstBaseIdleSrc * src, GstCaps * caps);


static gboolean gst_base_idle_src_activate_mode (GstPad * pad,
    GstObject * parent, GstPadMode mode, gboolean active);
static void gst_base_idle_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_base_idle_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_base_idle_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_base_idle_src_send_event (GstElement * elem,
    GstEvent * event);
static gboolean gst_base_idle_src_default_event (GstBaseIdleSrc * src,
    GstEvent * event);

static gboolean gst_base_idle_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static void gst_base_idle_src_set_pool_flushing (GstBaseIdleSrc * basesrc,
    gboolean flushing);
static gboolean gst_base_idle_src_default_negotiate (GstBaseIdleSrc * basesrc);

static gboolean gst_base_idle_src_default_query (GstBaseIdleSrc * src,
    GstQuery * query);
static gboolean gst_base_idle_src_default_prepare_seek_segment (GstBaseIdleSrc *
    src, GstEvent * event, GstSegment * segment);
static GstFlowReturn gst_base_idle_src_default_create (GstBaseIdleSrc * basesrc,
    guint64 offset, guint size, GstBuffer ** buf);
static GstFlowReturn gst_base_idle_src_default_alloc (GstBaseIdleSrc * basesrc,
    guint64 offset, guint size, GstBuffer ** buf);
static gboolean gst_base_idle_src_decide_allocation_default (GstBaseIdleSrc *
    basesrc, GstQuery * query);

static gboolean gst_base_idle_src_set_flushing (GstBaseIdleSrc * basesrc,
    gboolean flushing);

static gboolean gst_base_idle_src_start (GstBaseIdleSrc * basesrc);
static gboolean gst_base_idle_src_stop (GstBaseIdleSrc * basesrc);

static GstStateChangeReturn gst_base_idle_src_change_state (GstElement *
    element, GstStateChange transition);

static void gst_base_idle_src_loop (GstPad * pad);

static GstFlowReturn gst_base_idle_src_get_range (GstBaseIdleSrc * src,
    guint64 offset, guint length, GstBuffer ** buf);
static gboolean gst_base_idle_src_seekable (GstBaseIdleSrc * src);
static gboolean gst_base_idle_src_negotiate_unlocked (GstBaseIdleSrc * basesrc);
static gboolean gst_base_idle_src_update_length (GstBaseIdleSrc * src,
    guint64 offset, guint * length, gboolean force);

/**
 * gst_base_idle_src_set_format:
 * @src: base source instance
 * @format: the format to use
 *
 * Sets the default format of the source. This will be the format used
 * for sending SEGMENT events and for performing seeks.
 *
 * If a format of GST_FORMAT_BYTES is set, the element will be able to
 * operate in pull mode if the #GstBaseIdleSrcClass::is_seekable returns %TRUE.
 *
 * This function must only be called in states < %GST_STATE_PAUSED.
 */
void
gst_base_idle_src_set_format (GstBaseIdleSrc * src, GstFormat format)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));
  g_return_if_fail (GST_STATE (src) <= GST_STATE_READY);

  GST_OBJECT_LOCK (src);
  gst_segment_init (&src->segment, format);
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_idle_src_query_latency:
 * @src: the source
 * @live: (out) (allow-none): if the source is live
 * @min_latency: (out) (allow-none): the min latency of the source
 * @max_latency: (out) (allow-none): the max latency of the source
 *
 * Query the source for the latency parameters. @live will be %TRUE when @src is
 * configured as a live source. @min_latency and @max_latency will be set
 * to the difference between the running time and the timestamp of the first
 * buffer.
 *
 * This function is mostly used by subclasses.
 *
 * Returns: %TRUE if the query succeeded.
 */
gboolean
gst_base_idle_src_query_latency (GstBaseIdleSrc * src, gboolean * live,
    GstClockTime * min_latency, GstClockTime * max_latency)
{
  GstClockTime min;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);

  GST_OBJECT_LOCK (src);
  if (live)
    *live = src->is_live;

  /* if we have a startup latency, report this one, else report 0. Subclasses
   * are supposed to override the query function if they want something
   * else. */
  if (src->priv->latency != -1)
    min = src->priv->latency;
  else
    min = 0;

  if (min_latency)
    *min_latency = min;
  if (max_latency)
    *max_latency = min;

  GST_LOG_OBJECT (src, "latency: live %d, min %" GST_TIME_FORMAT
      ", max %" GST_TIME_FORMAT, src->is_live, GST_TIME_ARGS (min),
      GST_TIME_ARGS (min));
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

/**
 * gst_base_idle_src_set_do_timestamp:
 * @src: the source
 * @timestamp: enable or disable timestamping
 *
 * Configure @src to automatically timestamp outgoing buffers based on the
 * current running_time of the pipeline. This property is mostly useful for live
 * sources.
 */
void
gst_base_idle_src_set_do_timestamp (GstBaseIdleSrc * src, gboolean timestamp)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));

  GST_OBJECT_LOCK (src);
  src->priv->do_timestamp = timestamp;
  if (timestamp && src->segment.format != GST_FORMAT_TIME)
    gst_segment_init (&src->segment, GST_FORMAT_TIME);
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_idle_src_get_do_timestamp:
 * @src: the source
 *
 * Query if @src timestamps outgoing buffers based on the current running_time.
 *
 * Returns: %TRUE if the base class will automatically timestamp outgoing buffers.
 */
gboolean
gst_base_idle_src_get_do_timestamp (GstBaseIdleSrc * src)
{
  gboolean res;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);

  GST_OBJECT_LOCK (src);
  res = src->priv->do_timestamp;
  GST_OBJECT_UNLOCK (src);

  return res;
}

/**
 * gst_base_idle_src_new_seamless_segment:
 * @src: The source
 * @start: The new start value for the segment
 * @stop: Stop value for the new segment
 * @time: The new time value for the start of the new segment
 *
 * Prepare a new seamless segment for emission downstream. This function must
 * only be called by derived sub-classes, and only from the #GstBaseIdleSrcClass::create function,
 * as the stream-lock needs to be held.
 *
 * The format for the new segment will be the current format of the source, as
 * configured with gst_base_idle_src_set_format()
 *
 * Returns: %TRUE if preparation of the seamless segment succeeded.
 *
 * Deprecated: 1.18: Use gst_base_idle_src_new_segment()
 */
gboolean
gst_base_idle_src_new_seamless_segment (GstBaseIdleSrc * src, gint64 start,
    gint64 stop, gint64 time)
{
  gboolean res = TRUE;

  GST_OBJECT_LOCK (src);

  src->segment.base = gst_segment_to_running_time (&src->segment,
      src->segment.format, src->segment.position);
  src->segment.position = src->segment.start = start;
  src->segment.stop = stop;
  src->segment.time = time;

  /* Mark pending segment. Will be sent before next data */
  src->priv->segment_pending = TRUE;
  src->priv->segment_seqnum = gst_util_seqnum_next ();

  GST_DEBUG_OBJECT (src,
      "Starting new seamless segment. Start %" GST_TIME_FORMAT " stop %"
      GST_TIME_FORMAT " time %" GST_TIME_FORMAT " base %" GST_TIME_FORMAT,
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop), GST_TIME_ARGS (time),
      GST_TIME_ARGS (src->segment.base));

  GST_OBJECT_UNLOCK (src);

  src->priv->discont = TRUE;
  src->running = TRUE;

  return res;
}

/**
 * gst_base_idle_src_new_segment:
 * @src: a #GstBaseIdleSrc
 * @segment: a pointer to a #GstSegment
 *
 * Prepare a new segment for emission downstream. This function must
 * only be called by derived sub-classes, and only from the #GstBaseIdleSrcClass::create function,
 * as the stream-lock needs to be held.
 *
 * The format for the @segment must be identical with the current format
 * of the source, as configured with gst_base_idle_src_set_format().
 *
 * The format of @src must not be %GST_FORMAT_UNDEFINED and the format
 * should be configured via gst_base_idle_src_set_format() before calling this method.
 *
 * Returns: %TRUE if preparation of new segment succeeded.
 *
 * Since: 1.18
 */
gboolean
gst_base_idle_src_new_segment (GstBaseIdleSrc * src, const GstSegment * segment)
{
  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);
  g_return_val_if_fail (segment != NULL, FALSE);

  GST_OBJECT_LOCK (src);

  if (src->segment.format == GST_FORMAT_UNDEFINED) {
    /* subclass must set valid format before calling this method */
    GST_WARNING_OBJECT (src, "segment format is not configured yet, ignore");
    GST_OBJECT_UNLOCK (src);
    return FALSE;
  }

  if (src->segment.format != segment->format) {
    GST_WARNING_OBJECT (src, "segment format mismatched, ignore");
    GST_OBJECT_UNLOCK (src);
    return FALSE;
  }

  gst_segment_copy_into (segment, &src->segment);

  /* Mark pending segment. Will be sent before next data */
  src->priv->segment_pending = TRUE;
  src->priv->segment_seqnum = gst_util_seqnum_next ();

  GST_DEBUG_OBJECT (src, "Starting new segment %" GST_SEGMENT_FORMAT, segment);

  GST_OBJECT_UNLOCK (src);

  src->running = TRUE;

  return TRUE;
}

/* called with STREAM_LOCK */
static gboolean
gst_base_idle_src_send_stream_start (GstBaseIdleSrc * src)
{
  gboolean ret = TRUE;

  if (src->priv->stream_start_pending) {
    gchar *stream_id;
    GstEvent *event;

    stream_id =
        gst_pad_create_stream_id (src->srcpad, GST_ELEMENT_CAST (src), NULL);

    GST_DEBUG_OBJECT (src, "Pushing STREAM_START");
    event = gst_event_new_stream_start (stream_id);
    gst_event_set_group_id (event, gst_util_group_id_next ());

    ret = gst_pad_push_event (src->srcpad, event);
    src->priv->stream_start_pending = FALSE;
    g_free (stream_id);
  }

  return ret;
}

/**
 * gst_base_idle_src_set_caps:
 * @src: a #GstBaseIdleSrc
 * @caps: (transfer none): a #GstCaps
 *
 * Set new caps on the basesrc source pad.
 *
 * Returns: %TRUE if the caps could be set
 */
gboolean
gst_base_idle_src_set_caps (GstBaseIdleSrc * src, GstCaps * caps)
{
  GstBaseIdleSrcClass *bclass;
  gboolean res = TRUE;
  GstCaps *current_caps;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  gst_base_idle_src_send_stream_start (src);

  current_caps = gst_pad_get_current_caps (GST_BASE_IDLE_SRC_PAD (src));
  if (current_caps && gst_caps_is_equal (current_caps, caps)) {
    GST_DEBUG_OBJECT (src, "New caps equal to old ones: %" GST_PTR_FORMAT,
        caps);
    res = TRUE;
  } else {
    if (bclass->set_caps)
      res = bclass->set_caps (src, caps);

    if (res)
      res = gst_pad_push_event (src->srcpad, gst_event_new_caps (caps));
  }

  if (current_caps)
    gst_caps_unref (current_caps);

  return res;
}

static GstCaps *
gst_base_idle_src_default_get_caps (GstBaseIdleSrc * bsrc, GstCaps * filter)
{
  GstCaps *caps = NULL;
  GstPadTemplate *pad_template;
  GstBaseIdleSrcClass *bclass;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (bsrc);

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (bclass), "src");

  if (pad_template != NULL) {
    caps = gst_pad_template_get_caps (pad_template);

    if (filter) {
      GstCaps *intersection;

      intersection =
          gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
      caps = intersection;
    }
  }
  return caps;
}

static GstCaps *
gst_base_idle_src_default_fixate (GstBaseIdleSrc * bsrc, GstCaps * caps)
{
  GST_DEBUG_OBJECT (bsrc, "using default caps fixate function");
  return gst_caps_fixate (caps);
}

static GstCaps *
gst_base_idle_src_fixate (GstBaseIdleSrc * bsrc, GstCaps * caps)
{
  GstBaseIdleSrcClass *bclass;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (bsrc);

  if (bclass->fixate)
    caps = bclass->fixate (bsrc, caps);

  return caps;
}

static gboolean
gst_base_idle_src_default_query (GstBaseIdleSrc * src, GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat format;

      gst_query_parse_position (query, &format, NULL);

      GST_DEBUG_OBJECT (src, "position query in format %s",
          gst_format_get_name (format));

      switch (format) {
        case GST_FORMAT_PERCENT:
        {
          gint64 percent;
          gint64 position;
          gint64 duration;

          GST_OBJECT_LOCK (src);
          position = src->segment.position;
          duration = src->segment.duration;
          GST_OBJECT_UNLOCK (src);

          if (position != -1 && duration != -1) {
            if (position < duration)
              percent = gst_util_uint64_scale (GST_FORMAT_PERCENT_MAX, position,
                  duration);
            else
              percent = GST_FORMAT_PERCENT_MAX;
          } else
            percent = -1;

          gst_query_set_position (query, GST_FORMAT_PERCENT, percent);
          res = TRUE;
          break;
        }
        default:
        {
          gint64 position;
          GstFormat seg_format;

          GST_OBJECT_LOCK (src);
          position =
              gst_segment_to_stream_time (&src->segment, src->segment.format,
              src->segment.position);
          seg_format = src->segment.format;
          GST_OBJECT_UNLOCK (src);

          if (position != -1) {
            /* convert to requested format */
            res =
                gst_pad_query_convert (src->srcpad, seg_format,
                position, format, &position);
          } else
            res = TRUE;

          if (res)
            gst_query_set_position (query, format, position);

          break;
        }
      }
      break;
    }
    case GST_QUERY_DURATION:
    {
      GstFormat format;

      gst_query_parse_duration (query, &format, NULL);

      GST_DEBUG_OBJECT (src, "duration query in format %s",
          gst_format_get_name (format));

      switch (format) {
        case GST_FORMAT_PERCENT:
          gst_query_set_duration (query, GST_FORMAT_PERCENT,
              GST_FORMAT_PERCENT_MAX);
          res = TRUE;
          break;
        default:
        {
          gint64 duration;
          GstFormat seg_format;
          guint length = 0;

          /* may have to refresh duration */
          gst_base_idle_src_update_length (src, 0, &length,
              g_atomic_int_get (&src->priv->dynamic_size));

          /* this is the duration as configured by the subclass. */
          GST_OBJECT_LOCK (src);
          duration = src->segment.duration;
          seg_format = src->segment.format;
          GST_OBJECT_UNLOCK (src);

          GST_LOG_OBJECT (src, "duration %" G_GINT64_FORMAT ", format %s",
              duration, gst_format_get_name (seg_format));

          if (duration != -1) {
            /* convert to requested format, if this fails, we have a duration
             * but we cannot answer the query, we must return FALSE. */
            res =
                gst_pad_query_convert (src->srcpad, seg_format,
                duration, format, &duration);
          } else {
            /* The subclass did not configure a duration, we assume that the
             * media has an unknown duration then and we return TRUE to report
             * this. Note that this is not the same as returning FALSE, which
             * means that we cannot report the duration at all. */
            res = TRUE;
          }

          if (res)
            gst_query_set_duration (query, format, duration);

          break;
        }
      }
      break;
    }

    case GST_QUERY_SEGMENT:
    {
      GstFormat format;
      gint64 start, stop;

      GST_OBJECT_LOCK (src);

      format = src->segment.format;

      start =
          gst_segment_to_stream_time (&src->segment, format,
          src->segment.start);
      if ((stop = src->segment.stop) == -1)
        stop = src->segment.duration;
      else
        stop = gst_segment_to_stream_time (&src->segment, format, stop);

      gst_query_set_segment (query, src->segment.rate, format, start, stop);

      GST_OBJECT_UNLOCK (src);
      res = TRUE;
      break;
    }

    case GST_QUERY_FORMATS:
    {
      gst_query_set_formats (query, 3, GST_FORMAT_DEFAULT,
          GST_FORMAT_BYTES, GST_FORMAT_PERCENT);
      res = TRUE;
      break;
    }
    case GST_QUERY_CONVERT:
    {
      GstFormat src_fmt, dest_fmt;
      gint64 src_val, dest_val;

      gst_query_parse_convert (query, &src_fmt, &src_val, &dest_fmt, &dest_val);

      /* we can only convert between equal formats... */
      if (src_fmt == dest_fmt) {
        dest_val = src_val;
        res = TRUE;
      } else
        res = FALSE;

      gst_query_set_convert (query, src_fmt, src_val, dest_fmt, dest_val);
      break;
    }
    case GST_QUERY_LATENCY:
    {
      GstClockTime min, max;
      gboolean live;

      /* Subclasses should override and implement something useful */
      res = gst_base_idle_src_query_latency (src, &live, &min, &max);

      GST_LOG_OBJECT (src, "report latency: live %d, min %" GST_TIME_FORMAT
          ", max %" GST_TIME_FORMAT, live, GST_TIME_ARGS (min),
          GST_TIME_ARGS (max));

      gst_query_set_latency (query, live, min, max);
      break;
    }
    case GST_QUERY_JITTER:
    case GST_QUERY_RATE:
      res = FALSE;
      break;
    case GST_QUERY_BUFFERING:
    {
      GstFormat format, seg_format;
      gint64 start, stop, estimated;

      gst_query_parse_buffering_range (query, &format, NULL, NULL, NULL);

      GST_DEBUG_OBJECT (src, "buffering query in format %s",
          gst_format_get_name (format));

      GST_OBJECT_LOCK (src);
      if (src->random_access) {
        estimated = 0;
        start = 0;
        if (format == GST_FORMAT_PERCENT)
          stop = GST_FORMAT_PERCENT_MAX;
        else
          stop = src->segment.duration;
      } else {
        estimated = -1;
        start = -1;
        stop = -1;
      }
      seg_format = src->segment.format;
      GST_OBJECT_UNLOCK (src);

      /* convert to required format. When the conversion fails, we can't answer
       * the query. When the value is unknown, we can don't perform conversion
       * but report TRUE. */
      if (format != GST_FORMAT_PERCENT && stop != -1) {
        res = gst_pad_query_convert (src->srcpad, seg_format,
            stop, format, &stop);
      } else {
        res = TRUE;
      }
      if (res && format != GST_FORMAT_PERCENT && start != -1)
        res = gst_pad_query_convert (src->srcpad, seg_format,
            start, format, &start);

      gst_query_set_buffering_range (query, format, start, stop, estimated);
      break;
    }
    case GST_QUERY_CAPS:
    {
      GstBaseIdleSrcClass *bclass;
      GstCaps *caps, *filter;

      bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);
      if (bclass->get_caps) {
        gst_query_parse_caps (query, &filter);
        if ((caps = bclass->get_caps (src, filter))) {
          gst_query_set_caps_result (query, caps);
          gst_caps_unref (caps);
          res = TRUE;
        } else {
          res = FALSE;
        }
      } else
        res = FALSE;
      break;
    }
    case GST_QUERY_URI:{
      if (GST_IS_URI_HANDLER (src)) {
        gchar *uri = gst_uri_handler_get_uri (GST_URI_HANDLER (src));

        if (uri != NULL) {
          gst_query_set_uri (query, uri);
          g_free (uri);
          res = TRUE;
        } else {
          res = FALSE;
        }
      } else {
        res = FALSE;
      }
      break;
    }
    default:
      res = FALSE;
      break;
  }
  GST_DEBUG_OBJECT (src, "query %s returns %d", GST_QUERY_TYPE_NAME (query),
      res);

  return res;
}

static gboolean
gst_base_idle_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstBaseIdleSrc *src;
  GstBaseIdleSrcClass *bclass;
  gboolean result = FALSE;

  src = GST_BASE_IDLE_SRC (parent);
  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  if (bclass->query)
    result = bclass->query (src, query);

  return result;
}


static GstFlowReturn
gst_base_idle_src_default_alloc (GstBaseIdleSrc * src, guint64 offset,
    guint size, GstBuffer ** buffer)
{
  GstFlowReturn ret;
  GstBaseIdleSrcPrivate *priv = src->priv;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;

  GST_OBJECT_LOCK (src);
  if (priv->pool) {
    pool = gst_object_ref (priv->pool);
  } else if (priv->allocator) {
    allocator = gst_object_ref (priv->allocator);
  }
  params = priv->params;
  GST_OBJECT_UNLOCK (src);

  if (pool) {
    ret = gst_buffer_pool_acquire_buffer (pool, buffer, NULL);
  } else if (size != -1) {
    *buffer = gst_buffer_new_allocate (allocator, size, &params);
    if (G_UNLIKELY (*buffer == NULL))
      goto alloc_failed;

    ret = GST_FLOW_OK;
  } else {
    GST_WARNING_OBJECT (src, "Not trying to alloc %u bytes. Blocksize not set?",
        size);
    goto alloc_failed;
  }

done:
  if (pool)
    gst_object_unref (pool);
  if (allocator)
    gst_object_unref (allocator);

  return ret;

  /* ERRORS */
alloc_failed:
  {
    GST_ERROR_OBJECT (src, "Failed to allocate %u bytes", size);
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

static GstFlowReturn
gst_base_idle_src_default_create (GstBaseIdleSrc * src, guint64 offset,
    guint size, GstBuffer ** buffer)
{
  GstBaseIdleSrcClass *bclass;
  GstFlowReturn ret;
  GstBuffer *res_buf;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  if (G_UNLIKELY (!bclass->alloc))
    goto no_function;
  if (G_UNLIKELY (!bclass->fill))
    goto no_function;

  if (*buffer == NULL) {
    /* downstream did not provide us with a buffer to fill, allocate one
     * ourselves */
    ret = bclass->alloc (src, offset, size, &res_buf);
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto alloc_failed;
  } else {
    res_buf = *buffer;
  }

  if (G_LIKELY (size > 0)) {
    /* only call fill when there is a size */
    ret = bclass->fill (src, offset, size, res_buf);
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto not_ok;
  }

  *buffer = res_buf;

  return GST_FLOW_OK;

  /* ERRORS */
no_function:
  {
    GST_DEBUG_OBJECT (src, "no fill or alloc function");
    return GST_FLOW_NOT_SUPPORTED;
  }
alloc_failed:
  {
    GST_DEBUG_OBJECT (src, "Failed to allocate buffer of %u bytes", size);
    return ret;
  }
not_ok:
  {
    GST_DEBUG_OBJECT (src, "fill returned %d (%s)", ret,
        gst_flow_get_name (ret));
    if (*buffer == NULL)
      gst_buffer_unref (res_buf);
    return ret;
  }
}

/* all events send to this element directly. This is mainly done from the
 * application.
 */
static gboolean
gst_base_idle_src_send_event (GstElement * element, GstEvent * event)
{
  GstBaseIdleSrc *src;
  gboolean result = FALSE;
  GstBaseIdleSrcClass *bclass;

  src = GST_BASE_IDLE_SRC (element);
  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  GST_DEBUG_OBJECT (src, "handling event %p %" GST_PTR_FORMAT, event, event);

  switch (GST_EVENT_TYPE (event)) {
      /* bidirectional events */
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (src, "pushing flush-start event downstream");

      result = gst_pad_push_event (src->srcpad, event);
      gst_base_idle_src_set_flushing (src, TRUE);
      event = NULL;
      break;
    case GST_EVENT_FLUSH_STOP:
    {
      gboolean start;

      GST_PAD_STREAM_LOCK (src->srcpad);
      gst_base_idle_src_set_flushing (src, FALSE);

      GST_DEBUG_OBJECT (src, "pushing flush-stop event downstream");
      result = gst_pad_push_event (src->srcpad, event);

      /* For external flush, restart the task .. */
      GST_LIVE_LOCK (src);
      src->priv->segment_pending = TRUE;

      GST_OBJECT_LOCK (src->srcpad);
      start = (GST_PAD_MODE (src->srcpad) == GST_PAD_MODE_PUSH);
      GST_OBJECT_UNLOCK (src->srcpad);

      /* ... and for live sources, only if in playing state */
      if (src->is_live) {
        if (!src->live_running)
          start = FALSE;
      }

      if (start)
        gst_pad_start_task (src->srcpad,
            (GstTaskFunction) gst_base_idle_src_loop, src->srcpad, NULL);

      GST_LIVE_UNLOCK (src);
      GST_PAD_STREAM_UNLOCK (src->srcpad);

      event = NULL;
      break;
    }

      /* downstream serialized events */
    case GST_EVENT_EOS:
    {
      gboolean push_mode;

      /* queue EOS and make sure the task or pull function performs the EOS
       * actions.
       *
       * For push mode, This will be done in 3 steps. It is required to not
       * block here as gst_element_send_event() will hold the STATE_LOCK, hence
       * blocking would prevent asynchronous state change to complete.
       *
       * 1. We stop the streaming thread
       * 2. We set the pending eos
       * 3. We start the streaming thread again, so it is performed
       *    asynchronously.
       *
       * For pull mode, we simply mark the pending EOS without flushing.
       */

      GST_OBJECT_LOCK (src->srcpad);
      push_mode = GST_PAD_MODE (src->srcpad) == GST_PAD_MODE_PUSH;
      GST_OBJECT_UNLOCK (src->srcpad);

      if (push_mode) {
        gst_base_idle_src_set_flushing (src, TRUE);

        GST_PAD_STREAM_LOCK (src->srcpad);
        gst_base_idle_src_set_flushing (src, FALSE);

        GST_OBJECT_LOCK (src);
        g_atomic_int_set (&src->priv->has_pending_eos, TRUE);
        if (src->priv->pending_eos)
          gst_event_unref (src->priv->pending_eos);
        src->priv->pending_eos = event;
        GST_OBJECT_UNLOCK (src);

        GST_DEBUG_OBJECT (src,
            "EOS marked, start task for asynchronous handling");
        gst_pad_start_task (src->srcpad,
            (GstTaskFunction) gst_base_idle_src_loop, src->srcpad, NULL);

        GST_PAD_STREAM_UNLOCK (src->srcpad);
      } else {
        /* In pull mode, we need not to return flushing to downstream, though
         * the stream lock is not kept after getrange was unblocked */
        GST_OBJECT_LOCK (src);
        g_atomic_int_set (&src->priv->has_pending_eos, TRUE);
        if (src->priv->pending_eos)
          gst_event_unref (src->priv->pending_eos);
        src->priv->pending_eos = event;
        GST_OBJECT_UNLOCK (src);

        gst_base_idle_src_set_pool_flushing (src, TRUE);
        if (bclass->unlock)
          bclass->unlock (src);

        GST_PAD_STREAM_LOCK (src->srcpad);
        if (bclass->unlock_stop)
          bclass->unlock_stop (src);
        gst_base_idle_src_set_pool_flushing (src, TRUE);
        GST_PAD_STREAM_UNLOCK (src->srcpad);
      }


      event = NULL;
      result = TRUE;
      break;
    }
    case GST_EVENT_SEGMENT:
      /* sending random SEGMENT downstream can break sync. */
      break;
    case GST_EVENT_TAG:
    case GST_EVENT_SINK_MESSAGE:
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    case GST_EVENT_CUSTOM_BOTH:
    case GST_EVENT_PROTECTION:
      /* Insert TAG, CUSTOM_DOWNSTREAM, CUSTOM_BOTH, PROTECTION in the dataflow */
      GST_OBJECT_LOCK (src);
      src->priv->pending_events =
          g_list_append (src->priv->pending_events, event);
      g_atomic_int_set (&src->priv->have_events, TRUE);
      GST_OBJECT_UNLOCK (src);
      event = NULL;
      result = TRUE;
      break;
    case GST_EVENT_BUFFERSIZE:
      /* does not seem to make much sense currently */
      break;

      /* upstream events */
    case GST_EVENT_QOS:
      /* elements should override send_event and do something */
      break;
    case GST_EVENT_NAVIGATION:
      /* could make sense for elements that do something with navigation events
       * but then they would need to override the send_event function */
      break;
    case GST_EVENT_LATENCY:
      /* does not seem to make sense currently */
      break;

      /* custom events */
    case GST_EVENT_CUSTOM_UPSTREAM:
      /* override send_event if you want this */
      break;
    case GST_EVENT_CUSTOM_DOWNSTREAM_OOB:
    case GST_EVENT_CUSTOM_BOTH_OOB:
      /* insert a random custom event into the pipeline */
      GST_DEBUG_OBJECT (src, "pushing custom OOB event downstream");
      result = gst_pad_push_event (src->srcpad, event);
      /* we gave away the ref to the event in the push */
      event = NULL;
      break;
    default:
      break;
  }
done:
  /* if we still have a ref to the event, unref it now */
  if (event)
    gst_event_unref (event);

  return result;

  /* ERRORS */
wrong_mode:
  {
    GST_DEBUG_OBJECT (src, "cannot perform seek when operating in pull mode");
    GST_OBJECT_UNLOCK (src->srcpad);
    result = FALSE;
    goto done;
  }
}

static gboolean
gst_base_idle_src_seekable (GstBaseIdleSrc * src)
{
  GstBaseIdleSrcClass *bclass;
  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);
  if (bclass->is_seekable)
    return bclass->is_seekable (src);
  else
    return FALSE;
}

static void
gst_base_idle_src_update_qos (GstBaseIdleSrc * src,
    gdouble proportion, GstClockTimeDiff diff, GstClockTime timestamp)
{
  GST_CAT_DEBUG_OBJECT (GST_CAT_QOS, src,
      "qos: proportion: %lf, diff %" G_GINT64_FORMAT ", timestamp %"
      GST_TIME_FORMAT, proportion, diff, GST_TIME_ARGS (timestamp));

  GST_OBJECT_LOCK (src);
  src->priv->proportion = proportion;
  src->priv->earliest_time = timestamp + diff;
  GST_OBJECT_UNLOCK (src);
}


static gboolean
gst_base_idle_src_default_event (GstBaseIdleSrc * src, GstEvent * event)
{
  gboolean result;

  GST_DEBUG_OBJECT (src, "handle event %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      /* is normally called when in push mode */
      if (!gst_base_idle_src_seekable (src))
        goto not_seekable;

      result = gst_base_idle_src_perform_seek (src, event, TRUE);
      break;
    case GST_EVENT_FLUSH_START:
      /* cancel any blocking getrange, is normally called
       * when in pull mode. */
      result = gst_base_idle_src_set_flushing (src, TRUE);
      break;
    case GST_EVENT_FLUSH_STOP:
      result = gst_base_idle_src_set_flushing (src, FALSE);
      break;
    case GST_EVENT_QOS:
    {
      gdouble proportion;
      GstClockTimeDiff diff;
      GstClockTime timestamp;

      gst_event_parse_qos (event, NULL, &proportion, &diff, &timestamp);
      gst_base_idle_src_update_qos (src, proportion, diff, timestamp);
      result = TRUE;
      break;
    }
    case GST_EVENT_RECONFIGURE:
      result = TRUE;
      break;
    case GST_EVENT_LATENCY:
      result = TRUE;
      break;
    default:
      result = FALSE;
      break;
  }
  return result;

  /* ERRORS */
not_seekable:
  {
    GST_DEBUG_OBJECT (src, "is not seekable");
    return FALSE;
  }
}

static gboolean
gst_base_idle_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstBaseIdleSrc *src;
  GstBaseIdleSrcClass *bclass;
  gboolean result = FALSE;

  src = GST_BASE_IDLE_SRC (parent);
  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  if (bclass->event) {
    if (!(result = bclass->event (src, event)))
      goto subclass_failed;
  }

done:
  gst_event_unref (event);

  return result;

  /* ERRORS */
subclass_failed:
  {
    GST_DEBUG_OBJECT (src, "subclass refused event");
    goto done;
  }
}

static void
gst_base_idle_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBaseIdleSrc *src;

  src = GST_BASE_IDLE_SRC (object);

  switch (prop_id) {
    case PROP_DO_TIMESTAMP:
      gst_base_idle_src_set_do_timestamp (src, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_base_idle_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstBaseIdleSrc *src;

  src = GST_BASE_IDLE_SRC (object);

  switch (prop_id) {
    case PROP_DO_TIMESTAMP:
      g_value_set_boolean (value, gst_base_idle_src_get_do_timestamp (src));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

#if 0

gst_base_idle_src_send_stream_start (src);

  /* The stream-start event could've caused something to flush us */
GST_LIVE_LOCK (src);
if (G_UNLIKELY (src->priv->flushing || GST_PAD_IS_FLUSHING (pad)))
  goto flushing;
GST_LIVE_UNLOCK (src);

  /* check if we need to renegotiate */
if (gst_pad_check_reconfigure (pad)) {
  if (!gst_base_idle_src_negotiate_unlocked (src)) {
    gst_pad_mark_reconfigure (pad);
    if (GST_PAD_IS_FLUSHING (pad)) {
      GST_LIVE_LOCK (src);
      goto flushing;
    } else {
      goto negotiate_failed;
    }
  }
}

  /* push events to close/start our segment before we push the buffer. */
if (G_UNLIKELY (src->priv->segment_pending)) {
  GstEvent *seg_event = gst_event_new_segment (&src->segment);

  gst_event_set_seqnum (seg_event, src->priv->segment_seqnum);
  src->priv->segment_seqnum = gst_util_seqnum_next ();
  gst_pad_push_event (pad, seg_event);
  src->priv->segment_pending = FALSE;
}

if (g_atomic_int_get (&src->priv->have_events)) {
  GST_OBJECT_LOCK (src);
  /* take the events */
  pending_events = src->priv->pending_events;
  src->priv->pending_events = NULL;
  g_atomic_int_set (&src->priv->have_events, FALSE);
  GST_OBJECT_UNLOCK (src);
}

  /* Push out pending events if any */
if (G_UNLIKELY (pending_events != NULL)) {
  for (tmp = pending_events; tmp; tmp = g_list_next (tmp)) {
    GstEvent *ev = (GstEvent *) tmp->data;
    gst_pad_push_event (pad, ev);
  }
  g_list_free (pending_events);
}

  /* push buffer or buffer list */
if (src->priv->pending_bufferlist != NULL) {
  ret = gst_pad_push_list (pad, src->priv->pending_bufferlist);
  src->priv->pending_bufferlist = NULL;
} else {
  ret = gst_pad_push (pad, buf);
}

if (G_UNLIKELY (ret != GST_FLOW_OK)) {
  if (ret == GST_FLOW_NOT_NEGOTIATED) {
    goto not_negotiated;
  }
  GST_INFO_OBJECT (src, "pausing after gst_pad_push() = %s",
      gst_flow_get_name (ret));
  goto pause;
}

  /* special cases */
not_negotiated:
{
  if (gst_pad_needs_reconfigure (pad)) {
    GST_DEBUG_OBJECT (src, "Retrying to renegotiate");
    return;
  }
  /* fallthrough when push returns NOT_NEGOTIATED and we don't have
   * a pending negotiation request on our srcpad */
}

negotiate_failed:
{
  GST_DEBUG_OBJECT (src, "Not negotiated");
  ret = GST_FLOW_NOT_NEGOTIATED;
  goto pause;
}


#endif

static gboolean
gst_base_idle_src_set_allocation (GstBaseIdleSrc * basesrc,
    GstBufferPool * pool, GstAllocator * allocator,
    const GstAllocationParams * params)
{
  GstAllocator *oldalloc;
  GstBufferPool *oldpool;
  GstBaseIdleSrcPrivate *priv = basesrc->priv;

  if (pool) {
    GST_DEBUG_OBJECT (basesrc, "activate pool");
    if (!gst_buffer_pool_set_active (pool, TRUE))
      goto activate_failed;
  }

  GST_OBJECT_LOCK (basesrc);
  oldpool = priv->pool;
  priv->pool = pool;

  oldalloc = priv->allocator;
  priv->allocator = allocator;

  if (priv->pool)
    gst_object_ref (priv->pool);
  if (priv->allocator)
    gst_object_ref (priv->allocator);

  if (params)
    priv->params = *params;
  else
    gst_allocation_params_init (&priv->params);
  GST_OBJECT_UNLOCK (basesrc);

  if (oldpool) {
    /* only deactivate if the pool is not the one we're using */
    if (oldpool != pool) {
      GST_DEBUG_OBJECT (basesrc, "deactivate old pool");
      gst_buffer_pool_set_active (oldpool, FALSE);
    }
    gst_object_unref (oldpool);
  }
  if (oldalloc) {
    gst_object_unref (oldalloc);
  }
  return TRUE;

  /* ERRORS */
activate_failed:
  {
    GST_ERROR_OBJECT (basesrc, "failed to activate bufferpool.");
    return FALSE;
  }
}

static void
gst_base_idle_src_set_pool_flushing (GstBaseIdleSrc * basesrc,
    gboolean flushing)
{
  GstBaseIdleSrcPrivate *priv = basesrc->priv;
  GstBufferPool *pool;

  GST_OBJECT_LOCK (basesrc);
  if ((pool = priv->pool))
    pool = gst_object_ref (pool);
  GST_OBJECT_UNLOCK (basesrc);

  if (pool) {
    gst_buffer_pool_set_flushing (pool, flushing);
    gst_object_unref (pool);
  }
}


static gboolean
gst_base_idle_src_decide_allocation_default (GstBaseIdleSrc * basesrc,
    GstQuery * query)
{
  GstCaps *outcaps;
  GstBufferPool *pool;
  guint size, min, max;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstStructure *config;
  gboolean update_allocator;

  gst_query_parse_allocation (query, &outcaps, NULL);

  /* we got configuration from our peer or the decide_allocation method,
   * parse them */
  if (gst_query_get_n_allocation_params (query) > 0) {
    /* try the allocator */
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
    update_allocator = TRUE;
  } else {
    allocator = NULL;
    gst_allocation_params_init (&params);
    update_allocator = FALSE;
  }

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    if (pool == NULL) {
      /* no pool, we can make our own */
      GST_DEBUG_OBJECT (basesrc, "no pool, making new pool");
      pool = gst_buffer_pool_new ();
    }
  } else {
    pool = NULL;
    size = min = max = 0;
  }

  /* now configure */
  if (pool) {
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
    gst_buffer_pool_config_set_allocator (config, allocator, &params);

    /* buffer pool may have to do some changes */
    if (!gst_buffer_pool_set_config (pool, config)) {
      config = gst_buffer_pool_get_config (pool);

      /* If change are not acceptable, fallback to generic pool */
      if (!gst_buffer_pool_config_validate_params (config, outcaps, size, min,
              max)) {
        GST_DEBUG_OBJECT (basesrc, "unsupported pool, making new pool");

        gst_object_unref (pool);
        pool = gst_buffer_pool_new ();
        gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
        gst_buffer_pool_config_set_allocator (config, allocator, &params);
      }

      if (!gst_buffer_pool_set_config (pool, config))
        goto config_failed;
    }
  }

  if (update_allocator)
    gst_query_set_nth_allocation_param (query, 0, allocator, &params);
  else
    gst_query_add_allocation_param (query, allocator, &params);
  if (allocator)
    gst_object_unref (allocator);

  if (pool) {
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
    gst_object_unref (pool);
  }

  return TRUE;

config_failed:
  GST_ELEMENT_ERROR (basesrc, RESOURCE, SETTINGS,
      ("Failed to configure the buffer pool"),
      ("Configuration is most likely invalid, please report this issue."));
  gst_object_unref (pool);
  return FALSE;
}

static gboolean
gst_base_idle_src_prepare_allocation (GstBaseIdleSrc * basesrc, GstCaps * caps)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result = TRUE;
  GstQuery *query;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (basesrc);

  /* make query and let peer pad answer, we don't really care if it worked or
   * not, if it failed, the allocation query would contain defaults and the
   * subclass would then set better values if needed */
  query = gst_query_new_allocation (caps, TRUE);
  if (!gst_pad_peer_query (basesrc->srcpad, query)) {
    /* not a problem, just debug a little */
    GST_DEBUG_OBJECT (basesrc, "peer ALLOCATION query failed");
  }

  g_assert (bclass->decide_allocation != NULL);
  result = bclass->decide_allocation (basesrc, query);

  GST_DEBUG_OBJECT (basesrc, "ALLOCATION (%d) params: %" GST_PTR_FORMAT, result,
      query);

  if (!result)
    goto no_decide_allocation;

  /* we got configuration from our peer or the decide_allocation method,
   * parse them */
  if (gst_query_get_n_allocation_params (query) > 0) {
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
  } else {
    allocator = NULL;
    gst_allocation_params_init (&params);
  }

  if (gst_query_get_n_allocation_pools (query) > 0)
    gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);

  result = gst_base_idle_src_set_allocation (basesrc, pool, allocator, &params);

  if (allocator)
    gst_object_unref (allocator);
  if (pool)
    gst_object_unref (pool);

  gst_query_unref (query);

  return result;

  /* Errors */
no_decide_allocation:
  {
    GST_WARNING_OBJECT (basesrc, "Subclass failed to decide allocation");
    gst_query_unref (query);

    return result;
  }
}

/* default negotiation code.
 *
 * Take intersection between src and sink pads, take first
 * caps and fixate.
 */
static gboolean
gst_base_idle_src_default_negotiate (GstBaseIdleSrc * basesrc)
{
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_BASE_IDLE_SRC_PAD (basesrc), NULL);
  GST_DEBUG_OBJECT (basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL || gst_caps_is_any (thiscaps))
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps =
      gst_pad_peer_query_caps (GST_BASE_IDLE_SRC_PAD (basesrc), thiscaps);
  GST_DEBUG_OBJECT (basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps && !gst_caps_is_empty (caps)) {
    /* now fixate */
    GST_DEBUG_OBJECT (basesrc, "have caps: %" GST_PTR_FORMAT, caps);
    if (gst_caps_is_any (caps)) {
      GST_DEBUG_OBJECT (basesrc, "any caps, we stop");
      /* hmm, still anything, so element can do anything and
       * nego is not needed */
      result = TRUE;
    } else {
      caps = gst_base_idle_src_fixate (basesrc, caps);
      GST_DEBUG_OBJECT (basesrc, "fixated to: %" GST_PTR_FORMAT, caps);
      if (gst_caps_is_fixed (caps)) {
        /* yay, fixed caps, use those then, it's possible that the subclass does
         * not accept this caps after all and we have to fail. */
        result = gst_base_idle_src_set_caps (basesrc, caps);
      }
    }
    gst_caps_unref (caps);
  } else {
    if (caps)
      gst_caps_unref (caps);
    GST_DEBUG_OBJECT (basesrc, "no common caps");
  }
  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
}

static gboolean
gst_base_idle_src_negotiate_unlocked (GstBaseIdleSrc * basesrc)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (basesrc);

  GST_DEBUG_OBJECT (basesrc, "starting negotiation");

  if (G_LIKELY (bclass->negotiate))
    result = bclass->negotiate (basesrc);
  else
    result = TRUE;

  if (G_LIKELY (result)) {
    GstCaps *caps;

    caps = gst_pad_get_current_caps (basesrc->srcpad);

    result = gst_base_idle_src_prepare_allocation (basesrc, caps);

    if (caps)
      gst_caps_unref (caps);
  }
  return result;
}

/**
 * gst_base_idle_src_negotiate:
 * @src: base source instance
 *
 * Negotiates src pad caps with downstream elements.
 * Unmarks GST_PAD_FLAG_NEED_RECONFIGURE in any case. But marks it again
 * if #GstBaseIdleSrcClass::negotiate fails.
 *
 * Do not call this in the #GstBaseIdleSrcClass::fill vmethod. Call this in
 * #GstBaseIdleSrcClass::create or in #GstBaseIdleSrcClass::alloc, _before_ any
 * buffer is allocated.
 *
 * Returns: %TRUE if the negotiation succeeded, else %FALSE.
 *
 * Since: 1.18
 */
gboolean
gst_base_idle_src_negotiate (GstBaseIdleSrc * src)
{
  gboolean ret = TRUE;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);

  GST_PAD_STREAM_LOCK (src->srcpad);
  gst_pad_check_reconfigure (src->srcpad);
  ret = gst_base_idle_src_negotiate_unlocked (src);
  if (!ret)
    gst_pad_mark_reconfigure (src->srcpad);
  GST_PAD_STREAM_UNLOCK (src->srcpad);

  return ret;
}

static gboolean
gst_base_idle_src_start (GstBaseIdleSrc * basesrc)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result;

  GST_LIVE_LOCK (basesrc);

  GST_OBJECT_LOCK (basesrc);
  if (GST_BASE_IDLE_SRC_IS_STARTING (basesrc))
    goto was_starting;
  if (GST_BASE_IDLE_SRC_IS_STARTED (basesrc))
    goto was_started;

  basesrc->priv->start_result = GST_FLOW_FLUSHING;
  GST_OBJECT_FLAG_SET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTING);
  gst_segment_init (&basesrc->segment, basesrc->segment.format);
  GST_OBJECT_UNLOCK (basesrc);

  basesrc->num_buffers_left = basesrc->num_buffers;
  basesrc->running = FALSE;
  basesrc->priv->segment_pending = FALSE;
  basesrc->priv->segment_seqnum = gst_util_seqnum_next ();
  basesrc->priv->forced_eos = FALSE;
  GST_LIVE_UNLOCK (basesrc);

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (basesrc);
  if (bclass->start)
    result = bclass->start (basesrc);
  else
    result = TRUE;

  if (!result)
    goto could_not_start;

  if (!gst_base_idle_src_is_async (basesrc)) {
    gst_base_idle_src_start_complete (basesrc, GST_FLOW_OK);
    /* not really waiting here, we call this to get the result
     * from the start_complete call */
    result = gst_base_idle_src_start_wait (basesrc) == GST_FLOW_OK;
  }

  return result;

  /* ERROR */
was_starting:
  {
    GST_DEBUG_OBJECT (basesrc, "was starting");
    GST_OBJECT_UNLOCK (basesrc);
    GST_LIVE_UNLOCK (basesrc);
    return TRUE;
  }
was_started:
  {
    GST_DEBUG_OBJECT (basesrc, "was started");
    GST_OBJECT_UNLOCK (basesrc);
    GST_LIVE_UNLOCK (basesrc);
    return TRUE;
  }
could_not_start:
  {
    GST_DEBUG_OBJECT (basesrc, "could not start");
    /* subclass is supposed to post a message but we post one as a fallback
     * just in case. We don't have to call _stop. */
    GST_ELEMENT_ERROR (basesrc, CORE, STATE_CHANGE, (NULL),
        ("Failed to start"));
    gst_base_idle_src_start_complete (basesrc, GST_FLOW_ERROR);
    return FALSE;
  }
}

/**
 * gst_base_idle_src_start_complete:
 * @basesrc: base source instance
 * @ret: a #GstFlowReturn
 *
 * Complete an asynchronous start operation. When the subclass overrides the
 * start method, it should call gst_base_idle_src_start_complete() when the start
 * operation completes either from the same thread or from an asynchronous
 * helper thread.
 */
void
gst_base_idle_src_start_complete (GstBaseIdleSrc * basesrc, GstFlowReturn ret)
{
  gboolean have_size;
  guint64 size;
  gboolean seekable;
  GstFormat format;
  GstPadMode mode;
  GstEvent *event;

  if (ret != GST_FLOW_OK)
    goto error;

  GST_DEBUG_OBJECT (basesrc, "starting source");
  format = basesrc->segment.format;

  /* figure out the size */
  have_size = FALSE;
  size = -1;
  if (format == GST_FORMAT_BYTES) {
    GstBaseIdleSrcClass *bclass = GST_BASE_IDLE_SRC_GET_CLASS (basesrc);

    if (bclass->get_size) {
      if (!(have_size = bclass->get_size (basesrc, &size)))
        size = -1;
    }
    GST_DEBUG_OBJECT (basesrc, "setting size %" G_GUINT64_FORMAT, size);
    /* only update the size when operating in bytes, subclass is supposed
     * to set duration in the start method for other formats */
    GST_OBJECT_LOCK (basesrc);
    basesrc->segment.duration = size;
    GST_OBJECT_UNLOCK (basesrc);
  }

  GST_DEBUG_OBJECT (basesrc,
      "format: %s, have size: %d, size: %" G_GUINT64_FORMAT ", duration: %"
      G_GINT64_FORMAT, gst_format_get_name (format), have_size, size,
      basesrc->segment.duration);

  seekable = gst_base_idle_src_seekable (basesrc);
  GST_DEBUG_OBJECT (basesrc, "is seekable: %d", seekable);

  /* update for random access flag */
  basesrc->random_access = seekable && format == GST_FORMAT_BYTES;

  GST_DEBUG_OBJECT (basesrc, "is random_access: %d", basesrc->random_access);

  gst_pad_mark_reconfigure (GST_BASE_IDLE_SRC_PAD (basesrc));

  GST_OBJECT_LOCK (basesrc->srcpad);
  mode = GST_PAD_MODE (basesrc->srcpad);
  GST_OBJECT_UNLOCK (basesrc->srcpad);

  /* take the stream lock here, we only want to let the task run when we have
   * set the STARTED flag */
  GST_PAD_STREAM_LOCK (basesrc->srcpad);
  switch (mode) {
    case GST_PAD_MODE_PUSH:
      /* do initial seek, which will start the task */
      GST_OBJECT_LOCK (basesrc);
      event = basesrc->pending_seek;
      basesrc->pending_seek = NULL;
      GST_OBJECT_UNLOCK (basesrc);

      /* The perform seek code will start the task when finished. We don't have to
       * unlock the streaming thread because it is not running yet */
      if (G_UNLIKELY (!gst_base_idle_src_perform_seek (basesrc, event, FALSE)))
        goto seek_failed;

      if (event)
        gst_event_unref (event);
      break;
    case GST_PAD_MODE_PULL:
      /* if not random_access, we cannot operate in pull mode for now */
      if (G_UNLIKELY (!basesrc->random_access))
        goto no_get_range;
      break;
    default:
      goto not_activated_yet;
      break;
  }

  GST_OBJECT_LOCK (basesrc);
  GST_OBJECT_FLAG_SET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTED);
  GST_OBJECT_FLAG_UNSET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTING);
  basesrc->priv->start_result = ret;
  GST_ASYNC_SIGNAL (basesrc);
  GST_OBJECT_UNLOCK (basesrc);

  GST_PAD_STREAM_UNLOCK (basesrc->srcpad);

  return;

seek_failed:
  {
    GST_PAD_STREAM_UNLOCK (basesrc->srcpad);
    GST_ERROR_OBJECT (basesrc, "Failed to perform initial seek");
    gst_base_idle_src_stop (basesrc);
    if (event)
      gst_event_unref (event);
    ret = GST_FLOW_ERROR;
    goto error;
  }
no_get_range:
  {
    GST_PAD_STREAM_UNLOCK (basesrc->srcpad);
    gst_base_idle_src_stop (basesrc);
    GST_ERROR_OBJECT (basesrc, "Cannot operate in pull mode, stopping");
    ret = GST_FLOW_ERROR;
    goto error;
  }
not_activated_yet:
  {
    GST_PAD_STREAM_UNLOCK (basesrc->srcpad);
    gst_base_idle_src_stop (basesrc);
    GST_WARNING_OBJECT (basesrc, "pad not activated yet");
    ret = GST_FLOW_ERROR;
    goto error;
  }
error:
  {
    GST_OBJECT_LOCK (basesrc);
    basesrc->priv->start_result = ret;
    GST_OBJECT_FLAG_UNSET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTING);
    GST_ASYNC_SIGNAL (basesrc);
    GST_OBJECT_UNLOCK (basesrc);
    return;
  }
}

/**
 * gst_base_idle_src_start_wait:
 * @basesrc: base source instance
 *
 * Wait until the start operation completes.
 *
 * Returns: a #GstFlowReturn.
 */
GstFlowReturn
gst_base_idle_src_start_wait (GstBaseIdleSrc * basesrc)
{
  GstFlowReturn result;

  GST_OBJECT_LOCK (basesrc);
  while (GST_BASE_IDLE_SRC_IS_STARTING (basesrc)) {
    GST_ASYNC_WAIT (basesrc);
  }
  result = basesrc->priv->start_result;
  GST_OBJECT_UNLOCK (basesrc);

  GST_DEBUG_OBJECT (basesrc, "got %s", gst_flow_get_name (result));

  return result;
}

static gboolean
gst_base_idle_src_stop (GstBaseIdleSrc * basesrc)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result = TRUE;

  GST_DEBUG_OBJECT (basesrc, "stopping source");

  /* flush all */
  gst_base_idle_src_set_flushing (basesrc, TRUE);

  /* stop the task */
  gst_pad_stop_task (basesrc->srcpad);
  /* stop flushing, this will balance unlock/unlock_stop calls */
  gst_base_idle_src_set_flushing (basesrc, FALSE);

  GST_OBJECT_LOCK (basesrc);
  if (!GST_BASE_IDLE_SRC_IS_STARTED (basesrc)
      && !GST_BASE_IDLE_SRC_IS_STARTING (basesrc))
    goto was_stopped;

  GST_OBJECT_FLAG_UNSET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTING);
  GST_OBJECT_FLAG_UNSET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTED);
  basesrc->priv->start_result = GST_FLOW_FLUSHING;
  GST_ASYNC_SIGNAL (basesrc);
  GST_OBJECT_UNLOCK (basesrc);

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (basesrc);
  if (bclass->stop)
    result = bclass->stop (basesrc);

  if (basesrc->priv->pending_bufferlist != NULL) {
    gst_buffer_list_unref (basesrc->priv->pending_bufferlist);
    basesrc->priv->pending_bufferlist = NULL;
  }

  gst_base_idle_src_set_allocation (basesrc, NULL, NULL, NULL);

  return result;

was_stopped:
  {
    GST_DEBUG_OBJECT (basesrc, "was stopped");
    GST_OBJECT_UNLOCK (basesrc);
    return TRUE;
  }
}

/* start or stop flushing dataprocessing
 */
static gboolean
gst_base_idle_src_set_flushing (GstBaseIdleSrc * basesrc, gboolean flushing)
{
  GstBaseIdleSrcClass *bclass;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (basesrc);

  GST_DEBUG_OBJECT (basesrc, "flushing %d", flushing);

  if (flushing) {
    gst_base_idle_src_set_pool_flushing (basesrc, TRUE);
    /* unlock any subclasses to allow turning off the streaming thread */
    if (bclass->unlock)
      bclass->unlock (basesrc);
  }

  /* the live lock is released when we are blocked, waiting for playing,
   * when we sync to the clock or creating a buffer */
  GST_LIVE_LOCK (basesrc);
  basesrc->priv->flushing = flushing;
  if (flushing) {
    /* clear pending EOS if any */
    if (g_atomic_int_get (&basesrc->priv->has_pending_eos)) {
      GST_OBJECT_LOCK (basesrc);
      CLEAR_PENDING_EOS (basesrc);
      basesrc->priv->forced_eos = FALSE;
      GST_OBJECT_UNLOCK (basesrc);
    }

    /* unblock clock sync (if any) or any other blocking thing */
    if (basesrc->clock_id)
      gst_clock_id_unschedule (basesrc->clock_id);
  } else {
    gst_base_idle_src_set_pool_flushing (basesrc, FALSE);

    /* Drop all delayed events */
    GST_OBJECT_LOCK (basesrc);
    if (basesrc->priv->pending_events) {
      g_list_foreach (basesrc->priv->pending_events, (GFunc) gst_event_unref,
          NULL);
      g_list_free (basesrc->priv->pending_events);
      basesrc->priv->pending_events = NULL;
      g_atomic_int_set (&basesrc->priv->have_events, FALSE);
    }
    GST_OBJECT_UNLOCK (basesrc);
  }

  GST_LIVE_SIGNAL (basesrc);
  GST_LIVE_UNLOCK (basesrc);

  if (!flushing) {
    /* Now wait for the stream lock to be released and clear our unlock request */
    GST_PAD_STREAM_LOCK (basesrc->srcpad);
    if (bclass->unlock_stop)
      bclass->unlock_stop (basesrc);
    GST_PAD_STREAM_UNLOCK (basesrc->srcpad);
  }

  return TRUE;
}

static GstStateChangeReturn
gst_base_idle_src_change_state (GstElement * element, GstStateChange transition)
{
  GstBaseIdleSrc *basesrc;
  GstStateChangeReturn result;
  gboolean no_preroll = FALSE;

  basesrc = GST_BASE_IDLE_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      no_preroll = gst_base_idle_src_is_live (basesrc);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      GST_DEBUG_OBJECT (basesrc, "PAUSED->PLAYING");
      if (gst_base_idle_src_is_live (basesrc)) {
        /* now we can start playback */
        gst_base_idle_src_set_playing (basesrc, TRUE);
      }
      break;
    default:
      break;
  }

  if ((result =
          GST_ELEMENT_CLASS (parent_class)->change_state (element,
              transition)) == GST_STATE_CHANGE_FAILURE)
    goto failure;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      GST_DEBUG_OBJECT (basesrc, "PLAYING->PAUSED");
      if (gst_base_idle_src_is_live (basesrc)) {
        /* make sure we block in the live cond in PAUSED */
        gst_base_idle_src_set_playing (basesrc, FALSE);
        no_preroll = TRUE;
      }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    {
      /* we don't need to unblock anything here, the pad deactivation code
       * already did this */
      if (g_atomic_int_get (&basesrc->priv->has_pending_eos)) {
        GST_OBJECT_LOCK (basesrc);
        CLEAR_PENDING_EOS (basesrc);
        GST_OBJECT_UNLOCK (basesrc);
      }
      gst_event_replace (&basesrc->pending_seek, NULL);
      break;
    }
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  if (no_preroll && result == GST_STATE_CHANGE_SUCCESS)
    result = GST_STATE_CHANGE_NO_PREROLL;

  return result;

  /* ERRORS */
failure:
  {
    GST_DEBUG_OBJECT (basesrc, "parent failed state change");
    return result;
  }
}

/**
 * gst_base_idle_src_get_buffer_pool:
 * @src: a #GstBaseIdleSrc
 *
 * Returns: (nullable) (transfer full): the instance of the #GstBufferPool used
 * by the src; unref it after usage.
 */
GstBufferPool *
gst_base_idle_src_get_buffer_pool (GstBaseIdleSrc * src)
{
  GstBufferPool *ret = NULL;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), NULL);

  GST_OBJECT_LOCK (src);
  if (src->priv->pool)
    ret = gst_object_ref (src->priv->pool);
  GST_OBJECT_UNLOCK (src);

  return ret;
}

/**
 * gst_base_idle_src_get_allocator:
 * @src: a #GstBaseIdleSrc
 * @allocator: (out) (optional) (nullable) (transfer full): the #GstAllocator
 * used
 * @params: (out caller-allocates) (optional): the #GstAllocationParams of @allocator
 *
 * Lets #GstBaseIdleSrc sub-classes to know the memory @allocator
 * used by the base class and its @params.
 *
 * Unref the @allocator after usage.
 */
void
gst_base_idle_src_get_allocator (GstBaseIdleSrc * src,
    GstAllocator ** allocator, GstAllocationParams * params)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));

  GST_OBJECT_LOCK (src);
  if (allocator)
    *allocator = src->priv->allocator ?
        gst_object_ref (src->priv->allocator) : NULL;

  if (params)
    *params = src->priv->params;
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_idle_src_submit_buffer_list:
 * @src: a #GstBaseIdleSrc
 * @buffer_list: (transfer full): a #GstBufferList
 *
 * Subclasses can call this from their create virtual method implementation
 * to submit a buffer list to be pushed out later. This is useful in
 * cases where the create function wants to produce multiple buffers to be
 * pushed out in one go in form of a #GstBufferList, which can reduce overhead
 * drastically, especially for packetised inputs (for data streams where
 * the packetisation/chunking is not important it is usually more efficient
 * to return larger buffers instead).
 *
 * Subclasses that use this function from their create function must return
 * %GST_FLOW_OK and no buffer from their create virtual method implementation.
 * If a buffer is returned after a buffer list has also been submitted via this
 * function the behaviour is undefined.
 *
 * Subclasses must only call this function once per create function call and
 * subclasses must only call this function when the source operates in push
 * mode.
 *
 * Since: 1.14
 */
void
gst_base_idle_src_submit_buffer_list (GstBaseIdleSrc * src,
    GstBufferList * buffer_list)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));
  g_return_if_fail (GST_IS_BUFFER_LIST (buffer_list));
  g_return_if_fail (BASE_IDLE_SRC_HAS_PENDING_BUFFER_LIST (src) == FALSE);

  /* we need it to be writable later in get_range() where we use get_writable */
  src->priv->pending_bufferlist = gst_buffer_list_make_writable (buffer_list);

  GST_LOG_OBJECT (src, "%u buffers submitted in buffer list",
      gst_buffer_list_length (buffer_list));
}

static void
gst_base_idle_src_finalize (GObject * object)
{
  GstBaseIdleSrc *basesrc;
  GstEvent **event_p;

  basesrc = GST_BASE_IDLE_SRC (object);

  g_mutex_clear (&basesrc->live_lock);
  g_cond_clear (&basesrc->live_cond);
  g_cond_clear (&basesrc->priv->async_cond);

  event_p = &basesrc->pending_seek;
  gst_event_replace (event_p, NULL);

  if (basesrc->priv->pending_events) {
    g_list_foreach (basesrc->priv->pending_events, (GFunc) gst_event_unref,
        NULL);
    g_list_free (basesrc->priv->pending_events);
  }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_base_idle_src_class_init (GstBaseIdleSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);

  if (private_offset != 0)
    g_type_class_adjust_private_offset (klass, &private_offset);

  GST_DEBUG_CATEGORY_INIT (gst_base_idle_src_debug, "basesrc", 0,
      "basesrc element");

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_base_idle_src_finalize;
  gobject_class->set_property = gst_base_idle_src_set_property;
  gobject_class->get_property = gst_base_idle_src_get_property;

  g_object_class_install_property (gobject_class, PROP_DO_TIMESTAMP,
      g_param_spec_boolean ("do-timestamp", "Do timestamp",
          "Apply current stream time to buffers", DEFAULT_DO_TIMESTAMP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_base_idle_src_change_state);
  gstelement_class->send_event =
      GST_DEBUG_FUNCPTR (gst_base_idle_src_send_event);

  klass->get_caps = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_get_caps);
  klass->negotiate = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_negotiate);
  klass->fixate = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_fixate);
  klass->prepare_seek_segment =
      GST_DEBUG_FUNCPTR (gst_base_idle_src_default_prepare_seek_segment);
  klass->do_seek = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_do_seek);
  klass->query = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_query);
  klass->event = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_event);
  klass->create = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_create);
  klass->alloc = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_alloc);
  klass->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_base_idle_src_decide_allocation_default);

  /* Registering debug symbols for function pointers */

  GST_DEBUG_REGISTER_FUNCPTR (gst_base_idle_src_event);
  GST_DEBUG_REGISTER_FUNCPTR (gst_base_idle_src_query);
  GST_DEBUG_REGISTER_FUNCPTR (gst_base_idle_src_fixate);
}

static void
gst_base_idle_src_init (GstBaseIdleSrc * basesrc, gpointer g_class)
{
  GstPad *pad;
  GstPadTemplate *pad_template;

  basesrc->priv = gst_base_idle_src_get_instance_private (basesrc);

  basesrc->is_live = FALSE;
  g_mutex_init (&basesrc->live_lock);
  g_cond_init (&basesrc->live_cond);
  basesrc->num_buffers = DEFAULT_NUM_BUFFERS;
  basesrc->num_buffers_left = -1;
  g_atomic_int_set (&basesrc->priv->automatic_eos, TRUE);

  basesrc->can_activate_push = TRUE;

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (g_class), "src");
  g_return_if_fail (pad_template != NULL);

  GST_DEBUG_OBJECT (basesrc, "creating src pad");
  pad = gst_pad_new_from_template (pad_template, "src");

  GST_DEBUG_OBJECT (basesrc, "setting functions on src pad");
  gst_pad_set_event_function (pad, gst_base_idle_src_event);
  gst_pad_set_query_function (pad, gst_base_idle_src_query);

  /* hold pointer to pad */
  basesrc->srcpad = pad;
  GST_DEBUG_OBJECT (basesrc, "adding src pad");
  gst_element_add_pad (GST_ELEMENT (basesrc), pad);

  basesrc->blocksize = DEFAULT_BLOCKSIZE;
  basesrc->clock_id = NULL;
  /* we operate in BYTES by default */
  gst_base_idle_src_set_format (basesrc, GST_FORMAT_BYTES);
  basesrc->priv->do_timestamp = DEFAULT_DO_TIMESTAMP;
  g_atomic_int_set (&basesrc->priv->have_events, FALSE);

  g_cond_init (&basesrc->priv->async_cond);
  basesrc->priv->start_result = GST_FLOW_FLUSHING;
  GST_OBJECT_FLAG_UNSET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTED);
  GST_OBJECT_FLAG_UNSET (basesrc, GST_BASE_IDLE_SRC_FLAG_STARTING);
  GST_OBJECT_FLAG_SET (basesrc, GST_ELEMENT_FLAG_SOURCE);

  GST_DEBUG_OBJECT (basesrc, "init done");
}
