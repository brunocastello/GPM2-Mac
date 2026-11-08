/* Portable mixer: fixed output of 44100 Hz, 16 bits, stereo. Sources are PCM WAV (unsigned 8 bits or
 * 16 bits; mono or stereo; any rate) converted at load time with linear resampling. Slots: AU_SND (sndPlaySound,
 * one channel) and AU_MCI0.. (MCI waveaudio devices), which play together.
 *
 * Threads: `audio_render` runs on the backend's audio thread and only touches the mixer state (mutex); it NEVER calls the guest.
 * The end of a sound is only a flag (`done`) that the guest thread reads in audio_pump (-> mci_poll posts MM_MCINOTIFY).
 * Without a backend (headless) the mixer follows the guest clock (virtual time with --fixed-time), with no output, or writing
 * a debug WAV (audio_set_dump). */
#include "win32.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int state;                   /* 0 stopped, 1 playing, 2 paused */
    Pcm pcm;
    uint32_t pos, from, end;     /* in frames */
    int loop, done, log;
    char name[96];
} Voice;
typedef struct { char name[96]; uint32_t start, end; int slot; int open; } LogE;

static Voice vc[AU_NSLOT];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int backend;
static volatile uint32_t g_clock;
#define MAX_LOG 512
static LogE logs[MAX_LOG];
static int nlog;
static FILE *dump;
static uint32_t dump_bytes;
static const char *dump_path;

/* ---- WAV -> PCM 44100/16/stereo -------------------------------------------------------------------- */
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }

/* frames [k0, k0+n) of the output from the raw samples `src`, whose first frame is `first`. 32-bit integers only: cheap on PowerPC */
static void conv_frames(const uint8_t *src, uint32_t first, uint32_t nin, uint32_t ch, uint32_t bits, uint32_t rate, uint32_t k0, uint32_t n, int16_t *dst) {
    uint32_t bytes = bits / 8u, i0, num;
    uint64_t start = (uint64_t)k0 * rate;
    i0 = (uint32_t)(start / AUDIO_RATE); num = (uint32_t)(start % AUDIO_RATE);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i1 = i0 + 1 < nin ? i0 + 1 : i0, f = num * 32768u / AUDIO_RATE;
        for (uint32_t c = 0; c < 2; c++) {
            uint32_t cc = ch == 1 ? 0 : c;
            const uint8_t *pa = src + (size_t)(i0 - first) * ch * bytes + cc * bytes, *pb = src + (size_t)(i1 - first) * ch * bytes + cc * bytes;
            int32_t a = bits == 8 ? ((int32_t)pa[0] - 128) * 256 : (int16_t)le16(pa);   /* 8 bits: unsigned -> signed */
            int32_t b = bits == 8 ? ((int32_t)pb[0] - 128) * 256 : (int16_t)le16(pb);
            *dst++ = (int16_t)(a + (((b - a) * (int32_t)f + 16384) >> 15));
        }
        num += rate;
        while (num >= AUDIO_RATE) { num -= AUDIO_RATE; i0++; }
    }
}
int wav_to_pcm(const uint8_t *d, size_t n, Pcm *out) {
    size_t i = 12;
    uint32_t tag = 0, ch = 0, rate = 0, bits = 0, dlen = 0, nin, nout;
    const uint8_t *data = 0;
    int16_t *pcm;
    out->pcm = 0; out->frames = 0; out->st = 0;
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 1;
    while (i + 8 <= n) {
        uint32_t cl = le32(d + i + 4);
        if (!memcmp(d + i, "fmt ", 4) && i + 8 + 16 <= n) {
            tag = le16(d + i + 8); ch = le16(d + i + 10); rate = le32(d + i + 12); bits = le16(d + i + 22);
        } else if (!memcmp(d + i, "data", 4)) {
            data = d + i + 8;
            dlen = cl;
            if (i + 8 + (size_t)dlen > n) dlen = (uint32_t)(n - i - 8);       /* truncated file */
            break;
        }
        i += 8 + (size_t)cl + (cl & 1u);                  /* skip the other chunks */
    }
    if (!data || tag != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate == 0) return 1;
    nin = dlen / (ch * bits / 8);
    nout = (uint32_t)((uint64_t)nin * AUDIO_RATE / rate);
    pcm = (int16_t *)malloc((size_t)(nout + 1) * 4);
    if (!pcm) return 1;
    if (nin) for (uint32_t k = 0; k < nout; k += 0x4000u) {
        conv_frames(data, 0, nin, ch, bits, rate, k, nout - k < 0x4000u ? nout - k : 0x4000u, pcm + (size_t)k * 2);
        if (dispatch_yield_hook) dispatch_yield_hook();      /* long conversion: does not hang the cooperative host (Mac OS 9) */
    }
    out->pcm = pcm; out->frames = nout;
    return 0;
}

/* ---- streaming of long WAVs ---------------------------------------------------------------------------------
 * The music is 21 MB: instead of converting everything on open, a per-voice ring receives blocks converted on demand.
 * Who fills it: the service thread (or the host's event pump on OS 9, which has no threads; or the clock itself, with no backend).
 * The mixer only consumes the ring and never touches the file. Lock order: io_mu -> mu; io_mu protects the FILE. */
#define ST_CAP 65536u                /* power of 2: ~1.5 s */
#define ST_CHUNK 16384u              /* ~0,37 s */
struct AudioStream {
    FILE *f;
    long data;
    uint32_t ch, bits, rate, nin;
    int16_t *ring;
    uint32_t base, count, gen;       /* the ring holds frames [base, base+count); index = frame & (ST_CAP-1) */
};
static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;

/* 0 = ok and takes over `f`; 1 = not a valid PCM WAV (the caller closes `f`) */
int audio_stream_open(FILE *f, long size, Pcm *out) {
    uint8_t h[24];
    long i = 12, data = 0;
    uint32_t tag = 0, ch = 0, rate = 0, bits = 0, dlen = 0;
    AudioStream *st;
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 1;
    while (i + 8 <= size) {
        uint32_t cl;
        if (fseek(f, i, SEEK_SET) != 0 || fread(h, 1, 24, f) < 8) return 1;
        cl = le32(h + 4);
        if (!memcmp(h, "fmt ", 4) && i + 8 + 16 <= size) {
            tag = le16(h + 8); ch = le16(h + 10); rate = le32(h + 12); bits = le16(h + 22);
        } else if (!memcmp(h, "data", 4)) {
            data = i + 8;
            dlen = (uint64_t)cl > (uint64_t)(size - data) ? (uint32_t)(size - data) : cl;
            break;
        }
        i += 8 + (long)cl + (long)(cl & 1u);
    }
    if (!data || tag != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate == 0) return 1;
    st = (AudioStream *)calloc(1, sizeof *st);
    if (st) st->ring = (int16_t *)malloc(ST_CAP * 4u);
    if (!st || !st->ring) { free(st); return 1; }
    st->f = f; st->data = data; st->ch = ch; st->bits = bits; st->rate = rate;
    st->nin = dlen / (ch * bits / 8);
    out->pcm = 0; out->st = st;
    out->frames = (uint32_t)((uint64_t)st->nin * AUDIO_RATE / rate);
    return 0;
}
/* with `mu`: moves the ring to `pos`, reusing what is already in it */
static void st_reposition(AudioStream *st, uint32_t pos) {
    if (pos < st->base || pos > st->base + st->count) { st->gen++; st->base = pos; st->count = 0; }
    else { st->count -= pos - st->base; st->base = pos; }
}
/* with `io_mu`: one block for the voice. 1 = filled something. Reads and converts outside `mu`, so as not to stall the mixer. */
static int st_fill(int slot, int force) {
    Voice *v = &vc[slot];
    AudioStream *st;
    uint32_t at, n, gen, nin, first, last, fb, got;
    uint8_t *raw;
    int16_t *tmp;
    int bad = 0;
    pthread_mutex_lock(&mu);
    st = v->pcm.st;
    if (!st || (!force && v->state != 1)) { pthread_mutex_unlock(&mu); return 0; }
    at = st->base + st->count;
    n = ST_CAP - st->count;
    if (at >= v->pcm.frames || n == 0 || (n < ST_CHUNK && !force)) { pthread_mutex_unlock(&mu); return 0; }
    if (n > ST_CHUNK) n = ST_CHUNK;
    if (n > v->pcm.frames - at) n = v->pcm.frames - at;
    gen = st->gen;
    pthread_mutex_unlock(&mu);
    nin = st->nin; fb = st->ch * st->bits / 8u;
    first = (uint32_t)((uint64_t)at * st->rate / AUDIO_RATE);
    last = (uint32_t)((uint64_t)(at + n - 1u) * st->rate / AUDIO_RATE) + 1u;
    if (last >= nin) last = nin - 1u;
    raw = (uint8_t *)malloc((size_t)(last - first + 1u) * fb);
    tmp = (int16_t *)malloc((size_t)n * 4u);
    if (!raw || !tmp) { free(raw); free(tmp); return 0; }                          /* out of memory: try again later */
    got = 0;
    if (fseek(st->f, st->data + (long)first * (long)fb, SEEK_SET) == 0) got = (uint32_t)fread(raw, fb, (size_t)(last - first + 1u), st->f);
    if (got == last - first + 1u) conv_frames(raw, first, nin, st->ch, st->bits, st->rate, at, n, tmp);
    else bad = 1;
    free(raw);
    pthread_mutex_lock(&mu);
    if (st->gen == gen) {
        if (bad) {                                                                 /* read error: the sound ends here, instead of staying silent forever */
            v->pcm.frames = at;
            if (v->end > at) v->end = at;
        } else {
            for (uint32_t k = 0; k < n; k++) {
                uint32_t ix = (at + k) & (ST_CAP - 1u);
                st->ring[ix * 2] = tmp[k * 2]; st->ring[ix * 2 + 1] = tmp[k * 2 + 1];
            }
            st->count += n;
        }
    }
    pthread_mutex_unlock(&mu);
    free(tmp);
    return !bad;
}
void audio_stream_service(void) {
    pthread_mutex_lock(&io_mu);
    for (int s = 0; s < AU_NSLOT; s++) st_fill(s, 0);
    pthread_mutex_unlock(&io_mu);
}
static void *st_thread(void *arg) {
    (void)arg;
    for (;;) { audio_stream_service(); host_sleep_ms(20); }
    return 0;
}

/* ---- mixer ----------------------------------------------------------------------------------------- */
static void log_end(Voice *v) {
    if (v->log >= 0 && v->log < nlog && logs[v->log].open) { logs[v->log].end = g_clock; logs[v->log].open = 0; }
    v->log = -1;
}
void audio_mix_int(int16_t *out, int nframes) {
    pthread_mutex_lock(&mu);
    for (int f = 0; f < nframes; f++) {
        int32_t l = 0, r = 0;
        for (int s = 0; s < AU_NSLOT; s++) {
            Voice *v = &vc[s];
            if (v->state != 1) continue;
            if (v->pos >= v->end) {
                if (v->loop && v->end > v->from) { v->pos = v->from; if (v->pcm.st) st_reposition(v->pcm.st, v->pos); }
                else { v->state = 0; v->done = 1; v->pos = v->end; log_end(v); continue; }
            }
            if (v->pcm.st) {
                AudioStream *st = v->pcm.st;
                uint32_t ix = st->base & (ST_CAP - 1u);
                if (!st->count) continue;                     /* empty ring: silence, without losing the position */
                l += st->ring[ix * 2]; r += st->ring[ix * 2 + 1];
                st->base++; st->count--;
            } else {
                l += v->pcm.pcm[(size_t)v->pos * 2]; r += v->pcm.pcm[(size_t)v->pos * 2 + 1];
            }
            v->pos++;
        }
        out[f * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[f * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    pthread_mutex_unlock(&mu);
}
/* Last time the sound output asked for samples. A stalled output (OS 9 above 1 GB on official UTM) must not hang the game:
 * the end of each sound comes from the mixer, and the game waits for it. See audio_pump. */
static volatile uint32_t last_render_ms;
void audio_render(int16_t *out, int nframes) { last_render_ms = host_now_ms(); audio_mix_int(out, nframes); }

/* ---- voice ----------------------------------------------------------------------------------------- */
static uint32_t ms2f(uint32_t ms) { return (uint32_t)((uint64_t)ms * AUDIO_RATE / 1000u); }
static uint32_t f2ms(uint32_t f) { return (uint32_t)((uint64_t)f * 1000u / AUDIO_RATE); }

void audio_voice_release(int slot) {
    Voice *v = &vc[slot];
    pthread_mutex_lock(&io_mu);
    pthread_mutex_lock(&mu);
    log_end(v);
    v->state = 0;
    free(v->pcm.pcm);
    if (v->pcm.st) { fclose(v->pcm.st->f); free(v->pcm.st->ring); free(v->pcm.st); }
    memset(&v->pcm, 0, sizeof v->pcm);
    v->pos = v->from = v->end = 0; v->done = 0;
    pthread_mutex_unlock(&mu);
    pthread_mutex_unlock(&io_mu);
}
int audio_voice_load(int slot, Pcm pcm, const char *name) {
    Voice *v = &vc[slot];
    audio_voice_release(slot);
    pthread_mutex_lock(&mu);
    v->pcm = pcm; v->end = pcm.frames; v->loop = 0; v->log = -1;
    snprintf(v->name, sizeof v->name, "%s", name);
    pthread_mutex_unlock(&mu);
    return 0;
}
/* from_ms = 0xFFFFFFFF: continue from the current position; to_ms = 0: to the end */
int audio_voice_play(int slot, uint32_t from_ms, uint32_t to_ms, int loop) {
    Voice *v = &vc[slot];
    pthread_mutex_lock(&mu);
    if (!v->pcm.pcm && !v->pcm.st) { pthread_mutex_unlock(&mu); return 1; }
    log_end(v);
    v->from = from_ms == 0xFFFFFFFFu ? (v->pos >= v->end ? 0 : v->pos) : ms2f(from_ms);
    if (v->from > v->pcm.frames) v->from = v->pcm.frames;
    v->end = to_ms && ms2f(to_ms) < v->pcm.frames ? ms2f(to_ms) : v->pcm.frames;
    v->pos = v->from; v->loop = loop; v->done = 0;
    if (v->pcm.st) {                                       /* the first block comes before playing: no silence at the start */
        v->state = 0;
        st_reposition(v->pcm.st, v->pos);
        pthread_mutex_unlock(&mu);
        pthread_mutex_lock(&io_mu);
        st_fill(slot, 1);
        pthread_mutex_unlock(&io_mu);
        pthread_mutex_lock(&mu);
    }
    v->state = 1;
    if (nlog < MAX_LOG) {
        snprintf(logs[nlog].name, sizeof logs[nlog].name, "%s", v->name);
        logs[nlog].start = g_clock; logs[nlog].end = 0; logs[nlog].slot = slot; logs[nlog].open = 1;
        v->log = nlog++;
    }
    pthread_mutex_unlock(&mu);
    return 0;
}
void audio_voice_stop(int slot) {
    Voice *v = &vc[slot];
    pthread_mutex_lock(&mu);
    if (v->state) { v->state = 0; log_end(v); }
    v->done = 0;
    pthread_mutex_unlock(&mu);
}
void audio_voice_pause(int slot, int pause) {
    Voice *v = &vc[slot];
    pthread_mutex_lock(&mu);
    if (pause && v->state == 1) v->state = 2;
    else if (!pause && v->state == 2) v->state = 1;
    pthread_mutex_unlock(&mu);
}
int audio_voice_state(int slot) { return vc[slot].state; }
int audio_voice_take_done(int slot) {
    int r;
    pthread_mutex_lock(&mu);
    r = vc[slot].done; vc[slot].done = 0;
    pthread_mutex_unlock(&mu);
    return r;
}
uint32_t audio_voice_pos_ms(int slot) { return f2ms(vc[slot].pos); }
uint32_t audio_voice_len_ms(int slot) { return f2ms(vc[slot].pcm.frames); }
void audio_voice_seek(int slot, uint32_t ms) {
    Voice *v = &vc[slot];
    pthread_mutex_lock(&mu);
    v->state = 0; v->done = 0;
    v->pos = ms2f(ms) > v->pcm.frames ? v->pcm.frames : ms2f(ms);
    v->from = 0; v->end = v->pcm.frames;
    pthread_mutex_unlock(&mu);
}

/* ---- guest clock (no backend) and events ------------------------------------------------------------ */
void audio_set_dump(const char *path) { dump_path = path; }
static void dump_close(void) {
    uint8_t h[44];
    if (!dump) return;
    memcpy(h, "RIFF", 4); h[4] = (uint8_t)(36 + dump_bytes); h[5] = (uint8_t)((36 + dump_bytes) >> 8); h[6] = (uint8_t)((36 + dump_bytes) >> 16); h[7] = (uint8_t)((36 + dump_bytes) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = (uint8_t)AUDIO_RATE; h[25] = (uint8_t)(AUDIO_RATE >> 8); h[26] = (uint8_t)(AUDIO_RATE >> 16); h[27] = 0;
    { uint32_t br = AUDIO_RATE * 4; h[28] = (uint8_t)br; h[29] = (uint8_t)(br >> 8); h[30] = (uint8_t)(br >> 16); h[31] = (uint8_t)(br >> 24); }
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)dump_bytes; h[41] = (uint8_t)(dump_bytes >> 8); h[42] = (uint8_t)(dump_bytes >> 16); h[43] = (uint8_t)(dump_bytes >> 24);
    fseek(dump, 0, SEEK_SET); fwrite(h, 1, 44, dump); fclose(dump); dump = 0;
}
void audio_init(void) {
    for (int i = 0; i < AU_NSLOT; i++) vc[i].log = -1;
    backend = host_audio_open();
    if (backend) {                                         /* no threads (OS 9): the backend calls audio_stream_service in its pump */
        pthread_t th;
        if (pthread_create(&th, 0, st_thread, 0) != 0) win32_log("audio: no stream thread");
    }
}
static int any_playing(void) {
    for (int i = 0; i < AU_NSLOT; i++) if (vc[i].state == 1) return 1;
    return 0;
}
void audio_pump(uint32_t now) {
    static uint32_t t0, started;
    static uint64_t rendered;
    static uint32_t stall_t0;
    static int stalled;
    g_clock = now;
    if (backend) {                                         /* stalled output: the mixer follows the clock, silently, until it comes back */
        enum { STALL_MS = 500 };
        static int16_t mute[4096 * 2];
        uint32_t last = last_render_ms;
        if (!last) last_render_ms = last = now;
        if (now - last < STALL_MS) stalled = 0;
        else {
            uint64_t target;
            if (!stalled) { stalled = 1; stall_t0 = now; rendered = 0; win32_log("audio: output stalled, mixer follows the clock"); }
            target = (uint64_t)(now - stall_t0) * AUDIO_RATE / 1000u;
            while (rendered < target && any_playing()) {
                uint64_t k = target - rendered;
                if (k > 4096) k = 4096;
                audio_stream_service();
                audio_mix_int(mute, (int)k);
                rendered += k;
            }
            if (!any_playing()) rendered = target;
        }
    }
    if (!backend) {
        uint64_t target;
        static int16_t scratch[4096 * 2];
        if (!started) { started = 1; t0 = now; if (dump_path) { dump = fopen(dump_path, "wb"); if (dump) { fwrite(scratch, 1, 44, dump); atexit(dump_close); } } }
        target = (uint64_t)(now - t0) * AUDIO_RATE / 1000u;
        while (rendered < target && (any_playing() || dump)) {
            uint64_t k = target - rendered;
            if (k > 4096) k = 4096;
            audio_stream_service();
            audio_mix_int(scratch, (int)k);
#ifdef GPM2_BIG_ENDIAN
            if (dump) for (uint64_t j = 0; j < k * 2; j++) scratch[j] = (int16_t)LE16_(scratch[j]);   /* WAV is little-endian */
#endif
            if (dump) { fwrite(scratch, 4, (size_t)k, dump); dump_bytes += (uint32_t)k * 4u; }
            rendered += k;
        }
        if (!(any_playing() || dump)) rendered = target;
    }
    mci_poll();
}
uint32_t audio_ms_until_event(void) {
    uint32_t best = 0xFFFFFFFFu;
    for (int i = 0; i < AU_NSLOT; i++)
        if (vc[i].state == 1 && !vc[i].loop) {
            uint32_t ms = f2ms(vc[i].end - vc[i].pos) + 2u;
            if (ms < best) best = ms;
        }
    return best;
}

void audio_log_write(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nlog; i++) {
        if (logs[i].open) fprintf(f, "%6u  %6s  %s  %s\n", logs[i].start, "...", logs[i].slot == AU_SND ? "snd" : "mci", logs[i].name);
        else fprintf(f, "%6u  %6u  %s  %s\n", logs[i].start, logs[i].end, logs[i].slot == AU_SND ? "snd" : "mci", logs[i].name);
    }
    pthread_mutex_unlock(&mu);
    fclose(f);
}
