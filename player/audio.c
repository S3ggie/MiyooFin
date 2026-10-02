/*
 * miyoofin-audio: the audio engine behind MiyooFin Music.
 *
 * A small stand-alone process (FFmpeg 2.4 + SDL 1.2, the same device libraries as
 * miyoofin-player) that plays LOCAL files. It has no network or TLS code: the app downloads
 * tracks into a cache first. The app talks to it over stdin/stdout, one line per message.
 *
 *   commands (stdin)                     meaning
 *     load <seconds> <path>               play <path> now, starting <seconds> in
 *     next <path>                         the track to start (gaplessly) when the current ends
 *     nonext                              forget the queued next track
 *     pause | resume                      pause / resume output
 *     seek <seconds>                      jump within the current track
 *     stop                                stop and go idle
 *     quit                                exit
 *
 *   events (stdout)
 *     ready
 *     started <path>                      this track is now audible (also on a gapless switch)
 *     pos <seconds> <duration> <paused>   twice a second while a track plays
 *     ended <path>                        the track played to its end
 *     idle                                nothing left to play
 *     error <path>                        the file could not be opened or decoded
 *
 * Output is always 44.1 kHz stereo 16-bit. Decoding runs in its own thread and feeds a ring
 * buffer; markers in the stream say where each track begins so events fire when the audio
 * is actually heard, not when it is decoded.
 */
#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <SDL.h>
#include <SDL_thread.h>

#define OUT_RATE 44100
#define OUT_BPS (OUT_RATE * 4) /* bytes per second: 2 channels x 16 bit */
#define RING_BYTES (1 << 20)   /* ~6 s of audio */
#define MAX_MARKERS 8
#define PATH_MAX_LEN 1024

typedef struct Marker {
    int64_t begin;   /* ring offset at which this track becomes audible */
    int64_t base;    /* position in the track = (played - base) / OUT_BPS */
    double duration; /* seconds; < 0 marks the idle point after the last track */
    int natural;     /* it followed the previous track by itself (not a load or seek) */
    unsigned serial;
    char path[PATH_MAX_LEN];
} Marker;
static unsigned marker_serial;

static SDL_mutex *mu; /* guards everything below */
static SDL_cond *space_cond;
static uint8_t ring[RING_BYTES];
static int64_t decoded_total, played_total; /* bytes ever written to / read from the ring */
static Marker markers[MAX_MARKERS];
static int n_markers;
static char next_path[PATH_MAX_LEN]; /* queued next track */
static char want_path[PATH_MAX_LEN]; /* load request for the decoder thread */
static double want_start;
static int want_load, want_seek, quit_flag, paused;
static double want_seek_to;
static volatile int decoder_idle = 1;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...)
{
    va_list ap;
    flockfile(stdout); /* the decoder and control threads both report */
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    funlockfile(stdout);
}

/* ---- audio callback: drain the ring, silence on underrun --------------------------------- */
static void audio_cb(void *ud, Uint8 *stream, int len)
{
    int got = 0;
    (void)ud;
    SDL_LockMutex(mu);
    int64_t avail = decoded_total - played_total;
    got = avail < len ? (int)avail : len;
    if (got > 0) {
        int start = (int)(played_total % RING_BYTES), first = RING_BYTES - start;
        if (first > got)
            first = got;
        memcpy(stream, ring + start, (size_t)first);
        if (got > first)
            memcpy(stream + first, ring, (size_t)(got - first));
        played_total += got;
    }
    SDL_CondSignal(space_cond);
    SDL_UnlockMutex(mu);
    if (got < len)
        memset(stream + got, 0, (size_t)(len - got));
}

/* ---- decoder ------------------------------------------------------------------------------ */
typedef struct Source {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    int stream;
    double duration;
    char path[PATH_MAX_LEN];
} Source;

static void source_close(Source *s)
{
    if (s->swr)
        swr_free(&s->swr);
    if (s->dec)
        avcodec_close(s->dec);
    if (s->fmt)
        avformat_close_input(&s->fmt);
    memset(s, 0, sizeof(*s));
}

static int source_open(Source *s, const char *path, double start)
{
    AVCodec *codec = NULL;
    memset(s, 0, sizeof(*s));
    snprintf(s->path, sizeof(s->path), "%s", path);
    if (avformat_open_input(&s->fmt, path, NULL, NULL) < 0)
        goto fail;
    if (avformat_find_stream_info(s->fmt, NULL) < 0)
        goto fail;
    s->stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (s->stream < 0 || !codec)
        goto fail;
    s->dec = s->fmt->streams[s->stream]->codec;
    if (avcodec_open2(s->dec, codec, NULL) < 0) {
        s->dec = NULL;
        goto fail;
    }
    s->duration = s->fmt->duration > 0 ? (double)s->fmt->duration / AV_TIME_BASE : 0;
    if (start > 0.5)
        av_seek_frame(s->fmt, -1, (int64_t)(start * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    {
        int64_t layout = s->dec->channel_layout ? (int64_t)s->dec->channel_layout
                                                : av_get_default_channel_layout(s->dec->channels);
        s->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, OUT_RATE,
                                    layout, s->dec->sample_fmt, s->dec->sample_rate, 0, NULL);
        if (!s->swr || swr_init(s->swr) < 0)
            goto fail;
    }
    return 0;
fail:
    source_close(s);
    return -1;
}

/* Blocks until the ring has room, then appends. Returns 0, or -1 when told to stop/reload. */
static int ring_write(const uint8_t *data, int len)
{
    while (len > 0) {
        int chunk, start;
        SDL_LockMutex(mu);
        while (!quit_flag && !want_load && !want_seek &&
               RING_BYTES - (decoded_total - played_total) < 4096)
            SDL_CondWaitTimeout(space_cond, mu, 50);
        if (quit_flag || want_load || want_seek) {
            SDL_UnlockMutex(mu);
            return -1;
        }
        chunk = RING_BYTES - (int)(decoded_total - played_total);
        if (chunk > len)
            chunk = len;
        start = (int)(decoded_total % RING_BYTES);
        if (RING_BYTES - start < chunk)
            chunk = RING_BYTES - start;
        memcpy(ring + start, data, (size_t)chunk);
        decoded_total += chunk;
        SDL_UnlockMutex(mu);
        data += chunk;
        len -= chunk;
    }
    return 0;
}

/* Caller holds mu. `start` is how far into the track the audio begins. */
static void add_marker(const char *path, double duration, double start, int natural)
{
    Marker *m;
    if (n_markers == MAX_MARKERS) { /* drop the oldest */
        memmove(markers, markers + 1, sizeof(Marker) * (MAX_MARKERS - 1));
        n_markers--;
    }
    m = &markers[n_markers++];
    m->begin = decoded_total;
    m->base = decoded_total - (int64_t)(start * OUT_BPS);
    m->duration = duration;
    m->natural = natural;
    m->serial = ++marker_serial;
    snprintf(m->path, sizeof(m->path), "%s", path ? path : "");
}

/* Decodes one packet's worth into the ring. 1 = more, 0 = end of file, -1 = interrupted. */
static int decode_step(Source *s, AVFrame *frame)
{
    AVPacket pkt;
    int got;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    if (av_read_frame(s->fmt, &pkt) < 0)
        return 0;
    if (pkt.stream_index == s->stream) {
        AVPacket cur = pkt;
        while (cur.size > 0) {
            int used = avcodec_decode_audio4(s->dec, frame, &got, &cur);
            if (used < 0)
                break;
            if (got) {
                int out_samples = (int)av_rescale_rnd(
                    swr_get_delay(s->swr, s->dec->sample_rate) + frame->nb_samples, OUT_RATE,
                    s->dec->sample_rate, AV_ROUND_UP);
                uint8_t *buf = malloc((size_t)out_samples * 4 + 64);
                int n;
                if (!buf) {
                    av_free_packet(&pkt);
                    return 0;
                }
                n = swr_convert(s->swr, &buf, out_samples, (const uint8_t **)frame->extended_data,
                                frame->nb_samples);
                if (n > 0 && ring_write(buf, n * 4) < 0) {
                    free(buf);
                    av_free_packet(&pkt);
                    return -1;
                }
                free(buf);
            }
            cur.data += used;
            cur.size -= used;
        }
    }
    av_free_packet(&pkt);
    return 1;
}

static int decoder_main(void *arg)
{
    Source src;
    AVFrame *frame = av_frame_alloc();
    int active = 0;
    (void)arg;
    memset(&src, 0, sizeof(src));
    for (;;) {
        char path[PATH_MAX_LEN];
        double start;
        int load, seek;
        double seek_to;

        SDL_LockMutex(mu);
        if (quit_flag)
            break;
        load = want_load;
        seek = want_seek;
        seek_to = want_seek_to;
        snprintf(path, sizeof(path), "%s", want_path);
        start = want_start;
        if (load) {
            /* Empty the ring: anything not yet played is dropped. */
            decoded_total = played_total;
            n_markers = 0;
            want_load = 0;
            want_seek = 0;
        } else if (seek) {
            want_seek = 0;
        }
        if (!load && !seek && !active) {
            decoder_idle = 1;
            SDL_CondWaitTimeout(space_cond, mu, 50);
            SDL_UnlockMutex(mu);
            continue;
        }
        SDL_UnlockMutex(mu);

        if (load) {
            source_close(&src);
            active = 0;
            if (!path[0]) { /* stop */
                SDL_LockMutex(mu);
                add_marker(NULL, -1, 0, 0);
                SDL_UnlockMutex(mu);
            } else if (source_open(&src, path, start) == 0) {
                SDL_LockMutex(mu);
                add_marker(src.path, src.duration, start > 0.5 ? start : 0, 0);
                decoder_idle = 0;
                SDL_UnlockMutex(mu);
                active = 1;
            } else {
                emit("error %s", path);
            }
            continue;
        }
        if (seek && active) {
            /* Drop buffered audio and continue from the new spot. The marker is rewritten so
             * position = (played - offset)/rate + seek_to still reads correctly. */
            av_seek_frame(src.fmt, -1, (int64_t)(seek_to * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(src.dec);
            SDL_LockMutex(mu);
            decoded_total = played_total;
            n_markers = 0;
            add_marker(src.path, src.duration, seek_to, 0);
            SDL_UnlockMutex(mu);
            continue;
        }
        if (!active)
            continue;

        {
            int r = decode_step(&src, frame);
            if (r == 0) {
                /* End of this track: roll straight into the queued one (gapless). */
                char next[PATH_MAX_LEN];
                SDL_LockMutex(mu);
                snprintf(next, sizeof(next), "%s", next_path);
                next_path[0] = 0;
                SDL_UnlockMutex(mu);
                source_close(&src);
                active = 0;
                if (next[0] && source_open(&src, next, 0) == 0) {
                    SDL_LockMutex(mu);
                    add_marker(src.path, src.duration, 0, 1);
                    SDL_UnlockMutex(mu);
                    active = 1;
                } else {
                    if (next[0])
                        emit("error %s", next);
                    SDL_LockMutex(mu);
                    add_marker(NULL, -1, 0, 1); /* idle point after the last track */
                    SDL_UnlockMutex(mu);
                }
            }
        }
    }
    SDL_UnlockMutex(mu);
    source_close(&src);
    av_frame_free(&frame);
    return 0;
}

/* ---- control loop ------------------------------------------------------------------------- */
static void handle_line(char *line)
{
    char *arg;
    line[strcspn(line, "\r\n")] = 0;
    arg = strchr(line, ' ');
    if (arg)
        *arg++ = 0;
    if (!strcmp(line, "load") && arg) {
        double start = strtod(arg, &arg);
        while (*arg == ' ')
            arg++;
        SDL_LockMutex(mu);
        snprintf(want_path, sizeof(want_path), "%s", arg);
        want_start = start;
        want_load = 1;
        next_path[0] = 0;
        paused = 0;
        SDL_CondSignal(space_cond);
        SDL_UnlockMutex(mu);
        SDL_PauseAudio(0);
    } else if (!strcmp(line, "next") && arg) {
        SDL_LockMutex(mu);
        snprintf(next_path, sizeof(next_path), "%s", arg);
        SDL_UnlockMutex(mu);
    } else if (!strcmp(line, "nonext")) {
        SDL_LockMutex(mu);
        next_path[0] = 0;
        SDL_UnlockMutex(mu);
    } else if (!strcmp(line, "pause")) {
        paused = 1;
        SDL_PauseAudio(1);
    } else if (!strcmp(line, "resume")) {
        paused = 0;
        SDL_PauseAudio(0);
    } else if (!strcmp(line, "seek") && arg) {
        SDL_LockMutex(mu);
        want_seek_to = strtod(arg, NULL);
        if (want_seek_to < 0)
            want_seek_to = 0;
        want_seek = 1;
        SDL_CondSignal(space_cond);
        SDL_UnlockMutex(mu);
    } else if (!strcmp(line, "stop")) {
        SDL_LockMutex(mu);
        want_path[0] = 0;
        want_load = 0;
        decoded_total = played_total;
        n_markers = 0;
        next_path[0] = 0;
        SDL_UnlockMutex(mu);
        /* An empty load makes the decoder drop its source. */
        SDL_LockMutex(mu);
        want_load = 1;
        SDL_CondSignal(space_cond);
        SDL_UnlockMutex(mu);
    } else if (!strcmp(line, "quit")) {
        quit_flag = 1;
    }
}

int main(int argc, char **argv)
{
    SDL_AudioSpec want, have;
    SDL_Thread *decoder;
    char inbuf[2048];
    size_t inlen = 0;
    char reported[PATH_MAX_LEN] = "";
    int reported_valid = 0, idle_reported = 1;
    unsigned reported_serial = 0;
    Uint32 last_pos_ms = 0;
    (void)argc;
    (void)argv;

    /* Hygiene for a child of the UI app: no inherited descriptors (display, input devices,
     * sockets), and never outlive the app, whatever way it dies. */
    {
        int fd;
        for (fd = 3; fd < 1024; fd++)
            close(fd);
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        return 0; /* the app already went away */
    signal(SIGPIPE, SIG_IGN);

    av_register_all();
    avformat_network_init();
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    mu = SDL_CreateMutex();
    space_cond = SDL_CreateCond();
    memset(&want, 0, sizeof(want));
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    want.callback = audio_cb;
    if (SDL_OpenAudio(&want, &have) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return 3;
    }
    if (have.freq != OUT_RATE || have.channels != 2 || have.format != AUDIO_S16SYS)
        fprintf(stderr, "warning: audio device gave %d Hz %d ch fmt %d\n", have.freq,
                have.channels, have.format);
    decoder = SDL_CreateThread(decoder_main, NULL);
    SDL_PauseAudio(0);
    emit("ready");

    while (!quit_flag) {
        struct pollfd pfd = {0, POLLIN, 0};
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(0, inbuf + inlen, sizeof(inbuf) - inlen - 1);
            if (n <= 0) { /* the app went away */
                quit_flag = 1;
                break;
            }
            inlen += (size_t)n;
            for (;;) {
                char *nl = memchr(inbuf, '\n', inlen);
                size_t len;
                if (!nl) {
                    if (inlen >= sizeof(inbuf) - 1)
                        inlen = 0; /* overlong line: discard */
                    break;
                }
                *nl = 0;
                len = (size_t)(nl - inbuf) + 1;
                handle_line(inbuf);
                memmove(inbuf, inbuf + len, inlen - len);
                inlen -= len;
            }
        }

        /* Events: work out which marker is audible. */
        {
            char head_path[PATH_MAX_LEN] = "";
            double head_pos = 0, head_dur = 0;
            int have_head = 0, idle_now = 0, head_natural = 0;
            unsigned head_serial = 0;
            SDL_LockMutex(mu);
            {
                int i;
                for (i = 0; i < n_markers; i++) {
                    if (markers[i].begin > played_total)
                        break;
                    head_natural = markers[i].natural;
                    head_serial = markers[i].serial;
                    if (markers[i].duration < 0) {
                        idle_now = 1;
                        have_head = 0;
                    } else {
                        idle_now = 0;
                        have_head = 1;
                        snprintf(head_path, sizeof(head_path), "%s", markers[i].path);
                        head_pos = (double)(played_total - markers[i].base) / OUT_BPS;
                        head_dur = markers[i].duration;
                    }
                }
                /* Forget markers behind the head except the one in use. */
                while (n_markers > 1 && markers[1].begin <= played_total) {
                    memmove(markers, markers + 1, sizeof(Marker) * (size_t)(n_markers - 1));
                    n_markers--;
                }
            }
            SDL_UnlockMutex(mu);
            if (have_head && (!reported_valid || head_serial != reported_serial)) {
                if (reported_valid && head_natural)
                    emit("ended %s", reported);
                emit("started %s", head_path);
                snprintf(reported, sizeof(reported), "%s", head_path);
                reported_serial = head_serial;
                reported_valid = 1;
                idle_reported = 0;
            } else if (idle_now && !idle_reported && head_serial != reported_serial) {
                if (reported_valid && head_natural)
                    emit("ended %s", reported);
                emit("idle");
                reported_serial = head_serial;
                reported_valid = 0;
                idle_reported = 1;
            }
            if (have_head && SDL_GetTicks() - last_pos_ms >= 500) {
                last_pos_ms = SDL_GetTicks();
                emit("pos %.1f %.1f %d", head_pos < 0 ? 0 : head_pos, head_dur, paused);
            }
        }
    }

    SDL_LockMutex(mu);
    quit_flag = 1;
    SDL_CondSignal(space_cond);
    SDL_UnlockMutex(mu);
    SDL_WaitThread(decoder, NULL);
    SDL_CloseAudio();
    SDL_Quit();
    return 0;
}
