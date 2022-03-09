/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *                    2000 Wim Taymans <wtay@chello.be>
 *                    2005 Wim Taymans <wim@fluendo.com>
 *
 * gstbasesrc.h:
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

#ifndef __GST_BASE_IDLE_SRC_H__
#define __GST_BASE_IDLE_SRC_H__

#include <gst/gst.h>
#include <gst/base/base-prelude.h>

G_BEGIN_DECLS

#define GST_TYPE_BASE_IDLE_SRC               (gst_base_idle_src_get_type())
#define GST_BASE_IDLE_SRC(obj)               (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BASE_IDLE_SRC,GstBaseIdleSrc))
#define GST_BASE_IDLE_SRC_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_BASE_IDLE_SRC,GstBaseIdleSrcClass))
#define GST_BASE_IDLE_SRC_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_BASE_IDLE_SRC, GstBaseIdleSrcClass))
#define GST_IS_BASE_IDLE_SRC(obj)            (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BASE_IDLE_SRC))
#define GST_IS_BASE_IDLE_SRC_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_BASE_IDLE_SRC))
#define GST_BASE_IDLE_SRC_CAST(obj)          ((GstBaseIdleSrc *)(obj))

/**
 * GstBaseIdleSrcFlags:
 * @GST_BASE_IDLE_SRC_FLAG_STARTING: has source is starting
 * @GST_BASE_IDLE_SRC_FLAG_STARTED: has source been started
 * @GST_BASE_IDLE_SRC_FLAG_LAST: offset to define more flags
 *
 * The #GstElement flags that a basesrc element may have.
 */
typedef enum {
  GST_BASE_IDLE_SRC_FLAG_STARTING     = (GST_ELEMENT_FLAG_LAST << 0),
  GST_BASE_IDLE_SRC_FLAG_STARTED      = (GST_ELEMENT_FLAG_LAST << 1),
  /* padding */
  GST_BASE_IDLE_SRC_FLAG_LAST         = (GST_ELEMENT_FLAG_LAST << 6)
} GstBaseIdleSrcFlags;

#define GST_BASE_IDLE_SRC_IS_STARTING(obj) GST_OBJECT_FLAG_IS_SET ((obj), GST_BASE_IDLE_SRC_FLAG_STARTING)
#define GST_BASE_IDLE_SRC_IS_STARTED(obj)  GST_OBJECT_FLAG_IS_SET ((obj), GST_BASE_IDLE_SRC_FLAG_STARTED)

typedef struct _GstBaseIdleSrc GstBaseIdleSrc;
typedef struct _GstBaseIdleSrcClass GstBaseIdleSrcClass;
typedef struct _GstBaseIdleSrcPrivate GstBaseIdleSrcPrivate;

/**
 * GST_BASE_IDLE_SRC_PAD:
 * @obj: base source instance
 *
 * Gives the pointer to the #GstPad object of the element.
 */
#define GST_BASE_IDLE_SRC_PAD(obj)                 (GST_BASE_IDLE_SRC_CAST (obj)->srcpad)


/**
 * GstBaseIdleSrc:
 *
 * The opaque #GstBaseIdleSrc data structure.
 */
struct _GstBaseIdleSrc {
  GstElement     element;

  /*< protected >*/
  GstPad        *srcpad;

  /* available to subclass implementations */
  /* MT-protected (with LIVE_LOCK) */
  GMutex         live_lock;
  GCond          live_cond;
  gboolean       is_live;
  gboolean       live_running;

  /* MT-protected (with LOCK) */
  guint          blocksize;     /* size of buffers when operating push based */
  gboolean       can_activate_push;     /* some scheduling properties */
  gboolean       random_access;

  GstClockID     clock_id;      /* for syncing */

  /* MT-protected (with STREAM_LOCK *and* OBJECT_LOCK) */
  GstSegment     segment;
  /* MT-protected (with STREAM_LOCK) */
  gboolean       need_newsegment;

  gint           num_buffers;
  gint           num_buffers_left;

#ifndef GST_REMOVE_DEPRECATED
  gboolean       typefind;      /* unused */
#endif

  gboolean       running;
  GstEvent      *pending_seek;

  GstBaseIdleSrcPrivate *priv;

  /*< private >*/
  gpointer       _gst_reserved[GST_PADDING_LARGE];
};

/**
 * GstBaseIdleSrcClass:
 * @parent_class: Element parent class
 * @get_caps: Called to get the caps to report
 * @negotiate: Negotiated the caps with the peer.
 * @fixate: Called during negotiation if caps need fixating. Implement instead of
 *   setting a fixate function on the source pad.
 * @set_caps: Notify subclass of changed output caps
 * @decide_allocation: configure the allocation query
 * @start: Start processing. Subclasses should open resources and prepare
 *    to produce data. Implementation should call gst_base_idle_src_start_complete()
 *    when the operation completes, either from the current thread or any other
 *    thread that finishes the start operation asynchronously.
 * @stop: Stop processing. Subclasses should use this to close resources.
 * @get_times: Given a buffer, return the start and stop time when it
 *    should be pushed out. The base class will sync on the clock using
 *    these times.
 * @get_size: Return the total size of the resource, in the format set by
 *     gst_base_idle_src_set_format().
 * @is_seekable: Check if the source can seek
 * @prepare_seek_segment: Prepare the #GstSegment that will be passed to the
 *   #GstBaseIdleSrcClass::do_seek vmethod for executing a seek
 *   request. Sub-classes should override this if they support seeking in
 *   formats other than the configured native format. By default, it tries to
 *   convert the seek arguments to the configured native format and prepare a
 *   segment in that format.
 * @do_seek: Perform seeking on the resource to the indicated segment.
 * @unlock: Unlock any pending access to the resource. Subclasses should unblock
 *    any blocked function ASAP. In particular, any `create()` function in
 *    progress should be unblocked and should return GST_FLOW_FLUSHING. Any
 *    future #GstBaseIdleSrcClass::create function call should also return
 *    GST_FLOW_FLUSHING until the #GstBaseIdleSrcClass::unlock_stop function has
 *    been called.
 * @unlock_stop: Clear the previous unlock request. Subclasses should clear any
 *    state they set during #GstBaseIdleSrcClass::unlock, such as clearing command
 *    queues.
 * @query: Handle a requested query.
 * @event: Override this to implement custom event handling.
 * @create: Ask the subclass to create a buffer with offset and size.  When the
 *   subclass returns GST_FLOW_OK, it MUST return a buffer of the requested size
 *   unless fewer bytes are available because an EOS condition is near. No
 *   buffer should be returned when the return value is different from
 *   GST_FLOW_OK. A return value of GST_FLOW_EOS signifies that the end of
 *   stream is reached. The default implementation will call
 *   #GstBaseIdleSrcClass::alloc and then call #GstBaseIdleSrcClass::fill.
 * @alloc: Ask the subclass to allocate a buffer with for offset and size. The
 *   default implementation will create a new buffer from the negotiated allocator.
 * @fill: Ask the subclass to fill the buffer with data for offset and size. The
 *   passed buffer is guaranteed to hold the requested amount of bytes.
 *
 * Subclasses can override any of the available virtual methods or not, as
 * needed. At the minimum, the @create method should be overridden to produce
 * buffers.
 */
struct _GstBaseIdleSrcClass {
  GstElementClass parent_class;

  /*< public >*/
  /* virtual methods for subclasses */

  /**
   * GstBaseIdleSrcClass::get_caps:
   * @filter: (in) (nullable):
   *
   * Called to get the caps to report.
   */
  GstCaps*      (*get_caps)     (GstBaseIdleSrc *src, GstCaps *filter);
  /* decide on caps */
  gboolean      (*negotiate)    (GstBaseIdleSrc *src);
  /* called if, in negotiation, caps need fixating */
  GstCaps *     (*fixate)       (GstBaseIdleSrc *src, GstCaps *caps);
  /* notify the subclass of new caps */
  gboolean      (*set_caps)     (GstBaseIdleSrc *src, GstCaps *caps);

  /* setup allocation query */
  gboolean      (*decide_allocation)   (GstBaseIdleSrc *src, GstQuery *query);

  /* start and stop processing, ideal for opening/closing the resource */
  gboolean      (*start)        (GstBaseIdleSrc *src);
  gboolean      (*stop)         (GstBaseIdleSrc *src);

  /**
   * GstBaseIdleSrcClass::get_times:
   * @start: (out):
   * @end: (out):
   *
   * Given @buffer, return @start and @end time when it should be pushed
   * out. The base class will sync on the clock using these times.
   */
  void          (*get_times)    (GstBaseIdleSrc *src, GstBuffer *buffer,
                                 GstClockTime *start, GstClockTime *end);

  /**
   * GstBaseIdleSrcClass::get_size:
   * @size: (out):
   *
   * Get the total size of the resource in the format set by
   * gst_base_idle_src_set_format().
   *
   * Returns: %TRUE if the size is available and has been set.
   */
  gboolean      (*get_size)     (GstBaseIdleSrc *src, guint64 *size);

  /* check if the resource is seekable */
  gboolean      (*is_seekable)  (GstBaseIdleSrc *src);

  /* Prepare the segment on which to perform do_seek(), converting to the
   * current basesrc format. */
  gboolean      (*prepare_seek_segment) (GstBaseIdleSrc *src, GstEvent *seek,
                                         GstSegment *segment);
  /* notify subclasses of a seek */
  gboolean      (*do_seek)      (GstBaseIdleSrc *src, GstSegment *segment);

  /* unlock any pending access to the resource. subclasses should unlock
   * any function ASAP. */
  gboolean      (*unlock)       (GstBaseIdleSrc *src);
  /* Clear any pending unlock request, as we succeeded in unlocking */
  gboolean      (*unlock_stop)  (GstBaseIdleSrc *src);

  /* notify subclasses of a query */
  gboolean      (*query)        (GstBaseIdleSrc *src, GstQuery *query);

  /* notify subclasses of an event */
  gboolean      (*event)        (GstBaseIdleSrc *src, GstEvent *event);

  /**
   * GstBaseIdleSrcClass::create:
   * @buf: (inout):
   *
   * Ask the subclass to create a buffer with @offset and @size, the default
   * implementation will call alloc if no allocated @buf is provided and then call fill.
   */
  GstFlowReturn (*create)       (GstBaseIdleSrc *src, guint64 offset, guint size,
                                 GstBuffer **buf);
  /**
   * GstBaseIdleSrcClass::alloc:
   * @buf: (out):
   *
   * Ask the subclass to allocate an output buffer with @offset and @size, the default
   * implementation will use the negotiated allocator.
   */
  GstFlowReturn (*alloc)        (GstBaseIdleSrc *src, guint64 offset, guint size,
                                 GstBuffer **buf);
  /* ask the subclass to fill the buffer with data from offset and size */
  GstFlowReturn (*fill)         (GstBaseIdleSrc *src, guint64 offset, guint size,
                                 GstBuffer *buf);

  /*< private >*/
  gpointer       _gst_reserved[GST_PADDING_LARGE];
};

GST_BASE_API
GType           gst_base_idle_src_get_type (void);

GST_BASE_API
GstFlowReturn   gst_base_idle_src_wait_playing     (GstBaseIdleSrc *src);

GST_BASE_API
void            gst_base_idle_src_set_live         (GstBaseIdleSrc *src, gboolean live);

GST_BASE_API
gboolean        gst_base_idle_src_is_live          (GstBaseIdleSrc *src);

GST_BASE_API
void            gst_base_idle_src_set_format       (GstBaseIdleSrc *src, GstFormat format);

GST_BASE_API
void            gst_base_idle_src_set_dynamic_size (GstBaseIdleSrc * src, gboolean dynamic);

GST_BASE_API
void            gst_base_idle_src_set_automatic_eos (GstBaseIdleSrc * src, gboolean automatic_eos);

GST_BASE_API
void            gst_base_idle_src_set_async        (GstBaseIdleSrc *src, gboolean async);

GST_BASE_API
gboolean        gst_base_idle_src_is_async         (GstBaseIdleSrc *src);

GST_BASE_API
gboolean        gst_base_idle_src_negotiate        (GstBaseIdleSrc *src);

GST_BASE_API
void            gst_base_idle_src_start_complete   (GstBaseIdleSrc * basesrc, GstFlowReturn ret);

GST_BASE_API
GstFlowReturn   gst_base_idle_src_start_wait       (GstBaseIdleSrc * basesrc);

GST_BASE_API
gboolean        gst_base_idle_src_query_latency    (GstBaseIdleSrc *src, gboolean * live,
                                               GstClockTime * min_latency,
                                               GstClockTime * max_latency);
GST_BASE_API
void            gst_base_idle_src_set_blocksize    (GstBaseIdleSrc *src, guint blocksize);

GST_BASE_API
guint           gst_base_idle_src_get_blocksize    (GstBaseIdleSrc *src);

GST_BASE_API
void            gst_base_idle_src_set_do_timestamp (GstBaseIdleSrc *src, gboolean timestamp);

GST_BASE_API
gboolean        gst_base_idle_src_get_do_timestamp (GstBaseIdleSrc *src);

GST_BASE_API
gboolean        gst_base_idle_src_new_seamless_segment (GstBaseIdleSrc *src, gint64 start, gint64 stop, gint64 time);

GST_BASE_API
gboolean        gst_base_idle_src_new_segment      (GstBaseIdleSrc *src,
                                               const GstSegment * segment);

GST_BASE_API
gboolean        gst_base_idle_src_set_caps         (GstBaseIdleSrc *src, GstCaps *caps);

GST_BASE_API
GstBufferPool * gst_base_idle_src_get_buffer_pool  (GstBaseIdleSrc *src);

GST_BASE_API
void            gst_base_idle_src_get_allocator    (GstBaseIdleSrc *src,
                                               GstAllocator **allocator,
                                               GstAllocationParams *params);

GST_BASE_API
void            gst_base_idle_src_submit_buffer_list (GstBaseIdleSrc    * src,
                                                 GstBufferList * buffer_list);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GstBaseIdleSrc, gst_object_unref)

G_END_DECLS

#endif /* __GST_BASE_IDLE_SRC_H__ */
