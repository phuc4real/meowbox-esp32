/* Renders the speaker's cat sounds (components/audio/cat_voice.c, the same code the firmware runs) to WAV files, to
 * listen to them on a PC or send them to the speaker with POST /api/notify/audio before flashing.
 *
 *   gcc -O2 -I ../../components/audio/include render.c ../../components/audio/cat_voice.c -lm -o render
 *   ./render out_dir [seed]      -> out_dir/<name>.wav (44.1 kHz mono), and the peak level of each
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cat_voice.h"

#define RATE 44100

typedef struct { FILE *f; long n; int peak; } wav_t;

static bool sink(const int16_t *pcm, int n, void *ctx)
{
    wav_t *w = ctx;
    for (int i = 0; i < n; i++) {
        int a = abs(pcm[i]);
        if (a > w->peak) w->peak = a;
        unsigned char b[2] = {(unsigned char)(pcm[i] & 0xFF), (unsigned char)((pcm[i] >> 8) & 0xFF)};
        fwrite(b, 1, 2, w->f);
    }
    w->n += n;
    return true;
}

static void le32(FILE *f, unsigned v) { unsigned char b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void le16(FILE *f, unsigned v) { unsigned char b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }

static void header(FILE *f, long samples)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); le32(f, 36 + samples * 2); fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, 1); le32(f, RATE); le32(f, RATE * 2); le16(f, 2); le16(f, 16);
    fwrite("data", 1, 4, f); le32(f, samples * 2);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s out_dir [seed]\n", argv[0]); return 2; }
    unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 12345;
    for (int s = 0; s < CV_COUNT; s++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.wav", argv[1], cv_name(s));
        wav_t w = {fopen(path, "wb"), 0, 0};
        if (!w.f) { perror(path); return 1; }
        header(w.f, 0);
        cv_play(s, RATE, seed + s, sink, &w);
        header(w.f, w.n);
        fclose(w.f);
        printf("%-7s %5ld ms  peak %5.1f %%%s\n", cv_name(s), w.n * 1000 / RATE, w.peak * 100.0 / 32767,
               w.peak >= 32767 ? "  CLIPS" : "");
    }
    return 0;
}
