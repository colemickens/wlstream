#include <libavutil/mathematics.h>

typedef struct CaptureSource {
    const char *name;
    int  (*init)(void **s, AVFrameFIFO *dst);
    void (*stop)(void  *s);
    void (*free)(void **s);
} CaptureSource;

extern const CaptureSource src_pulse;
