#include "frame_fifo.h"
#include "src_common.h"
#include <stdatomic.h>
#include <libavutil/channel_layout.h>
#include <pulse/pulseaudio.h>

typedef struct PulseCtx {
    AVClass *class;
    AVFrameFIFO *fifo;

    pa_sample_spec pa_spec;
    pa_context *pa_context;
    pa_threaded_mainloop *pa_mainloop;
    pa_mainloop_api *pa_mainloop_api;

    atomic_bool quit;
} PulseCtx;

static void pulse_stream_read_cb(pa_stream *stream, size_t size,
                                 void *data)
{
    PulseCtx *ctx = data;

    const void *buffer;
    pa_stream_peek(stream, &buffer, &size);

    AVFrame *f = av_frame_alloc();
    f->sample_rate    = ctx->pa_spec.rate;
    f->format         = AV_SAMPLE_FMT_FLT;
    f->channel_layout = AV_CH_LAYOUT_STEREO;
    f->nb_samples     = size >> 3;

    /* I can't believe these bastards don't output aligned data. Let's hope the
     * ability to have writeable refcounted frames is worth it elsewhere. */
    av_frame_get_buffer(f, 0);
    memcpy(f->data[0], buffer, size);

    int delay_neg;
    pa_usec_t pts, delay;
    if (!pa_stream_get_time(stream, &pts) &&
        !pa_stream_get_latency(stream, &delay, &delay_neg)) {
        f->pts = pts;
        f->pts -= delay_neg ? -delay : delay;
    } else {
        f->pts = INT64_MIN;
    }

    pa_stream_drop(stream);

    if (push_to_fifo(ctx->fifo, f))
        av_log(ctx, AV_LOG_WARNING, "Dropped audio frame!\n");

    if (atomic_load(&ctx->quit))
        pa_stream_disconnect(stream);
}

static void pulse_stream_status_cb(pa_stream *stream, void *data)
{
    PulseCtx *ctx = data;
    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_UNCONNECTED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream currently unconnected!\n");
        break;
    case PA_STREAM_CREATING:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream being created!\n");
        break;
    case PA_STREAM_READY:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream now ready!\n");
        break;
    case PA_STREAM_FAILED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream connection failed!\n");
        break;
    case PA_STREAM_TERMINATED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream terminated!\n");
        pa_context_disconnect(ctx->pa_context);
        break;
    }
}

static void pulse_stream_buffer_attr_cb(pa_stream *s, void *data)
{
    const struct pa_buffer_attr *att = pa_stream_get_buffer_attr(s);
    av_log(data, AV_LOG_WARNING, "Using buffer pa buffer attrs: "
           "fragsize=%i, maxlen=%i\n", att->fragsize, att->maxlength);
}

static void pulse_stream_overflow_cb(pa_stream *stream, void *data)
{
    av_log(data, AV_LOG_ERROR, "PulseAudio stream overflowed!\n");
}

static void pulse_stream_underflow_cb(pa_stream *stream, void *data)
{
    av_log(data, AV_LOG_ERROR, "PulseAudio stream underflowed!\n");
}

static void pulse_sink_info_cb(pa_context *context, const pa_sink_info *info,
                               int is_last, void *data)
{
    PulseCtx *ctx = data;

    if (is_last)
        return;

    av_log(ctx, AV_LOG_INFO, "Starting capturing from \"%s\"\n",
           info->description);

    pa_stream *stream;
    pa_buffer_attr attr = { 0 };
    pa_channel_map map  = { 0 };

    ctx->pa_spec.format   = PA_SAMPLE_FLOAT32LE;
    ctx->pa_spec.rate     = 44100;
    ctx->pa_spec.channels = 2;
    pa_channel_map_init_stereo(&map);

    attr.maxlength = -1;
    attr.fragsize  = -1;

    stream = pa_stream_new(context, "wlstream", &ctx->pa_spec, &map);

    /* Set stream callbacks */
    pa_stream_set_state_callback(stream, pulse_stream_status_cb, ctx);
    pa_stream_set_read_callback(stream, pulse_stream_read_cb, ctx);
    pa_stream_set_underflow_callback(stream, pulse_stream_underflow_cb, ctx);
    pa_stream_set_overflow_callback(stream, pulse_stream_overflow_cb, ctx);
    pa_stream_set_buffer_attr_callback(stream, pulse_stream_buffer_attr_cb, ctx);

    /* Start stream */
    pa_stream_connect_record(stream, info->monitor_source_name, &attr,
                             PA_STREAM_ADJUST_LATENCY     |
                             PA_STREAM_AUTO_TIMING_UPDATE |
                             PA_STREAM_INTERPOLATE_TIMING |
                             PA_STREAM_FIX_RATE           |
                             PA_STREAM_FIX_CHANNELS       |
                             PA_STREAM_START_UNMUTED);
}

static void pulse_server_info_cb(pa_context *context, const pa_server_info *info,
                                 void *data)
{
    PulseCtx *ctx = data;

    pa_operation *op;
    op = pa_context_get_sink_info_by_name(context, info->default_sink_name,
                                          pulse_sink_info_cb, ctx);
    pa_operation_unref(op);
}

static void pulse_state_cb(pa_context *context, void *data)
{
    PulseCtx *ctx = data;

    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_UNCONNECTED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio reports it is unconnected!\n");
        break;
    case PA_CONTEXT_CONNECTING:
        av_log(ctx, AV_LOG_INFO, "Connecting to PulseAudio!\n");
        break;
    case PA_CONTEXT_AUTHORIZING:
        av_log(ctx, AV_LOG_INFO, "Authorizing PulseAudio connection!\n");
        break;
    case PA_CONTEXT_SETTING_NAME:
        av_log(ctx, AV_LOG_INFO, "Sending client name!\n");
        break;
    case PA_CONTEXT_FAILED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection failed!\n");
        break;
    case PA_CONTEXT_TERMINATED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection terminated!\n");
        break;
    case PA_CONTEXT_READY:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection ready!\n");
        pa_operation_unref(pa_context_get_server_info(context,
                           pulse_server_info_cb, ctx));
        break;
    }
}

static int init_pulse(void **s, AVFrameFIFO *dst)
{
    PulseCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = "wlstream_pulse",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };
    ctx->quit = ATOMIC_VAR_INIT(0);

    ctx->fifo = dst;
    ctx->pa_mainloop = pa_threaded_mainloop_new();
    ctx->pa_mainloop_api = pa_threaded_mainloop_get_api(ctx->pa_mainloop);

    ctx->pa_context = pa_context_new(ctx->pa_mainloop_api, "wlstream");
    pa_context_set_state_callback(ctx->pa_context, pulse_state_cb, ctx);
    pa_context_connect(ctx->pa_context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);

    pa_threaded_mainloop_start(ctx->pa_mainloop);

    *s = ctx;

    return 0;
}

static void stop_pulse(void *s)
{
    PulseCtx *ctx = s;

    atomic_store(&ctx->quit, true);
}

static void free_pulse(void **s)
{
    PulseCtx *ctx = *s;

    if (ctx->pa_mainloop) {
        pa_threaded_mainloop_stop(ctx->pa_mainloop);
        pa_threaded_mainloop_free(ctx->pa_mainloop);
    }

    if (ctx->pa_context)
        pa_context_unref(ctx->pa_context);

    av_freep(s);
}

const CaptureSource src_pulse = {
    .name = "pulse",
    .init = init_pulse,
    .stop = stop_pulse,
    .free = free_pulse,
};
