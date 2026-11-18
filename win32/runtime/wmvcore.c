// Windows Media Format SDK (wmvcore.dll) for 32-bit guests: IWMSyncReader on FFmpeg (third_party/ffmpeg, built
// by ffmpeg.mk: the ASF demuxer and the WMV / VC-1 / WMA decoders).
//
// The reader reads the guest's own IStream (OpenStream) or a file (Open) through an AVIOContext, and offers per
// stream the outputs a WMF reader offers: video as YV12 or RGB32 (VIDEOINFOHEADER), audio as 16-bit PCM
// (WAVEFORMATEX). GetNextSample decodes on demand; samples of other outputs read meanwhile wait in per-output
// queues. Samples are INSSBuffer objects over guest memory. IWMHeaderInfo answers "Duration" and "Seekable".
#include "com32.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>

#define S_OK 0
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define NS_E_NO_MORE_SAMPLES 0xC00D0BCFu
#define ASF_E_BUFFERTOOSMALL 0xC00D07D2u
#define ASF_E_NOTFOUND 0xC00D07F0u
#define NS_E_INVALID_OUTPUT_FORMAT 0xC00D0BC3u

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
static const Guid MT_Video = {0x73646976, 0, 0x10, {0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
static const Guid MT_Audio = {0x73647561, 0, 0x10, {0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
static const Guid ST_YV12  = {0x32315659, 0, 0x10, {0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
static const Guid ST_RGB32 = {0xE436EB7E, 0x524F, 0x11CE, {0x9F, 0x53, 0, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
static const Guid ST_PCM   = {0x00000001, 0, 0x10, {0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
static const Guid FT_VideoInfo = {0x05589F80, 0xC356, 0x11CE, {0xBF, 0x01, 0, 0xAA, 0, 0x55, 0x59, 0x5A}};
static const Guid FT_WaveFormatEx = {0x05589F81, 0xC356, 0x11CE, {0xBF, 0x01, 0, 0xAA, 0, 0x55, 0x59, 0x5A}};
static const Guid IID_IWMHeaderInfo = {0x96406BDA, 0x2B2B, 0x11D3, {0xB3, 0x6B, 0, 0xC0, 0x4F, 0x61, 0x08, 0xFF}};

// ---------------- INSSBuffer ----------------
typedef struct { ComObj base; uint32_t buf, len, max; } Sample;
static void sample_destroy(ComObj *o) { gfree(((Sample *)o)->buf); free(o); }
static uint32_t sb_GetLength(CPU *c, ComObj *s, const ComMethod *m) { if (M(1)) wr32(M(1), ((Sample *)s)->len); return S_OK; }
static uint32_t sb_SetLength(CPU *c, ComObj *s, const ComMethod *m) { Sample *b = (Sample *)s; if (M(1) > b->max) return E_INVALIDARG; b->len = M(1); return S_OK; }
static uint32_t sb_GetMaxLength(CPU *c, ComObj *s, const ComMethod *m) { if (M(1)) wr32(M(1), ((Sample *)s)->max); return S_OK; }
static uint32_t sb_GetBuffer(CPU *c, ComObj *s, const ComMethod *m) { if (M(1)) wr32(M(1), ((Sample *)s)->buf); return S_OK; }
static uint32_t sb_GetBufferAndLength(CPU *c, ComObj *s, const ComMethod *m) {
    Sample *b = (Sample *)s;
    if (M(1)) wr32(M(1), b->buf);
    if (M(2)) wr32(M(2), b->len);
    return S_OK;
}
static const ComMethod sample_m[] = { COM_IUNKNOWN,
    {"GetLength", 2, sb_GetLength, 0, 0, 0}, {"SetLength", 2, sb_SetLength, 0, 0, 0}, {"GetMaxLength", 2, sb_GetMaxLength, 0, 0, 0},
    {"GetBuffer", 2, sb_GetBuffer, 0, 0, 0}, {"GetBufferAndLength", 3, sb_GetBufferAndLength, 0, 0, 0} };
static ComClass sample_cls = {"INSSBuffer", sizeof sample_m / sizeof *sample_m, sample_m, 0, sample_destroy};

// ---------------- the reader ----------------
typedef struct Queued { struct Queued *next; uint32_t buf, len; uint64_t time, dur; uint32_t flags; } Queued;
typedef struct {
    int stream;               // FFmpeg stream index
    int video;                // 1 video, 0 audio
    int format;               // video: 0 YV12, 1 RGB32 (the output format the game chose)
    AVCodecContext *dec;
    struct SwsContext *sws;
    SwrContext *swr;
    int width, height, channels, rate;
    Queued *head, *tail;
    int eof;
} Output;
typedef struct {
    ComObj base;
    CPU *cpu;
    uint32_t istream;         // guest IStream (OpenStream) or 0
    FILE *file;               // Open(filename)
    uint32_t io_buf;          // guest buffer for IStream::Read
    AVFormatContext *fmt;
    AVIOContext *avio;
    Output out[8];
    int nout;
    int64_t duration;         // 100 ns
    int ended;
} Reader;

// guest IStream: 3 Read(pv, cb, *pcbRead), 5 Seek(LARGE_INTEGER, origin, ULARGE_INTEGER *), 12 Stat(STATSTG *, flags)
static uint32_t is_call(Reader *r, int slot, int n, const uint32_t *args) {
    uint32_t vt = rd32(r->istream), a[8];
    a[0] = r->istream;
    for (int i = 0; i < n; i++) a[i + 1] = args[i];
    return guest_call(r->cpu, rd32(vt + 4u * (uint32_t)slot), n + 1, a);
}
#define IO_CHUNK 65536
static int io_read(void *opaque, uint8_t *buf, int size) {
    Reader *r = opaque;
    if (r->file) { size_t n = fread(buf, 1, (size_t)size, r->file); return n ? (int)n : AVERROR_EOF; }
    if (size > IO_CHUNK) size = IO_CHUNK;
    uint32_t got = r->io_buf + IO_CHUNK;   // the count goes right after the data buffer
    wr32(got, 0);
    uint32_t args[3] = {r->io_buf, (uint32_t)size, got};
    uint32_t hr = is_call(r, 3, 3, args);
    uint32_t n = rd32(got);
    if ((int32_t)hr < 0 || n == 0) return AVERROR_EOF;
    memcpy(buf, G2H(r->io_buf), n);
    return (int)n;
}
static int64_t io_seek(void *opaque, int64_t off, int whence) {
    Reader *r = opaque;
    if (r->file) {
        if (whence == AVSEEK_SIZE) { long cur = ftell(r->file); fseek(r->file, 0, SEEK_END); long sz = ftell(r->file); fseek(r->file, cur, SEEK_SET); return sz; }
        return fseeko(r->file, off, whence & 3) ? -1 : ftello(r->file);
    }
    uint32_t tmp = r->io_buf + IO_CHUNK + 8;
    if (whence == AVSEEK_SIZE) {   // IStream::Stat: STATSTG.cbSize at +8
        memset(G2H(tmp), 0, 80);
        uint32_t args[2] = {tmp, 1};   // STATFLAG_NONAME
        if ((int32_t)is_call(r, 12, 2, args) < 0) return -1;
        return (int64_t)rd64(tmp + 8);
    }
    uint32_t args[4] = {(uint32_t)off, (uint32_t)((uint64_t)off >> 32), (uint32_t)(whence & 3), tmp};   // STREAM_SEEK_SET/CUR/END match
    if ((int32_t)is_call(r, 5, 4, args) < 0) return -1;
    return (int64_t)rd64(tmp);
}

static void out_flush(Output *o) {
    while (o->head) { Queued *q = o->head; o->head = q->next; gfree(q->buf); free(q); }
    o->tail = NULL; o->eof = 0;
    if (o->dec) avcodec_flush_buffers(o->dec);
}
static void reader_close(Reader *r) {
    for (int i = 0; i < r->nout; i++) {
        out_flush(&r->out[i]);
        avcodec_free_context(&r->out[i].dec);
        if (r->out[i].sws) sws_freeContext(r->out[i].sws);
        swr_free(&r->out[i].swr);
    }
    r->nout = 0;
    if (r->fmt) avformat_close_input(&r->fmt);
    if (r->avio) { av_freep(&r->avio->buffer); avio_context_free(&r->avio); }
    if (r->file) { fclose(r->file); r->file = NULL; }
    if (r->istream) { uint32_t vt = rd32(r->istream); guest_call(r->cpu, rd32(vt + 8), 1, &r->istream); r->istream = 0; }   // Release
    if (r->io_buf) { gfree(r->io_buf); r->io_buf = 0; }
}

static uint32_t reader_open(Reader *r) {
    r->io_buf = gmalloc(IO_CHUNK + 128);
    unsigned char *b = av_malloc(IO_CHUNK);
    r->avio = avio_alloc_context(b, IO_CHUNK, 0, r, io_read, NULL, io_seek);
    r->fmt = avformat_alloc_context();
    r->fmt->pb = r->avio;
    if (avformat_open_input(&r->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(r->fmt, NULL) < 0) {
        rt_log("[wmv] not a readable ASF/WMV stream");
        reader_close(r);
        return E_FAIL;
    }
    r->duration = r->fmt->duration > 0 ? r->fmt->duration * 10 : 0;   // AV_TIME_BASE (us) -> 100 ns
    for (unsigned i = 0; i < r->fmt->nb_streams && r->nout < 8; i++) {
        AVStream *st = r->fmt->streams[i];
        enum AVMediaType t = st->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) { st->discard = AVDISCARD_ALL; continue; }
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) { rt_log("[wmv] no decoder for %s", avcodec_get_name(st->codecpar->codec_id)); st->discard = AVDISCARD_ALL; continue; }
        Output *o = &r->out[r->nout++];
        memset(o, 0, sizeof *o);
        o->stream = (int)i; o->video = t == AVMEDIA_TYPE_VIDEO;
        o->dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(o->dec, st->codecpar);
        o->dec->thread_count = 0;   // automatic frame/slice threads
        if (avcodec_open2(o->dec, codec, NULL) < 0) { rt_log("[wmv] can't open the %s decoder", codec->name); r->nout--; continue; }
        if (o->video) { o->width = st->codecpar->width; o->height = st->codecpar->height; }
        else { o->channels = st->codecpar->ch_layout.nb_channels > 2 ? 2 : st->codecpar->ch_layout.nb_channels; o->rate = st->codecpar->sample_rate; }
        rt_log("[wmv] output %d: stream %d (ASF #%d) %s %s %dx%d %d ch %d Hz", r->nout - 1, i, st->id, o->video ? "video" : "audio",
               codec->name, o->width, o->height, o->channels, o->rate);
    }
    return r->nout ? S_OK : E_FAIL;
}

static void enqueue(Output *o, uint32_t buf, uint32_t len, uint64_t time, uint64_t dur, uint32_t flags) {
    Queued *q = calloc(1, sizeof *q);
    q->buf = buf; q->len = len; q->time = time; q->dur = dur; q->flags = flags;
    if (o->tail) o->tail->next = q; else o->head = q;
    o->tail = q;
}
static uint64_t to_100ns(int64_t ts, AVRational tb) { return ts == AV_NOPTS_VALUE ? 0 : (uint64_t)av_rescale_q(ts, tb, (AVRational){1, 10000000}); }

static void convert(Reader *r, Output *o, AVFrame *f) {
    AVRational tb = r->fmt->streams[o->stream]->time_base;
    uint64_t t = to_100ns(f->best_effort_timestamp, tb), d = to_100ns(f->duration, tb);
    if (o->video) {
        int w = o->width, h = o->height;
        if (o->format == 0) {   // YV12: Y, then V, then U, pitch = width
            uint32_t ysz = (uint32_t)(w * h), csz = (uint32_t)((w / 2) * (h / 2));
            uint32_t buf = gmalloc(ysz + 2 * csz);
            uint8_t *dst = G2H(buf);
            if (f->format == AV_PIX_FMT_YUV420P) {
                for (int y = 0; y < h; y++) memcpy(dst + y * w, f->data[0] + y * f->linesize[0], (size_t)w);
                for (int y = 0; y < h / 2; y++) memcpy(dst + ysz + y * (w / 2), f->data[2] + y * f->linesize[2], (size_t)(w / 2));
                for (int y = 0; y < h / 2; y++) memcpy(dst + ysz + csz + y * (w / 2), f->data[1] + y * f->linesize[1], (size_t)(w / 2));
            } else {
                o->sws = sws_getCachedContext(o->sws, f->width, f->height, f->format, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
                uint8_t *planes[3] = {dst, dst + ysz + csz, dst + ysz};
                int strides[3] = {w, w / 2, w / 2};
                sws_scale(o->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, planes, strides);
            }
            enqueue(o, buf, ysz + 2 * csz, t, d, f->key_frame ? 1 : 0);
        } else {                // RGB32, bottom-up like a DIB
            uint32_t len = (uint32_t)(w * h * 4), buf = gmalloc(len);
            o->sws = sws_getCachedContext(o->sws, f->width, f->height, f->format, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
            uint8_t *dst = (uint8_t *)G2H(buf) + (size_t)(h - 1) * (size_t)w * 4;
            uint8_t *planes[1] = {dst}; int strides[1] = {-w * 4};
            sws_scale(o->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, planes, strides);
            enqueue(o, buf, len, t, d, f->key_frame ? 1 : 0);
        }
    } else {
        if (!o->swr) {
            AVChannelLayout outl; av_channel_layout_default(&outl, o->channels);
            swr_alloc_set_opts2(&o->swr, &outl, AV_SAMPLE_FMT_S16, o->rate, &f->ch_layout, f->format, f->sample_rate, 0, NULL);
            swr_init(o->swr);
        }
        int max = swr_get_out_samples(o->swr, f->nb_samples);
        uint32_t buf = gmalloc((uint32_t)(max * o->channels * 2 + 16));
        uint8_t *dst[1] = {G2H(buf)};
        int n = swr_convert(o->swr, dst, max, (const uint8_t **)f->extended_data, f->nb_samples);
        if (n <= 0) { gfree(buf); return; }
        if (!d) d = (uint64_t)n * 10000000ull / (uint64_t)o->rate;
        enqueue(o, buf, (uint32_t)(n * o->channels * 2), t, d, 1);
    }
}

static void drain(Reader *r, Output *o, int send_eof) {
    AVFrame *f = av_frame_alloc();
    if (send_eof) avcodec_send_packet(o->dec, NULL);
    while (avcodec_receive_frame(o->dec, f) == 0) { convert(r, o, f); av_frame_unref(f); }
    av_frame_free(&f);
}

// read and decode until output `want` (or any, -1) has a sample; 0 when everything has ended
static int pump(Reader *r, int want) {
    AVPacket *pkt = av_packet_alloc();
    for (;;) {
        if (want >= 0 && r->out[want].head) break;
        if (want < 0) { int any = 0; for (int i = 0; i < r->nout; i++) any |= r->out[i].head != NULL; if (any) break; }
        if (r->ended) break;
        int e = av_read_frame(r->fmt, pkt);
        if (e < 0) {   // end of the file: flush every decoder
            r->ended = 1;
            for (int i = 0; i < r->nout; i++) drain(r, &r->out[i], 1);
            continue;
        }
        for (int i = 0; i < r->nout; i++) if (r->out[i].stream == pkt->stream_index) {
            if (avcodec_send_packet(r->out[i].dec, pkt) == 0) drain(r, &r->out[i], 0);
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (want >= 0) return r->out[want].head != NULL;
    for (int i = 0; i < r->nout; i++) if (r->out[i].head) return 1;
    return 0;
}

// ---------------- IWMOutputMediaProps ----------------
typedef struct { ComObj base; Reader *r; int out; int format; } Props;
static int props_type(Props *p, uint32_t buf) {   // writes WM_MEDIA_TYPE + format block at buf; returns size
    Output *o = &p->r->out[p->out];
    const uint32_t mt = 72;
    if (o->video) {
        uint32_t fmtsz = 88, size = mt + fmtsz;
        if (!buf) return (int)size;
        memset(G2H(buf), 0, size);
        memcpy(G2H(buf), &MT_Video, 16);
        memcpy(G2H(buf + 16), p->format == 0 ? &ST_YV12 : &ST_RGB32, 16);
        uint32_t img = p->format == 0 ? (uint32_t)(o->width * o->height * 3 / 2) : (uint32_t)(o->width * o->height * 4);
        wr32(buf + 32, 1); wr32(buf + 36, 0); wr32(buf + 40, img);
        memcpy(G2H(buf + 44), &FT_VideoInfo, 16);
        wr32(buf + 60, 0); wr32(buf + 64, fmtsz); wr32(buf + 68, buf + mt);
        uint32_t v = buf + mt;   // VIDEOINFOHEADER
        wr32(v + 8, (uint32_t)o->width); wr32(v + 12, (uint32_t)o->height);   // rcSource
        wr32(v + 24, (uint32_t)o->width); wr32(v + 28, (uint32_t)o->height);  // rcTarget
        AVRational fr = p->r->fmt->streams[o->stream]->avg_frame_rate;
        wr64(v + 40, fr.num ? (uint64_t)(10000000.0 * fr.den / fr.num) : 333333);
        uint32_t bi = v + 48;    // BITMAPINFOHEADER
        wr32(bi, 40); wr32(bi + 4, (uint32_t)o->width); wr32(bi + 8, (uint32_t)o->height);
        wr16(bi + 12, 1); wr16(bi + 14, p->format == 0 ? 12 : 32);
        wr32(bi + 16, p->format == 0 ? 0x32315659u : 0); wr32(bi + 20, img);
        return (int)size;
    }
    uint32_t fmtsz = 18, size = mt + fmtsz;
    if (!buf) return (int)size;
    memset(G2H(buf), 0, size);
    memcpy(G2H(buf), &MT_Audio, 16);
    memcpy(G2H(buf + 16), &ST_PCM, 16);
    wr32(buf + 32, 1); wr32(buf + 36, 0); wr32(buf + 40, (uint32_t)(o->channels * 2));
    memcpy(G2H(buf + 44), &FT_WaveFormatEx, 16);
    wr32(buf + 64, fmtsz); wr32(buf + 68, buf + mt);
    uint32_t w = buf + mt;   // WAVEFORMATEX
    wr16(w, 1); wr16(w + 2, (uint16_t)o->channels); wr32(w + 4, (uint32_t)o->rate);
    wr32(w + 8, (uint32_t)(o->rate * o->channels * 2)); wr16(w + 12, (uint16_t)(o->channels * 2)); wr16(w + 14, 16); wr16(w + 16, 0);
    return (int)size;
}
static uint32_t mp_GetType(CPU *c, ComObj *s, const ComMethod *m) {
    Props *p = (Props *)s;
    if (M(1)) memcpy(G2H(M(1)), p->r->out[p->out].video ? &MT_Video : &MT_Audio, 16);
    return S_OK;
}
static uint32_t mp_GetMediaType(CPU *c, ComObj *s, const ComMethod *m) {
    Props *p = (Props *)s;
    uint32_t need = (uint32_t)props_type(p, 0);
    if (!M(2)) return E_INVALIDARG;
    if (!M(1)) { wr32(M(2), need); return S_OK; }
    if (rd32(M(2)) < need) { wr32(M(2), need); return ASF_E_BUFFERTOOSMALL; }
    props_type(p, M(1));
    wr32(M(2), need);
    return S_OK;
}
static uint32_t mp_SetMediaType(CPU *c, ComObj *s, const ComMethod *m) {
    Props *p = (Props *)s;
    if (!M(1)) return E_INVALIDARG;
    Guid sub; memcpy(&sub, G2H(M(1) + 16), 16);
    if (!memcmp(&sub, &ST_YV12, 16)) p->format = 0;
    else if (!memcmp(&sub, &ST_RGB32, 16)) p->format = 1;
    return S_OK;
}
static uint32_t mp_name(CPU *c, ComObj *s, const ComMethod *m) {
    Props *p = (Props *)s;
    const char *n = p->r->out[p->out].video ? "Video" : "Audio";
    uint32_t len = (uint32_t)strlen(n) + 1;
    if (!M(2)) return E_INVALIDARG;
    if (!M(1)) { wr16(M(2), (uint16_t)len); return S_OK; }
    if (rd16(M(2)) < len) { wr16(M(2), (uint16_t)len); return ASF_E_BUFFERTOOSMALL; }
    for (uint32_t i = 0; i < len; i++) wr16(M(1) + 2 * i, (uint8_t)n[i]);
    return S_OK;
}
static const ComMethod props_m[] = { COM_IUNKNOWN,
    {"GetType", 2, mp_GetType, 0, 0, 0}, {"GetMediaType", 3, mp_GetMediaType, 0, 0, 0}, {"SetMediaType", 2, mp_SetMediaType, 0, 0, 0},
    {"GetStreamGroupName", 3, mp_name, 0, 0, 0}, {"GetConnectionName", 3, mp_name, 0, 0, 0} };
static ComClass props_cls = {"IWMOutputMediaProps", sizeof props_m / sizeof *props_m, props_m, 0, NULL};
static uint32_t new_props(Reader *r, int out, int format) {
    Props *p = calloc(1, sizeof *p);
    p->r = r; p->out = out; p->format = format;
    return com_wrap(&p->base, &props_cls);
}

// ---------------- IWMHeaderInfo ----------------
typedef struct { ComObj base; Reader *r; } Header;
// GetAttributeByName(WORD *pwStreamNum, LPCWSTR name, WMT_ATTR_DATATYPE *type, BYTE *value, WORD *pcbLength)
static uint32_t hi_GetAttributeByName(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = ((Header *)s)->r;
    char name[64]; w_to_utf8(gp(M(2)), name, sizeof name);
    uint32_t type, len; uint64_t v;
    if (!strcmp(name, "Duration")) { type = 4; len = 8; v = (uint64_t)r->duration; }   // WMT_TYPE_QWORD
    else if (!strcmp(name, "Seekable") || !strcmp(name, "Stridable")) { type = 2; len = 4; v = 1; }   // WMT_TYPE_BOOL
    else return ASF_E_NOTFOUND;
    if (M(3)) wr32(M(3), type);
    if (!M(5)) return E_INVALIDARG;
    if (!M(4)) { wr16(M(5), (uint16_t)len); return S_OK; }
    if (rd16(M(5)) < len) { wr16(M(5), (uint16_t)len); return ASF_E_BUFFERTOOSMALL; }
    if (len == 8) wr64(M(4), v); else wr32(M(4), (uint32_t)v);
    wr16(M(5), (uint16_t)len);
    return S_OK;
}
static uint32_t hi_GetAttributeCount(CPU *c, ComObj *s, const ComMethod *m) { if (M(2)) wr16(M(2), 2); return S_OK; }
static const ComMethod header_m[] = { COM_IUNKNOWN,
    {"GetAttributeCount", 3, hi_GetAttributeCount, 0, 0, 0}, {"GetAttributeByIndex", 8, NULL, ASF_E_NOTFOUND, 0, 0},
    {"GetAttributeByName", 6, hi_GetAttributeByName, 0, 0, 0}, {"SetAttribute", 6, NULL, E_FAIL, 0, 0},
    {"GetMarkerCount", 2, NULL, E_FAIL, 0, 0}, {"GetMarker", 5, NULL, E_FAIL, 0, 0}, {"AddMarker", 3, NULL, E_FAIL, 0, 0},
    {"RemoveMarker", 2, NULL, E_FAIL, 0, 0}, {"GetScriptCount", 2, NULL, E_FAIL, 0, 0}, {"GetScript", 7, NULL, E_FAIL, 0, 0},
    {"AddScript", 5, NULL, E_FAIL, 0, 0}, {"RemoveScript", 2, NULL, E_FAIL, 0, 0} };
static ComClass header_cls = {"IWMHeaderInfo", sizeof header_m / sizeof *header_m, header_m, 0, NULL};

// ---------------- IWMSyncReader ----------------
static int out_of_stream(Reader *r, uint32_t asf) {
    for (int i = 0; i < r->nout; i++) if ((uint32_t)r->fmt->streams[r->out[i].stream]->id == asf) return i;
    return -1;
}
static uint32_t sr_QueryInterface(CPU *c, ComObj *s, const ComMethod *m) {
    Guid iid; memcpy(&iid, G2H(M(1)), 16);
    if (!memcmp(&iid, &IID_IWMHeaderInfo, 12)) {   // IWMHeaderInfo, 2, 3 share the first 12 bytes' layout family
        Header *h = calloc(1, sizeof *h); h->r = (Reader *)s;
        wr32(M(2), com_wrap(&h->base, &header_cls));
        return S_OK;
    }
    return com_QueryInterface(c, s, m);
}
static uint32_t sr_Open(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    char wn[1024], p[1200]; w_to_utf8(gp(M(1)), wn, sizeof wn);
    r->cpu = c;
    r->file = fopen(rt_host_path(wn, p, sizeof p), "rb");
    if (!r->file) return 0x80070002u;
    return reader_open(r);
}
static uint32_t sr_OpenStream(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (!M(1)) return E_INVALIDARG;
    r->cpu = c;
    r->istream = M(1);
    uint32_t vt = rd32(r->istream); guest_call(c, rd32(vt + 4), 1, &r->istream);   // AddRef
    return reader_open(r);
}
static uint32_t sr_Close(CPU *c, ComObj *s, const ComMethod *m) { Reader *r = (Reader *)s; r->cpu = c; reader_close(r); return S_OK; }
// SetRange(QWORD cnsStartTime, LONGLONG cnsDuration)
static uint32_t sr_SetRange(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (!r->fmt) return E_FAIL;
    r->cpu = c;
    uint64_t start = ARG64(1);
    av_seek_frame(r->fmt, -1, (int64_t)(start / 10), AVSEEK_FLAG_BACKWARD);
    for (int i = 0; i < r->nout; i++) out_flush(&r->out[i]);
    r->ended = 0;
    return S_OK;
}
// GetNextSample(WORD wStreamNum, INSSBuffer **, QWORD *time, QWORD *duration, DWORD *flags, DWORD *output, WORD *stream)
static uint32_t sr_GetNextSample(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (!r->fmt) return E_FAIL;
    r->cpu = c;
    int want = (M(1) & 0xFFFF) ? out_of_stream(r, M(1) & 0xFFFF) : -1;
    if ((M(1) & 0xFFFF) && want < 0) return E_INVALIDARG;
    if (!pump(r, want)) return NS_E_NO_MORE_SAMPLES;
    int o = want;
    if (o < 0) {   // any stream: the earliest queued sample
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < r->nout; i++) if (r->out[i].head && r->out[i].head->time < best) { best = r->out[i].head->time; o = i; }
    }
    Output *out = &r->out[o];
    Queued *q = out->head; out->head = q->next; if (!out->head) out->tail = NULL;
    Sample *b = calloc(1, sizeof *b);
    b->buf = q->buf; b->len = q->len; b->max = q->len;
    if (M(2)) wr32(M(2), com_wrap(&b->base, &sample_cls)); else sample_destroy(&b->base);
    if (M(3)) wr64(M(3), q->time);
    if (M(4)) wr64(M(4), q->dur);
    if (M(5)) wr32(M(5), q->flags);
    if (M(6)) wr32(M(6), (uint32_t)o);
    if (M(7)) wr16(M(7), (uint16_t)r->fmt->streams[out->stream]->id);
    free(q);
    static int nsamples; if (rt_verbose && (++nsamples == 1 || nsamples % 100 == 0)) rt_log("[wmv] %d samples delivered", nsamples);
    return S_OK;
}
static uint32_t sr_ok(CPU *c, ComObj *s, const ComMethod *m) { return S_OK; }
static uint32_t sr_GetOutputCount(CPU *c, ComObj *s, const ComMethod *m) { if (M(1)) wr32(M(1), (uint32_t)((Reader *)s)->nout); return S_OK; }
static uint32_t sr_GetOutputProps(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (M(1) >= (uint32_t)r->nout || !M(2)) return E_INVALIDARG;
    wr32(M(2), new_props(r, (int)M(1), r->out[M(1)].format));
    return S_OK;
}
static uint32_t sr_SetOutputProps(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (M(1) >= (uint32_t)r->nout) return E_INVALIDARG;
    Props *p = (Props *)com_host(M(2));
    if (!p) return NS_E_INVALID_OUTPUT_FORMAT;
    r->out[M(1)].format = p->format;
    return S_OK;
}
static uint32_t sr_GetOutputFormatCount(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (M(1) >= (uint32_t)r->nout) return E_INVALIDARG;
    if (M(2)) wr32(M(2), r->out[M(1)].video ? 2 : 1);   // video: YV12, RGB32; audio: PCM
    return S_OK;
}
static uint32_t sr_GetOutputFormat(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (M(1) >= (uint32_t)r->nout || !M(3)) return E_INVALIDARG;
    if (M(2) >= (r->out[M(1)].video ? 2u : 1u)) return E_INVALIDARG;
    wr32(M(3), new_props(r, (int)M(1), (int)M(2)));
    return S_OK;
}
static uint32_t sr_GetOutputNumberForStream(CPU *c, ComObj *s, const ComMethod *m) {
    int o = out_of_stream((Reader *)s, M(1) & 0xFFFF);
    if (o < 0) return E_INVALIDARG;
    if (M(2)) wr32(M(2), (uint32_t)o);
    return S_OK;
}
static uint32_t sr_GetStreamNumberForOutput(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (M(1) >= (uint32_t)r->nout) return E_INVALIDARG;
    if (M(2)) wr16(M(2), (uint16_t)r->fmt->streams[r->out[M(1)].stream]->id);
    return S_OK;
}
static uint32_t sr_GetMaxOutputSampleSize(CPU *c, ComObj *s, const ComMethod *m) {
    Reader *r = (Reader *)s;
    if (M(1) >= (uint32_t)r->nout) return E_INVALIDARG;
    Output *o = &r->out[M(1)];
    if (M(2)) wr32(M(2), o->video ? (uint32_t)(o->width * o->height * 4) : (uint32_t)(o->rate * o->channels * 2));
    return S_OK;
}
static void reader_destroy(ComObj *o) { Reader *r = (Reader *)o; r->cpu = rt_cpu(); reader_close(r); free(r); }
static const ComMethod reader_m[] = {
    {"QueryInterface", 3, sr_QueryInterface, 0, 0, 0}, {"AddRef", 1, com_AddRef, 0, 0, 0}, {"Release", 1, com_Release, 0, 0, 0},
    {"Open", 2, sr_Open, 0, 0, 0}, {"Close", 1, sr_Close, 0, 0, 0}, {"SetRange", 5, sr_SetRange, 0, 0, 0},
    {"SetRangeByFrame", 6, NULL, E_FAIL, 0, 0}, {"GetNextSample", 8, sr_GetNextSample, 0, 0, 0},
    {"SetStreamsSelected", 4, sr_ok, 0, 0, 0}, {"GetStreamSelected", 3, NULL, E_FAIL, 0, 0},
    {"SetReadStreamSamples", 3, sr_ok, 0, 0, 0}, {"GetReadStreamSamples", 3, NULL, E_FAIL, 0, 0},
    {"GetOutputSetting", 6, NULL, E_FAIL, 0, 0}, {"SetOutputSetting", 6, sr_ok, 0, 0, 0},
    {"GetOutputCount", 2, sr_GetOutputCount, 0, 0, 0}, {"GetOutputProps", 3, sr_GetOutputProps, 0, 0, 0},
    {"SetOutputProps", 3, sr_SetOutputProps, 0, 0, 0}, {"GetOutputFormatCount", 3, sr_GetOutputFormatCount, 0, 0, 0},
    {"GetOutputFormat", 4, sr_GetOutputFormat, 0, 0, 0}, {"GetOutputNumberForStream", 3, sr_GetOutputNumberForStream, 0, 0, 0},
    {"GetStreamNumberForOutput", 3, sr_GetStreamNumberForOutput, 0, 0, 0}, {"GetMaxOutputSampleSize", 3, sr_GetMaxOutputSampleSize, 0, 0, 0},
    {"GetMaxStreamSampleSize", 3, sr_GetMaxOutputSampleSize, 0, 0, 0}, {"OpenStream", 2, sr_OpenStream, 0, 0, 0},
};
static ComClass reader_cls = {"IWMSyncReader", sizeof reader_m / sizeof *reader_m, reader_m, 0, reader_destroy};

// WMCreateSyncReader(IUnknown *pUnkCert, DWORD dwRights, IWMSyncReader **ppSyncReader)
SHIM(wmvcore_WMCreateSyncReader, 3) {
    if (!ARG(2)) return E_INVALIDARG;
    Reader *r = calloc(1, sizeof *r);
    wr32(ARG(2), com_wrap(&r->base, &reader_cls));
    if (rt_verbose) rt_log("[wmv] WMCreateSyncReader -> 0x%x", rd32(ARG(2)));
    return S_OK;
}
