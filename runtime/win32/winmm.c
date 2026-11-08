/* WINMM (stdcall): sndPlaySoundA and MCI "waveaudio" over the audio.c mixer.
 * MCI_NOTIFY: when finished (1 = SUCCESSFUL) or interrupted (4 = ABORTED, 2 = SUPERSEDED) we post MM_MCINOTIFY
 * (0x3B9; wParam = code, lParam = device id) to the guest queue, always from the guest thread. */
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM_MCINOTIFY 0x03B9
#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_PLAY 0x0806
#define MCI_SEEK 0x0807
#define MCI_STOP 0x0808
#define MCI_PAUSE 0x0809
#define MCI_SET 0x080D
#define MCI_STATUS 0x0814
#define MCI_RESUME 0x0855
#define MCI_NOTIFY 0x1u
#define MCI_WAIT 0x2u
#define MCI_FROM 0x4u
#define MCI_TO 0x8u
#define MCI_OPEN_ELEMENT 0x200u
#define MCI_OPEN_TYPE 0x2000u
#define MCIERR_INVALID_DEVICE_ID 257
#define MCIERR_UNRECOGNIZED_COMMAND 261
#define MCIERR_INVALID_DEVICE_NAME 263
#define MCIERR_UNSUPPORTED_FUNCTION 274
#define MCIERR_FILE_NOT_FOUND 275
#define MCIERR_BAD_TIME_FORMAT 293
#define MCIERR_INVALID_FILE 296
#define MCIERR_NULL_PARAMETER_BLOCK 297
#define MCIERR_OUT_OF_MEMORY 264
#define MCIERR_DEVICE_OPEN 265

typedef struct { int used; uint32_t id, notify_hwnd; int pending; } Dev;
static Dev devs[AU_NMCI];

/* ---- locate and load the WAV --------------------------------------------------------------------------- */
static int load_wav_file(const char *name, Pcm *pcm, int *notfound) {
    char cand[2][300], host[2048];
    uint8_t *buf;
    long n;
    FILE *f = 0;
    int nc = 0;
    snprintf(cand[nc++], sizeof cand[0], "%s", name);
    if (!strchr(name, '\\') && !strchr(name, '/') && !(name[0] && name[1] == ':')) snprintf(cand[nc++], sizeof cand[0], "C:\\GPM2\\%s", name);
    for (int i = 0; i < nc && !f; i++) {
        vfs_resolve(cand[i], 0, host, sizeof host);
        f = fopen(host, "rb");
    }
    if (!f) { *notfound = 1; return 1; }
    *notfound = 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 128 * 1024) {                                       /* music and long speech: streaming, opening converts nothing */
        if (audio_stream_open(f, n, pcm) == 0) return 0;
        fclose(f);
        return 1;
    }
    buf = (uint8_t *)malloc((size_t)n + 1u);
    if (!buf || fread_yield(buf, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 1; }
    fclose(f);
    n = wav_to_pcm(buf, (size_t)n, pcm);
    free(buf);
    return (int)n;
}

/* ---- sndPlaySound ------------------------------------------------------------------------------------- */
static int snd_start(Pcm pcm, const char *name, uint32_t flags) {
    audio_voice_load(AU_SND, pcm, name);
    audio_voice_play(AU_SND, 0xFFFFFFFFu, 0, (flags & 8u) != 0);
    if (!(flags & 1u) && !(flags & 8u)) {                      /* SND_SYNC: waits for the end */
        for (int guard = 0; guard < 60000 && audio_voice_state(AU_SND) != 0; guard++) {
            uint32_t w = audio_ms_until_event();
            audio_pump(win32_now_ms());
            if (opt_fixed_time_set) win32_advance_ms(w != 0xFFFFFFFFu && w > 1 ? w : 1); else host_sleep_ms(2);
        }
    }
    return 1;
}
int snd_play(const char *name, uint32_t flags) {
    Pcm pcm;
    int nf;
    char path[320];
    if (!name) { audio_voice_stop(AU_SND); return 1; }
    snprintf(path, sizeof path, "%s", name);
    if (!strchr(path, '.')) strncat(path, ".wav", sizeof path - strlen(path) - 1);
    if (load_wav_file(path, &pcm, &nf)) {
        win32_log("sndPlaySound('%s'): %s", name, nf ? "file does not exist" : "invalid WAV");
        return (flags & 2u) ? 0 : 1;                           /* SND_NODEFAULT: FALSE; otherwise the default beep (silent) */
    }
    return snd_start(pcm, name, flags);
}
static uint32_t wm_sndPlaySoundA(void) {
    char n[300];
    if (!ARG(0)) return (uint32_t)snd_play(0, ARG(1));
    if (ARG(1) & 4u) {                                          /* SND_MEMORY: WAV image in memory (the game's speech, already mixed) */
        uint32_t n = R32(ARG(0) + 4) + 8u;                      /* RIFF size + header */
        uint8_t *buf;
        Pcm pcm;
        if (n < 44 || n > 0x100000u) return 0;
        buf = (uint8_t *)malloc(n);
        if (!buf) return 0;
        gmem_get(buf, ARG(0), n);
        if (wav_to_pcm(buf, n, &pcm) != 0) { free(buf); win32_log("sndPlaySound(memory): invalid WAV"); return 0; }
        free(buf);
        return (uint32_t)snd_start(pcm, "<memory>", ARG(1));
    }
    gstr_get(n, sizeof n, ARG(0));
    return (uint32_t)snd_play(n, ARG(1));
}

/* ---- MCI ---------------------------------------------------------------------------------------------------- */
static Dev *dev_of(uint32_t id) {
    for (int i = 0; i < AU_NMCI; i++) if (devs[i].used && devs[i].id == (id & 0xFFFFu)) return &devs[i];
    return 0;
}
static int slot_of(const Dev *d) { return AU_MCI0 + (int)(d - devs); }
static void post_notify(uint32_t hwnd, uint32_t code, uint32_t id) { if (hwnd) user_post(hwnd, MM_MCINOTIFY, code, id); }
static void abort_pending(Dev *d, uint32_t code) {
    if (d->pending) { post_notify(d->notify_hwnd, code, d->id); d->pending = 0; }
}
void mci_poll(void) {
    for (int i = 0; i < AU_NMCI; i++) {
        if (!devs[i].used) continue;
        if (audio_voice_take_done(AU_MCI0 + i) && devs[i].pending) {
            post_notify(devs[i].notify_hwnd, 1, devs[i].id);
            devs[i].pending = 0;
        }
    }
}
static void wait_done(int slot) {
    for (int guard = 0; guard < 60000 && audio_voice_state(slot) == 1; guard++) {
        uint32_t w = audio_ms_until_event();
        audio_pump(win32_now_ms());
        if (opt_fixed_time_set) win32_advance_ms(w != 0xFFFFFFFFu && w > 1 ? w : 1); else host_sleep_ms(2);
    }
}
static uint32_t mci_cmd(uint32_t id, uint32_t msg, uint32_t flags, uint32_t parm) {
    Dev *d;
    if (msg == MCI_OPEN) {
        char type[64], el[300];
        Pcm pcm;
        int nf, i;
        if (!parm) return MCIERR_NULL_PARAMETER_BLOCK;
        type[0] = el[0] = 0;
        if ((flags & MCI_OPEN_TYPE) && R32(parm + 8) >= 0x10000u) gstr_get(type, sizeof type, R32(parm + 8));
        if (gstr_cmp_ci(type, "waveaudio") != 0) return MCIERR_INVALID_DEVICE_NAME;
        if (!(flags & MCI_OPEN_ELEMENT) || !R32(parm + 12)) return MCIERR_UNSUPPORTED_FUNCTION;
        gstr_get(el, sizeof el, R32(parm + 12));
        for (i = 0; i < AU_NMCI; i++) if (!devs[i].used) break;
        if (i == AU_NMCI) return MCIERR_OUT_OF_MEMORY;
        if (load_wav_file(el, &pcm, &nf)) { win32_log("MCI_OPEN('%s'): %s", el, nf ? "file does not exist" : "invalid WAV"); return nf ? MCIERR_FILE_NOT_FOUND : MCIERR_INVALID_FILE; }
        audio_voice_load(AU_MCI0 + i, pcm, el);
        devs[i].used = 1; devs[i].id = 0x7B69u + (uint32_t)i; devs[i].pending = 0; devs[i].notify_hwnd = 0;
        W16(parm + 4, devs[i].id);
        if ((flags & MCI_NOTIFY) && R32(parm)) post_notify(R32(parm), 1, devs[i].id);
        return 0;
    }
    d = dev_of(id);
    if (!d) return MCIERR_INVALID_DEVICE_ID;
    switch (msg) {
    case MCI_PLAY: {
        uint32_t from = 0xFFFFFFFFu, to = 0;
        if (!parm) return MCIERR_NULL_PARAMETER_BLOCK;
        if (flags & MCI_FROM) from = R32(parm + 4);
        if (flags & MCI_TO) to = R32(parm + 8);
        abort_pending(d, 2);                                    /* MCI_NOTIFY_SUPERSEDED */
        audio_voice_play(slot_of(d), from, to, 0);
        if (flags & MCI_NOTIFY) { d->pending = 1; d->notify_hwnd = R32(parm); }
        if (flags & MCI_WAIT) wait_done(slot_of(d));
        return 0;
    }
    case MCI_STOP: audio_voice_stop(slot_of(d)); abort_pending(d, 4); return 0;           /* ABORTED */
    case MCI_CLOSE: audio_voice_stop(slot_of(d)); abort_pending(d, 4); audio_voice_release(slot_of(d)); d->used = 0; return 0;
    case MCI_PAUSE: audio_voice_pause(slot_of(d), 1); return 0;
    case MCI_RESUME: audio_voice_pause(slot_of(d), 0); return 0;
    case MCI_SEEK: {
        uint32_t to = 0;
        if (flags & 0x100u) to = 0;
        else if (flags & 0x200u) to = audio_voice_len_ms(slot_of(d));
        else if (flags & MCI_TO) { if (!parm) return MCIERR_NULL_PARAMETER_BLOCK; to = R32(parm + 4); }
        abort_pending(d, 4);
        audio_voice_seek(slot_of(d), to);
        return 0;
    }
    case MCI_STATUS: {
        uint32_t item, v = 0;
        if (!parm) return MCIERR_NULL_PARAMETER_BLOCK;
        item = R32(parm + 8);
        switch (item) {
        case 1: v = audio_voice_len_ms(slot_of(d)); break;                       /* MCI_STATUS_LENGTH */
        case 2: v = audio_voice_pos_ms(slot_of(d)); break;                       /* POSITION */
        case 3: case 5: case 7: case 8: v = 1; break;                            /* tracks, media present, ready, current track */
        case 4: { int s = audio_voice_state(slot_of(d)); v = s == 1 ? 526u : s == 2 ? 529u : 525u; break; }   /* MODE: play/pause/stop */
        case 6: v = 0; break;                                                    /* TIME_FORMAT = ms */
        default: return MCIERR_UNSUPPORTED_FUNCTION;
        }
        W32(parm + 4, v);
        return 0;
    }
    case MCI_SET:
        if ((flags & 0x400u) && parm && R32(parm + 4) != 0) return MCIERR_BAD_TIME_FORMAT;   /* milliseconds only */
        return 0;
    default: return MCIERR_UNRECOGNIZED_COMMAND;
    }
}
static uint32_t wm_mciSendCommandA(void) { return mci_cmd(ARG(0), ARG(1), ARG(2), ARG(3)); }
static uint32_t wm_mciGetErrorStringA(void) {
    static const struct { uint32_t c; const char *s; } t[] = {
        {257, "Invalid MCI device ID."}, {261, "Unrecognized command."}, {263, "Invalid device name."},
        {264, "Out of memory."}, {265, "The device is already open."}, {274, "The device does not support that function."},
        {275, "File not found."}, {293, "Invalid time format."}, {296, "Invalid file."}, {297, "Null parameter block passed to MCI."}};
    const char *s = "Unknown error.";
    int ok = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].c == ARG(0)) { s = t[i].s; ok = 1; }
    if (ARG(1) && ARG(2)) gstr_putn(ARG(1), ARG(2), s);
    return (uint32_t)ok;
}

static uint32_t wm_waveOutOpen(void) {
    if (ARG(0)) W32(ARG(0), 0x00000A01u);               /* fake HWAVEOUT: the game only detects the device */
    return 0;                                           /* MMSYSERR_NOERROR */
}
static uint32_t wm_zero(void) { return 0; }

void winmm_register(void) {
#define R(n, f) win32_register("WINMM.dll!" n, f)
    R("sndPlaySoundA", wm_sndPlaySoundA); R("waveOutOpen", wm_waveOutOpen); R("waveOutClose", wm_zero);
    R("mciSendCommandA", wm_mciSendCommandA); R("mciGetErrorStringA", wm_mciGetErrorStringA);
#undef R
}
