/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_text.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Punctuation and symbols, and the letters that take two, by code point. */
static const struct {
    uint16_t cp;
    char ascii[4];
} STAND_INS[] = {
    { 0x00A0, " " },     /* no-break space */
    { 0x00A1, "!" },
    { 0x00A2, "c" },     /* cent */
    { 0x00A3, "GBP" },
    { 0x00A5, "Y" },     /* yen */
    { 0x00A6, "|" },
    { 0x00A7, "S" },     /* section */
    { 0x00A9, "(c)" },
    { 0x00AB, "<<" },
    { 0x00AD, "" },      /* soft hyphen */
    { 0x00AE, "(R)" },
    { 0x00B0, "o" },     /* degree */
    { 0x00B1, "+-" },
    { 0x00B2, "2" },
    { 0x00B3, "3" },
    { 0x00B4, "'" },
    { 0x00B5, "u" },     /* micro */
    { 0x00B7, "." },     /* middle dot */
    { 0x00B9, "1" },
    { 0x00BB, ">>" },
    { 0x00BC, "1/4" },
    { 0x00BD, "1/2" },
    { 0x00BE, "3/4" },
    { 0x00BF, "?" },
    { 0x00C6, "AE" },
    { 0x00DE, "Th" },
    { 0x00DF, "ss" },
    { 0x00E6, "ae" },
    { 0x00FE, "th" },
    { 0x0132, "IJ" },
    { 0x0133, "ij" },
    { 0x0149, "'n" },
    { 0x0152, "OE" },
    { 0x0153, "oe" },
    { 0x01A0, "O" },     /* Vietnamese O and U with horn */
    { 0x01A1, "o" },
    { 0x01AF, "U" },
    { 0x01B0, "u" },
    { 0x02BC, "'" },     /* modifier apostrophe */
    { 0x02C6, "^" },
    { 0x02DC, "~" },
    { 0x2010, "-" },     /* hyphen */
    { 0x2011, "-" },     /* non-breaking hyphen */
    { 0x2012, "-" },     /* figure dash */
    { 0x2013, "-" },     /* en dash */
    { 0x2014, "--" },    /* em dash */
    { 0x2015, "--" },    /* horizontal bar */
    { 0x2016, "||" },
    { 0x2018, "'" },     /* single quotes */
    { 0x2019, "'" },
    { 0x201A, "," },
    { 0x201B, "'" },
    { 0x201C, "\"" },    /* double quotes */
    { 0x201D, "\"" },
    { 0x201E, "\"" },
    { 0x201F, "\"" },
    { 0x2020, "+" },     /* dagger */
    { 0x2022, "*" },     /* bullet */
    { 0x2023, ">" },
    { 0x2024, "." },
    { 0x2025, ".." },
    { 0x2026, "..." },   /* ellipsis */
    { 0x2027, "-" },
    { 0x2028, " " },     /* line and paragraph separators */
    { 0x2029, " " },
    { 0x202F, " " },     /* narrow no-break space */
    { 0x2032, "'" },     /* prime */
    { 0x2033, "\"" },
    { 0x2039, "<" },
    { 0x203A, ">" },
    { 0x203C, "!!" },
    { 0x2044, "/" },
    { 0x2047, "??" },
    { 0x2048, "?!" },
    { 0x2049, "!?" },
    { 0x205F, " " },
    { 0x2060, "" },      /* word joiner */
    { 0x20AC, "EUR" },
    { 0x2122, "TM" },
    { 0x2190, "<-" },    /* arrows */
    { 0x2191, "^" },
    { 0x2192, "->" },
    { 0x2193, "v" },
    { 0x2194, "<->" },
    { 0x21D0, "<=" },
    { 0x21D2, "=>" },
    { 0x21D4, "<=>" },
    { 0x2212, "-" },     /* minus */
    { 0x2215, "/" },
    { 0x2217, "*" },
    { 0x2248, "~" },
    { 0x2260, "!=" },
    { 0x2264, "<=" },
    { 0x2265, ">=" },
    { 0x2713, "v" },     /* check marks and crosses */
    { 0x2714, "v" },
    { 0x2717, "x" },
    { 0x2718, "x" },
    { 0xFEFF, "" },      /* byte order mark */
};

/* U+00C0-017F by their plain letter; '_' where STAND_INS has two. */
static const char LATIN[] =
    "AAAAAA_CEEEEIIIIDNOOOOOxOUUUUY__aaaaaa_ceeeeiiiidnooooo/ouuuuy_y"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi__JjKkkLlLlLlLlLl"
    "NnNnNn_NnOoOoOo__RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

/* U+1EA0-1EF9, Vietnamese letters with a tone mark (and a hat, breve or
 * horn), by their plain letter. */
static const char VIETNAMESE[] =
    "AaAaAaAaAaAaAaAaAaAaAaAaEeEeEeEeEeEeEeEeIiIiOoOoOoOoOoOoOoOoOoOoOoOo"
    "UuUuUuUuUuUuUuYyYyYyYy";

/* The code point at s, and its length in *len; -1 if it's broken. */
static int32_t decode(const unsigned char *s, size_t *len)
{
    if (s[0] >= 0x80 && s[0] < 0xC0) {
        *len = 1;   /* a stray continuation byte */
        return -1;
    }
    size_t n = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 1;
    int32_t cp = n == 1 ? s[0] : s[0] & (0x7F >> n);
    size_t i = 1;
    for (; i < n && (s[i] & 0xC0) == 0x80; i++) {
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *len = i;
    return i == n ? cp : -1;
}

static const char *stand_in(int32_t cp)
{
    size_t lo = 0, hi = sizeof(STAND_INS) / sizeof(STAND_INS[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (STAND_INS[mid].cp == cp) {
            return STAND_INS[mid].ascii;
        }
        if (STAND_INS[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (cp >= 0x2000 && cp <= 0x200A) {
        return " ";   /* spaces of other widths */
    }
    if ((cp >= 0x0300 && cp <= 0x036F)      /* accents typed after their letter */
        || (cp >= 0x200B && cp <= 0x200F)   /* zero-width spaces and joiners, direction marks */
        || cp == 0x20E3                     /* keycap */
        || (cp >= 0x2600 && cp <= 0x27BF)   /* symbols and dingbats */
        || (cp >= 0x2B00 && cp <= 0x2BFF)   /* more arrows, stars */
        || (cp >= 0xFE00 && cp <= 0xFE0F)   /* emoji or text style */
        || (cp >= 0x1F000 && cp <= 0x1FAFF) /* emoji */
        || (cp >= 0xE0000 && cp <= 0xE007F)) {
        return "";
    }
    return NULL;
}

int muse_text_ascii(const char *s, size_t *len, char out[4])
{
    int32_t cp = decode((const unsigned char *)s, len);
    if (cp < 0x80) {
        return -1;   /* ASCII, or broken */
    }
    if (cp >= 0xC0 && cp <= 0x17F && LATIN[cp - 0xC0] != '_') {
        out[0] = LATIN[cp - 0xC0];
        out[1] = '\0';
        return 1;
    }
    if (cp >= 0x1EA0 && cp <= 0x1EF9) {
        out[0] = VIETNAMESE[cp - 0x1EA0];
        out[1] = '\0';
        return 1;
    }
    const char *a = stand_in(cp);
    if (!a) {
        return -1;
    }
    strlcpy(out, a, 4);
    return (int)strlen(out);
}

muse_text_cjk_t muse_text_cjk(const char *s)
{
    static const uint16_t CLOSE[] = {
        0x3001, 0x3002, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0x3017, 0x3019,
        0x301B, 0x30FB, 0x30FC, 0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F,
        0xFF3D, 0xFF5D, 0xFF60,
    };
    size_t len;
    int32_t cp = decode((const unsigned char *)s, &len);
    for (size_t i = 0; i < sizeof(CLOSE) / sizeof(CLOSE[0]); i++) {
        if (cp == CLOSE[i]) {
            return MUSE_TEXT_CJK_CLOSE;
        }
    }
    bool cjk = (cp >= 0x2E80 && cp <= 0x9FFF)      /* radicals, punctuation, kana, ideographs */
               || (cp >= 0xAC00 && cp <= 0xD7AF)   /* Hangul */
               || (cp >= 0xF900 && cp <= 0xFAFF)   /* compatibility ideographs */
               || (cp >= 0xFF00 && cp <= 0xFFEF);  /* fullwidth forms */
    return cjk ? MUSE_TEXT_CJK : MUSE_TEXT_NOT_CJK;
}

bool muse_text_has_cjk(const char *s)
{
    while (*s) {
        if (muse_text_cjk(s) != MUSE_TEXT_NOT_CJK) {
            return true;
        }
        size_t len;
        decode((const unsigned char *)s, &len);
        s += len;
    }
    return false;
}

void muse_text_to_ascii(char *s, size_t cap)
{
    size_t n = strlen(s);
    for (char *p = s; *p;) {
        size_t len;
        char a[4];
        int alen = muse_text_ascii(p, &len, a);
        if (alen < 0) {
            p += len;
            continue;
        }
        if (n - len + alen >= cap) {
            *p = '\0';
            return;
        }
        memmove(p + alen, p + len, n - (p - s) - len + 1);
        memcpy(p, a, alen);
        n = n - len + alen;
        p += alen;
    }
}

bool muse_text_has_devanagari(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; p[0] && p[1]; p++) {
        if (p[0] == 0xE0 && (p[1] == 0xA4 || p[1] == 0xA5)) {
            return true;
        }
    }
    return false;
}

/* A Devanagari consonant's stem (its sound with no vowel), or NULL. */
static const char *deva_stem(int32_t cp)
{
    switch (cp) {
    case 0x0915: return "k";   case 0x0916: return "kh";  case 0x0917: return "g";
    case 0x0918: return "gh";  case 0x0919: return "ng";  case 0x091A: return "ch";
    case 0x091B: return "chh"; case 0x091C: return "j";   case 0x091D: return "jh";
    case 0x091E: return "ny";  case 0x091F: return "t";   case 0x0920: return "th";
    case 0x0921: return "d";   case 0x0922: return "dh";  case 0x0923: return "n";
    case 0x0924: return "t";   case 0x0925: return "th";  case 0x0926: return "d";
    case 0x0927: return "dh";  case 0x0928: return "n";   case 0x092A: return "p";
    case 0x092B: return "ph";  case 0x092C: return "b";   case 0x092D: return "bh";
    case 0x092E: return "m";   case 0x092F: return "y";   case 0x0930: return "r";
    case 0x0932: return "l";   case 0x0935: return "w";   case 0x0936: return "sh";
    case 0x0937: return "sh";  case 0x0938: return "s";   case 0x0939: return "h";
    case 0x0958: return "q";   case 0x0959: return "kh";  case 0x095A: return "g";
    case 0x095B: return "z";   case 0x095C: return "r";   case 0x095D: return "rh";
    case 0x095E: return "f";   case 0x095F: return "y";
    default: return NULL;
    }
}

/* A vowel sign's (matra's) vowel, or NULL if cp is not a vowel sign. */
static const char *deva_matra(int32_t cp)
{
    switch (cp) {
    case 0x093A: case 0x093B: return "o";
    case 0x093E: return "a"; case 0x093F: return "i";  case 0x0940: return "ee";
    case 0x0941: return "u";  case 0x0942: return "oo"; case 0x0943: return "ri";
    case 0x0944: return "ree"; case 0x0945: case 0x0946: case 0x0947: return "e";
    case 0x0948: return "ai"; case 0x0949: case 0x094A: return "o";
    case 0x094B: return "o";  case 0x094C: return "au"; case 0x094E: case 0x094F: return "e";
    case 0x0955: return "e";  case 0x0956: return "ai"; case 0x0957: return "o";
    default: return NULL;
    }
}

/* An independent vowel's sound, or NULL. */
static const char *deva_vowel(int32_t cp)
{
    switch (cp) {
    case 0x0905: return "a";  case 0x0906: return "aa"; case 0x0907: return "i";
    case 0x0908: return "ee"; case 0x0909: return "u";  case 0x090A: return "oo";
    case 0x090B: return "ri"; case 0x090C: return "li"; case 0x090D: case 0x090E:
    case 0x090F: return "e";  case 0x0910: return "ai"; case 0x0911: case 0x0912:
    case 0x0913: return "o";  case 0x0914: return "au";
    default: return NULL;
    }
}

size_t muse_text_to_hinglish(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!cap) {
        return 0;
    }
    const unsigned char *p = (const unsigned char *)in;
    while (*p) {
        size_t len;
        int32_t cp = decode(p, &len);
        const char *stem = cp >= 0 ? deva_stem(cp) : NULL;
        if (!stem) {
            const char *v = cp >= 0 ? deva_vowel(cp) : NULL;
            const char *one = NULL;
            char digit[2] = {};
            if (v) {
                one = v;
            } else if (cp == 0x0901 || cp == 0x0902) {
                one = "n";   /* chandrabindu, anusvara */
            } else if (cp == 0x0903) {
                one = "h";   /* visarga */
            } else if (cp == 0x0964 || cp == 0x0965) {
                one = ".";   /* danda */
            } else if (cp >= 0x0966 && cp <= 0x096F) {
                digit[0] = (char)('0' + cp - 0x0966);
                digit[1] = '\0';
                one = digit;
            } else if (cp == 0x0950) {
                one = "Om";
            } else if (deva_matra(cp)) {
                one = deva_matra(cp);   /* a sign with no consonant before it */
            }
            if (one) {
                size_t n = strlen(one);
                if (o + n >= cap) {
                    break;
                }
                memcpy(out + o, one, n);
                o += n;
            } else if (cp < 0x0900 || cp > 0x097F) {
                if (o + len >= cap) {
                    break;
                }
                memcpy(out + o, p, len);   /* Latin, punctuation, English words: as is */
                o += len;
            }
            /* Anything else in the block (virama, nukta, joiner signs) adds nothing alone. */
            p += len;
            continue;
        }
        /* Consonant: stem, then its vowel from what follows. */
        size_t n = strlen(stem);
        if (o + n + 4 >= cap) {
            break;
        }
        memcpy(out + o, stem, n);
        o += n;
        p += len;
        if (*p) {
            size_t nlen;
            int32_t next = decode(p, &nlen);
            if (next == 0x093C) {
                p += nlen;   /* nukta: the stem already covers the common forms */
                if (*p) {
                    next = decode(p, &nlen);
                } else {
                    next = -1;
                }
            }
            const char *matra = next >= 0 ? deva_matra(next) : NULL;
            if (next == 0x094D) {
                p += nlen;   /* virama: no vowel */
            } else if (matra) {
                size_t m = strlen(matra);
                memcpy(out + o, matra, m);
                o += m;
                p += nlen;
            } else if (next == ' ' || next == '\n' || next == '.' || next == ',' || next == '!' ||
                       next == '?' || next < 0) {
                /* Word end: Hindi drops the inherent vowel. */
            } else {
                out[o++] = 'a';
            }
        }
        /* End of text after a consonant: no inherent vowel either. */
    }
    out[o] = '\0';
    return o;
}

const char *muse_text_showable(const char *text, char *buf, size_t cap)
{
    const char *p = text;
    while (*p && !(*p & 0x80)) {
        p++;
    }
    if (!*p || strlcpy(buf, text, cap) >= cap) {
        return text;
    }
    muse_text_to_ascii(buf, cap);
    return buf;
}
