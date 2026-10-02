/*
 * miyoofin-player subtitle logic: SRT parsing, cue lookup and the track list
 * file written by the playback reporter. Plain C99 with no FFmpeg/SDL
 * dependency so it also compiles as C++ and is unit-tested on the host
 * (tests/test_player.cpp).
 */
#ifndef MIYOOFIN_PLAYER_SUBS_H
#define MIYOOFIN_PLAYER_SUBS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct SubCue {
    int64_t start_ms, end_ms;
    char *text; /* '\n' separated lines, markup removed */
} SubCue;

typedef struct SubCues {
    SubCue *cues;
    int n, cap;
} SubCues;

typedef struct SubTrackInfo {
    char type;      /* 'a' audio, 's' subtitle */
    int index;      /* Jellyfin's global stream index */
    int text;       /* subtitle can be fetched as SRT */
    int is_default, forced;
    char lang[16];
    char title[64];
} SubTrackInfo;

static inline void subs_free(SubCues *c)
{
    int i;
    for (i = 0; i < c->n; i++)
        free(c->cues[i].text);
    free(c->cues);
    c->cues = NULL;
    c->n = c->cap = 0;
}

/* "hh:mm:ss,mmm" (or '.') at p; returns pointer past it or NULL. */
static inline const char *subs_parse_time(const char *p, int64_t *ms)
{
    int h = 0, m = 0, s = 0, f = 0, digits = 0;
    char *end;
    h = (int)strtol(p, &end, 10);
    if (end == p || *end != ':')
        return NULL;
    p = end + 1;
    m = (int)strtol(p, &end, 10);
    if (end == p || *end != ':')
        return NULL;
    p = end + 1;
    s = (int)strtol(p, &end, 10);
    if (end == p || (*end != ',' && *end != '.'))
        return NULL;
    p = end + 1;
    while (*p >= '0' && *p <= '9') {
        if (digits < 3) {
            f = f * 10 + (*p - '0');
            digits++;
        }
        p++;
    }
    if (!digits)
        return NULL;
    while (digits++ < 3)
        f *= 10;
    *ms = ((int64_t)h * 3600 + m * 60 + s) * 1000 + f;
    return p;
}

/* Copies [src, src+len) into dst without <...> and {...} markup; "\N" and "\n"
 * escapes become line breaks. Returns the new length. */
static inline size_t subs_strip_markup(const char *src, size_t len, char *dst)
{
    size_t i, o = 0;
    for (i = 0; i < len; i++) {
        char c = src[i];
        if (c == '<') {
            while (i < len && src[i] != '>')
                i++;
        } else if (c == '{') {
            while (i < len && src[i] != '}')
                i++;
        } else if (c == '\\' && i + 1 < len && (src[i + 1] == 'N' || src[i + 1] == 'n')) {
            dst[o++] = '\n';
            i++;
        } else if (c != '\r') {
            dst[o++] = c;
        }
    }
    dst[o] = 0;
    return o;
}

static inline int subs_add(SubCues *c, int64_t start, int64_t end, const char *text, size_t len)
{
    char *t;
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 256;
        SubCue *grown = (SubCue *)realloc(c->cues, (size_t)cap * sizeof(SubCue));
        if (!grown)
            return 0;
        c->cues = grown;
        c->cap = cap;
    }
    t = (char *)malloc(len + 1);
    if (!t)
        return 0;
    subs_strip_markup(text, len, t);
    /* trim trailing/leading whitespace lines */
    {
        size_t n = strlen(t);
        while (n && (t[n - 1] == '\n' || t[n - 1] == ' '))
            t[--n] = 0;
    }
    if (!t[0]) {
        free(t);
        return 1; /* nothing to show; not an error */
    }
    c->cues[c->n].start_ms = start;
    c->cues[c->n].end_ms = end;
    c->cues[c->n].text = t;
    c->n++;
    return 1;
}

/* Parses SRT text; tolerant of BOM, CRLF, missing indices and blank-line
 * variants. Cues are sorted by start time. Returns the cue count (0 on junk). */
static inline int subs_parse_srt(const char *data, size_t len, SubCues *out)
{
    const char *p = data, *end = data + len;
    memset(out, 0, sizeof(*out));
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        p += 3;
    while (p < end) {
        const char *line = p, *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *arrow;
        int64_t start, stop;
        size_t linelen;
        if (!eol)
            eol = end;
        linelen = (size_t)(eol - line);
        p = eol < end ? eol + 1 : end;
        {
            char buf[96];
            const char *t;
            size_t n = linelen < sizeof(buf) - 1 ? linelen : sizeof(buf) - 1;
            memcpy(buf, line, n);
            buf[n] = 0;
            arrow = strstr(buf, "-->");
            if (!arrow)
                continue;
            t = subs_parse_time(buf, &start);
            if (!t)
                continue;
            t = subs_parse_time(arrow + 3 + strspn(arrow + 3, " \t"), &stop);
            if (!t)
                continue;
        }
        /* text: following lines until a blank line */
        {
            const char *text = p;
            size_t tl = 0;
            while (p < end) {
                const char *l2 = p, *e2 = (const char *)memchr(p, '\n', (size_t)(end - p));
                size_t l2len;
                if (!e2)
                    e2 = end;
                l2len = (size_t)(e2 - l2);
                if (l2len == 0 || (l2len == 1 && l2[0] == '\r'))
                    break;
                p = e2 < end ? e2 + 1 : end;
                tl = (size_t)(p - text);
            }
            if (tl && text[tl - 1] == '\n')
                tl--;
            if (!subs_add(out, start, stop, text, tl)) {
                subs_free(out);
                return 0;
            }
        }
    }
    /* insertion sort by start (input is almost always sorted already) */
    {
        int i, j;
        for (i = 1; i < out->n; i++) {
            SubCue key = out->cues[i];
            for (j = i - 1; j >= 0 && out->cues[j].start_ms > key.start_ms; j--)
                out->cues[j + 1] = out->cues[j];
            out->cues[j + 1] = key;
        }
    }
    return out->n;
}

/* Index of the cue showing at `ms` (+ offset applied by the caller), or -1.
 * Overlapping cues: the latest-starting one wins. */
static inline int subs_find(const SubCues *c, int64_t ms)
{
    int lo = 0, hi = c->n - 1, best = -1, i;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c->cues[mid].start_ms <= ms) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    for (i = best; i >= 0 && i > best - 8; i--)
        if (ms < c->cues[i].end_ms)
            return i;
    return -1;
}

/* Parses the reporter's playback-tracks.txt ("type|index|text|default|forced|lang|title"
 * per line). Returns the number of tracks stored (at most `max`). */
static inline int subs_parse_tracks(const char *data, size_t len, SubTrackInfo *out, int max)
{
    const char *p = data, *end = data + len;
    int n = 0;
    while (p < end && n < max) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
        char line[160];
        size_t l;
        char *f[7];
        int k = 0;
        char *q;
        if (!eol)
            eol = end;
        l = (size_t)(eol - p);
        if (l >= sizeof(line))
            l = sizeof(line) - 1;
        memcpy(line, p, l);
        line[l] = 0;
        p = eol < end ? eol + 1 : end;
        f[k++] = line;
        for (q = line; *q && k < 7; q++)
            if (*q == '|') {
                *q = 0;
                f[k++] = q + 1;
            }
        if (k < 7 || (f[0][0] != 'a' && f[0][0] != 's'))
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].type = f[0][0];
        out[n].index = atoi(f[1]);
        out[n].text = f[2][0] == '1';
        out[n].is_default = f[3][0] == '1';
        out[n].forced = f[4][0] == '1';
        strncpy(out[n].lang, f[5], sizeof(out[n].lang) - 1);
        strncpy(out[n].title, f[6], sizeof(out[n].title) - 1);
        n++;
    }
    return n;
}

#endif /* MIYOOFIN_PLAYER_SUBS_H */
