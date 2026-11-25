#pragma once
/* Byte-limited copies (snprintf into a fixed buffer, buf[n] = 0) can split a multi-byte UTF-8 character; LVGL then
 * draws a box and JSON output carries invalid UTF-8. Call utf8_trim() after such a cut. */
#include <stddef.h>
#include <string.h>

/* Drop an incomplete multi-byte sequence at the end of s. Returns the new length. */
static inline size_t utf8_trim(char *s)
{
    size_t n = strlen(s);
    size_t i = n;
    int cont = 0;
    while (i > 0 && cont < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
        i--;
        cont++;
    }
    if (i == 0) return n;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && (size_t)cont + 1 < need) { /* lead byte + fewer continuation bytes than it announces */
        s[i - 1] = 0;
        return i - 1;
    }
    return n;
}

/* snprintf(dst, len, "%s", src) that never ends in half a character. */
static inline void utf8_copy(char *dst, size_t len, const char *src)
{
    if (!len) return;
    size_t n = strlen(src ? src : "");
    if (n >= len) n = len - 1;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
    utf8_trim(dst);
}
