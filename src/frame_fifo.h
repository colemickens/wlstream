#include <stdbool.h>
#include <pthread.h>
#include <libavutil/frame.h>

typedef struct AVFrameFIFO {
    AVFrame **queued_frames;
    int num_queued_frames;
    int max_queued_frames;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_mutex_t cond_lock;
} AVFrameFIFO;

static inline int init_fifo(AVFrameFIFO *buf, int max_queued_frames)
{
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->cond, NULL);
    pthread_mutex_init(&buf->cond_lock, NULL);
    buf->num_queued_frames = 0;
    buf->max_queued_frames = max_queued_frames;
    buf->queued_frames = av_mallocz(buf->max_queued_frames * sizeof(AVFrame *));
    return !buf->queued_frames ? AVERROR(ENOMEM) : 0;
}

static inline int get_fifo_size(AVFrameFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);
    int ret = buf->num_queued_frames;
    pthread_mutex_unlock(&buf->lock);
    return ret;
}

static inline bool push_to_fifo(AVFrameFIFO *buf, AVFrame *f)
{
	bool ret;
    pthread_mutex_lock(&buf->lock);
    if ((buf->num_queued_frames + 1) > buf->max_queued_frames) {
        av_frame_free(&f);
        ret = true;
    } else {
        buf->queued_frames[buf->num_queued_frames++] = f;
        ret = false;
    }
    pthread_mutex_unlock(&buf->lock);
    pthread_cond_signal(&buf->cond);
    return ret;
}

static inline AVFrame *pop_from_fifo(AVFrameFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued_frames) {
        pthread_mutex_unlock(&buf->lock);
        pthread_mutex_lock(&buf->cond_lock);
        pthread_cond_wait(&buf->cond, &buf->cond_lock);
        pthread_mutex_unlock(&buf->cond_lock);
        pthread_mutex_lock(&buf->lock);
    }

    int i;
    AVFrame *rf = buf->queued_frames[0];
    for (i = 1; i < buf->num_queued_frames; i++)
        buf->queued_frames[i - 1] = buf->queued_frames[i];
    buf->queued_frames[i - 1] = NULL;

    buf->num_queued_frames--;

    pthread_mutex_unlock(&buf->lock);
    return rf;
}

static inline void free_fifo(AVFrameFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);
    if (buf->num_queued_frames)
        for (int i = 0; i < buf->num_queued_frames; i++)
            av_frame_free(&buf->queued_frames[i]);
    av_freep(&buf->queued_frames);
    pthread_mutex_unlock(&buf->lock);
}
