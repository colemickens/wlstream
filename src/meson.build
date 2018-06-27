executable(
    'wlstream',
    [ 'wlstream.c', 'src_pulse.c', ],
    install: true,
    dependencies: [wayland_client,
                   wlstream_protos,
                   libavutil,
                   libavcodec,
                   libpulse,
                   libavformat,
                   libswresample,
                   pthreads,
                   libm, ]
)
