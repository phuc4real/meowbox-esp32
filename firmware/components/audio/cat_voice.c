#include "cat_voice.h"

#include <math.h>

/* How a meow is made here: a sawtooth (band-limited with PolyBLEP) through a one-pole low-pass stands in for the
 * vocal folds; three band-pass resonators are the mouth, their frequencies gliding from "m/i" to an open "a" to a
 * rounded "ow" while the pitch rises and falls. Vibrato, a slow random wobble and some breath noise keep it from
 * sounding like a synth. A trill ("mrrp") is the same voice with its amplitude fluttering at ~28 Hz at the start. */

#define PI2 6.2831853f
#define BLK 256
#define CTRL 32      /* pitch and formants are updated every CTRL samples (0.7 ms at 44.1 kHz) */
#ifndef CV_NORM
#define CV_NORM 1.0f /* overall level; per-sound gains below are set from tools/cat_voice peaks */
#endif

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}
static float frand(uint32_t *s) { return (int32_t)rnd(s) * (1.0f / 2147483648.0f); } /* -1..1 */

typedef struct { float b0, b2, a1, a2, x1, x2, y1, y2; } bp_t; /* band-pass, 0 dB at the centre (RBJ cookbook) */

static void bp_set(bp_t *f, float hz, float q, int rate)
{
    float w = PI2 * hz / rate, al = sinf(w) / (2 * q), a0 = 1 + al;
    f->b0 = al / a0;
    f->b2 = -al / a0;
    f->a1 = -2 * cosf(w) / a0;
    f->a2 = (1 - al) / a0;
}

static float bp_run(bp_t *f, float x)
{
    float y = f->b0 * x + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

static float polyblep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}

static float ease(float x) { return x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
/* start -> mid -> end, the middle reached at m (0..1) */
static float curve3(float a, float b, float c, float m, float t)
{
    return t < m ? lerp(a, b, ease(t / m)) : lerp(b, c, ease((t - m) / (1 - m)));
}

static int16_t to_pcm(float y)
{
    if (y > 1) y = 1;
    if (y < -1) y = -1;
    return (int16_t)(y * 32767);
}

bool cv_call(const cv_call_t *c, int rate, float pitch, float stretch, uint32_t seed, cv_out_t out, void *ctx)
{
    int total = (int)(c->ms * stretch * rate / 1000);
    int att = c->attack_ms * rate / 1000, rel = c->release_ms * rate / 1000;
    int trill = (int)(c->trill_ms * stretch * rate / 1000);
    if (att < 1) att = 1;
    if (rel < 1) rel = 1;
    uint32_t s = (seed << 1) | 1;
    bp_t f1 = {0}, f2 = {0}, f3 = {0};
    bp_set(&f3, 3400, 9, rate); /* a fixed high resonance: a little brightness */
    float lpk = 1 - expf(-PI2 * 1800 / rate), lp = 0, ph = 0, wob = 0, f = c->f0[0] * pitch;
    float top = (c->f0[0] > c->f0[1] ? (c->f0[0] > c->f0[2] ? c->f0[0] : c->f0[2])
                                     : (c->f0[1] > c->f0[2] ? c->f0[1] : c->f0[2])) * pitch;
    int16_t buf[BLK];
    for (int done = 0; done < total; done += BLK) {
        int n = total - done < BLK ? total - done : BLK;
        for (int i = 0; i < n; i++) {
            int p = done + i;
            float t = (float)p / total;
            if (p % CTRL == 0) {
                wob += (frand(&s) * 0.012f - wob) * 0.08f; /* slow random pitch wobble, about +-0.5 % */
                f = curve3(c->f0[0], c->f0[1], c->f0[2], c->peak, t) * pitch;
                f *= 1 + c->vib * sinf(PI2 * 5.5f * p / rate) + wob;
                bp_set(&f1, curve3(c->fmt[0][0], c->fmt[1][0], c->fmt[2][0], c->peak, t), 4.5f, rate);
                bp_set(&f2, curve3(c->fmt[0][1], c->fmt[1][1], c->fmt[2][1], c->peak, t), 7, rate);
            }
            float dt = f / rate;
            ph += dt;
            if (ph >= 1) ph -= 1;
            float src = (2 * ph - 1 - polyblep(ph, dt)) * (1 - c->breath) + frand(&s) * c->breath;
            lp += (src - lp) * lpk;
            float v = bp_run(&f1, lp) + 0.7f * bp_run(&f2, lp) + 0.25f * bp_run(&f3, lp) + 0.08f * lp;
            float env = ease((float)p / att) * ease((float)(total - p) / rel);
            env *= 0.7f + 0.3f * f / top; /* louder where the voice is highest */
            if (p < trill) {               /* "rrr": the flutter fades out over its last 30 % */
                float depth = 0.85f * (1 - ease((p - 0.7f * trill) / (0.3f * trill)));
                float tr = 0.5f + 0.5f * cosf(PI2 * c->trill_hz * p / rate);
                env *= 1 - depth * tr * tr;
            }
            buf[i] = to_pcm(v * env * c->gain * CV_NORM);
        }
        if (!out(buf, n, ctx)) return false;
    }
    return true;
}

bool cv_purr(int ms, int rate, float gain, uint32_t seed, cv_out_t out, void *ctx)
{
    /* one breath: out for 0.6 s at 26 Hz, a short pause, in for 0.45 s at 22 Hz (softer), a pause */
    static const struct { float s, hz, amp; } part[] = {{0.60f, 26, 1}, {0.08f, 0, 0}, {0.45f, 22, 0.6f}, {0.10f, 0, 0}};
    const float cycle = 1.23f;
    uint32_t s = (seed << 1) | 1;
    bp_t lo = {0}, mid = {0};
    bp_set(&lo, 170, 1.5f, rate);
    bp_set(&mid, 430, 2.5f, rate);
    int total = ms * rate / 1000, fade = rate / 30;
    float ph = 0, l1 = 0, l2 = 0, lk = 1 - expf(-PI2 * 800 / rate); /* two low-passes: a rumble, not a hiss */
    int16_t buf[BLK];
    for (int done = 0; done < total; done += BLK) {
        int n = total - done < BLK ? total - done : BLK;
        for (int i = 0; i < n; i++) {
            int p = done + i;
            float tc = fmodf((float)p / rate, cycle), hz = 0, amp = 0, x = 0;
            for (int k = 0; k < 4; k++) {
                if (tc < part[k].s) { hz = part[k].hz; amp = part[k].amp; x = tc / part[k].s; break; }
                tc -= part[k].s;
            }
            float noise = frand(&s), v = bp_run(&lo, noise) + 0.5f * bp_run(&mid, noise);
            l1 += (v - l1) * lk;
            l2 += (l1 - l2) * lk;
            v = l2;
            float y = 0;
            if (hz > 0) {
                ph += hz / rate;
                if (ph >= 1) ph -= 1;
                float k = 1 - ph; /* each pulse: quick start, then dies away */
                y = v * k * k * k * k * amp * sinf(3.14159f * x);
            }
            if (p < fade) y *= (float)p / fade;
            if (total - p < fade) y *= (float)(total - p) / fade;
            buf[i] = to_pcm(y * gain * 3.0f);
        }
        if (!out(buf, n, ctx)) return false;
    }
    return true;
}

bool cv_silence(int ms, int rate, cv_out_t out, void *ctx)
{
    int16_t buf[BLK] = {0};
    for (int left = ms * rate / 1000; left > 0; left -= BLK)
        if (!out(buf, left < BLK ? left : BLK, ctx)) return false;
    return true;
}

/* ---------------- the sounds ---------------- */
/* {ms, pitch start/peak/end, where it peaks, formants F1,F2 start/middle/end,
 *  attack, release, trill Hz, trill ms, vibrato, breath, gain} */
static const cv_call_t MEOW = {620, {560, 860, 470}, 0.38f, {{400, 2200}, {1150, 1900}, {600, 1000}},
                               45, 160, 0, 0, 0.012f, 0.06f, 0.70f};
static const cv_call_t MEOW_LONG = {760, {600, 1000, 520}, 0.40f, {{420, 2300}, {1250, 2000}, {620, 1000}},
                                    40, 200, 0, 0, 0.015f, 0.07f, 1.0f};
static const cv_call_t MEW = {260, {900, 1250, 1000}, 0.45f, {{500, 2400}, {1000, 2300}, {750, 1600}},
                              20, 90, 0, 0, 0.015f, 0.05f, 0.6f};
static const cv_call_t CHIRP = {380, {470, 620, 840}, 0.50f, {{420, 1300}, {800, 1700}, {950, 2300}},
                                25, 80, 28, 230, 0.008f, 0.05f, 0.78f};
static const cv_call_t DEMAND = {330, {680, 1080, 640}, 0.32f, {{500, 1700}, {1300, 2200}, {700, 1200}},
                                 12, 70, 30, 60, 0.02f, 0.10f, 0.86f};
static const cv_call_t MIP = {70, {1250, 1450, 1300}, 0.40f, {{600, 2400}, {900, 2600}, {800, 2400}},
                              6, 35, 0, 0, 0, 0.03f, 1.64f};
static const cv_call_t MUP = {70, {650, 760, 640}, 0.40f, {{500, 1200}, {800, 1400}, {600, 1000}}, 6, 35, 0, 0, 0, 0.03f, 0.46f};

bool cv_play(cv_sound_t snd, int rate, uint32_t seed, cv_out_t out, void *ctx)
{
    switch (snd) {
    case CV_MIP: return cv_call(&MIP, rate, 1, 1, seed, out, ctx);
    case CV_MUP: return cv_call(&MUP, rate, 1, 1, seed, out, ctx);
    case CV_BOOT:
        return cv_call(&CHIRP, rate, 1.05f, 1, seed, out, ctx) && cv_silence(40, rate, out, ctx) &&
               cv_call(&MEW, rate, 1, 1, seed + 1, out, ctx);
    case CV_CHIRP: return cv_call(&CHIRP, rate, 1, 1, seed, out, ctx);
    case CV_DEMAND:
        return cv_call(&DEMAND, rate, 1, 1, seed, out, ctx) && cv_silence(70, rate, out, ctx) &&
               cv_call(&DEMAND, rate, 1.06f, 1, seed + 1, out, ctx);
    case CV_ALARM: /* 560 + 140 + 760 + 540 ms = 2 s */
        return cv_call(&MEOW, rate, 1, 0.9f, seed, out, ctx) && cv_silence(140, rate, out, ctx) &&
               cv_call(&MEOW_LONG, rate, 1, 1, seed + 1, out, ctx) && cv_silence(540, rate, out, ctx);
    case CV_MEOW: { /* a different cat mood every time: pitch and length within +-7 % */
        uint32_t s = (seed << 1) | 1;
        float pitch = 1 + (int)(rnd(&s) % 15 - 7) / 100.0f, stretch = 1 + (int)(rnd(&s) % 15 - 7) / 100.0f;
        return cv_call(&MEOW, rate, pitch, stretch, seed, out, ctx);
    }
    case CV_PURR: return cv_purr(2460, rate, 0.9f, seed, out, ctx);
    default: return true;
    }
}

const char *cv_name(cv_sound_t s)
{
    static const char *const n[] = {"mip", "mup", "boot", "chirp", "demand", "alarm", "meow", "purr"};
    return s < CV_COUNT ? n[s] : "?";
}
