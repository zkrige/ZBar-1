/*------------------------------------------------------------------------
 *  Copyright 2007 (c) Jeff Brown <spadix@users.sourceforge.net>
 *
 *  This file is part of the Zebra Barcode Library.
 *
 *  The Zebra Barcode Library is free software; you can redistribute it
 *  and/or modify it under the terms of the GNU Lesser Public License as
 *  published by the Free Software Foundation; either version 2.1 of
 *  the License, or (at your option) any later version.
 *
 *  The Zebra Barcode Library is distributed in the hope that it will be
 *  useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *  of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser Public License
 *  along with the Zebra Barcode Library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 *  Boston, MA  02110-1301  USA
 *
 *  http://sourceforge.net/projects/zebra
 *------------------------------------------------------------------------*/

#include <config.h>
#include <assert.h>

#include <zebra.h>
#include "decoder.h"

#ifdef DEBUG_EAN
# define DEBUG_LEVEL (DEBUG_EAN)
#endif
#include "debug.h"

/* partial decode symbol location */
typedef enum symbol_partial_e {
    EAN_LEFT   = 0x0000,
    EAN_RIGHT  = 0x1000,
} symbol_partial_t;

/* convert compact encoded D2E1E2 to character (bit4 is parity) */
static const unsigned char digits[] = {  /* E1   E2 */
    0x06, 0x10, 0x04, 0x13,              /*  2  2-5 */
    0x19, 0x08, 0x11, 0x05,              /*  3  2-5 (d2 <= thr) */
    0x09, 0x12, 0x07, 0x15,              /*  4  2-5 (d2 <= thr) */
    0x16, 0x00, 0x14, 0x03,              /*  5  2-5 */
    0x18, 0x01, 0x02, 0x17,              /* E1E2=43,44,33,34 (d2 > thr) */
};

static const unsigned char parity_decode[] = {
    0xf0, /* [xx] BBBBBB = RIGHT half EAN-13 */

    /* UPC-E check digit encoding */
    0xff,
    0xff,
    0x0f, /* [07] BBBAAA = 0 */
    0xff,
    0x1f, /* [0b] BBABAA = 1 */
    0x2f, /* [0d] BBAABA = 2 */
    0xf3, /* [0e] BBAAAB = 3 */
    0xff,
    0x4f, /* [13] BABBAA = 4 */
    0x7f, /* [15] BABABA = 7 */
    0xf8, /* [16] BABAAB = 8 */
    0x5f, /* [19] BAABBA = 5 */
    0xf9, /* [1a] BAABAB = 9 */
    0xf6, /* [1c] BAAABB = 6 */
    0xff,

    /* LEFT half EAN-13 leading digit */
    0xff,
    0x6f, /* [23] ABBBAA = 6 */
    0x9f, /* [25] ABBABA = 9 */
    0xf5, /* [26] ABBAAB = 5 */
    0x8f, /* [29] ABABBA = 8 */
    0xf7, /* [2a] ABABAB = 7 */
    0xf4, /* [2c] ABAABB = 4 */
    0xff,
    0x3f, /* [31] AABBBA = 3 */
    0xf2, /* [32] AABBAB = 2 */
    0xf1, /* [34] AABABB = 1 */
    0xff,
    0xff,
    0xff,
    0xff,
    0x0f, /* [3f] AAAAAA = 0 */
};

#ifdef DEBUG_EAN
static unsigned char debug_buf[0x18];

static inline const unsigned char *dsprintbuf(ean_decoder_t *ean)
{
    int i;
    for(i = 0; i < 7; i++)
        debug_buf[i] = ((ean->buf[0] < 0 || ean->buf[i] < 0)
                        ? '-'
                        : ean->buf[i] + '0');
    debug_buf[i] = ' ';
    for(; i < 13; i++)
        debug_buf[i + 1] = ((ean->buf[7] < 0 || ean->buf[i] < 0)
                            ? '-'
                            : ean->buf[i] + '0');
    debug_buf[i + 1] = ' ';
    for(; i < 18; i++)
        debug_buf[i + 2] = ((ean->buf[13] < 0 || ean->buf[i] < 0)
                            ? '-'
                            : ean->buf[i] + '0');
    debug_buf[i + 2] = '\0';
    return(debug_buf);
}
#endif

/* evaluate previous N (>= 2) widths as auxiliary pattern,
 * using preceding 4 as character width
 */
static inline signed char aux_end (zebra_decoder_t *dcode,
                                   unsigned char n)
{
    /* reference width from previous character */
    unsigned s = calc_s(dcode, n, 4);

    dprintf(2, " (");
    signed char code = 0;
    unsigned char i;
    for(i = 0; i < n - 1; i++) {
        unsigned e = get_width(dcode, i) + get_width(dcode, i + 1);
        dprintf(2, " %d", e);
        code = (code << 2) | decode_e(e, s, 7);
        if(code < 0)
            return(-1);
    }
    dprintf(2, ") s=%d aux=%x", s, code);
    return(code);
}

/* determine possible auxiliary pattern
 * using current 4 as possible character
 */
static inline signed char aux_start (zebra_decoder_t *dcode)
{
    /* FIXME NB add-on has no guard in reverse */
    unsigned e2 = get_width(dcode, 5) + get_width(dcode, 6);
    if(decode_e(e2, dcode->ean.s4, 7)) {
        dprintf(2, " [invalid any]");
        return(/*FIXME (get_color(dcode) == ZEBRA_SPACE) ? STATE_ADDON : */-1);
    }

    unsigned e1 = get_width(dcode, 4) + get_width(dcode, 5);
    unsigned char E1 = decode_e(e1, dcode->ean.s4, 7);

    if(get_color(dcode) == ZEBRA_BAR) {
        /* check for quiet-zone */
        if((get_width(dcode, 7) * 14 + 1) / dcode->ean.s4 >= 3) {
            if(!E1) {
                dprintf(2, " [valid normal]");
                return(0); /* normal symbol start */
            }
            else if(E1 == 1) {
                dprintf(2, " [valid add-on]");
                return(STATE_ADDON); /* add-on symbol start */
            }
        }
        dprintf(2, " [invalid start]");
        return(-1);
    }

    if(!E1) {
        /* attempting decode from SPACE => validate center guard */
        unsigned e3 = get_width(dcode, 6) + get_width(dcode, 7);
        if(!decode_e(e3, dcode->ean.s4, 7)) {
            dprintf(2, " [valid center]");
            return(0); /* start after center guard */
        }
    }
    dprintf(2, " [invalid center]");
    return(/*STATE_ADDON*/-1);
}

/* attempt to decode previous 4 widths (2 bars and 2 spaces) as a character */
static inline signed char decode4 (zebra_decoder_t *dcode)
{
    /* calculate similar edge measurements */
    unsigned e1 = ((get_color(dcode) == ZEBRA_BAR)
                   ? get_width(dcode, 0) + get_width(dcode, 1)
                   : get_width(dcode, 2) + get_width(dcode, 3));
    unsigned e2 = get_width(dcode, 1) + get_width(dcode, 2);
    dprintf(2, "\n        e1=%d e2=%d", e1, e2);

    /* create compacted encoding for direct lookup */
    signed char code = ((decode_e(e1, dcode->ean.s4, 7) << 2) |
                        decode_e(e2, dcode->ean.s4, 7));
    if(code < 0)
        return(-1);
    dprintf(2, " code=%x", code);

    /* 4 combinations require additional determinant (D2)
       E1E2 == 34 (0110)
       E1E2 == 43 (1001)
       E1E2 == 33 (0101)
       E1E2 == 44 (1010)
     */
    if((1 << code) & 0x0660) {
        /* use sum of bar widths */
        unsigned d2 = ((get_color(dcode) == ZEBRA_BAR)
                       ? get_width(dcode, 0) + get_width(dcode, 2)
                       : get_width(dcode, 1) + get_width(dcode, 3));
        d2 *= 7;
        unsigned char mid = (((1 << code) & 0x0420)
                             ? 3     /* E1E2 in 33,44 */
                             : 4);   /* E1E2 in 34,43 */
        unsigned char alt = d2 > (mid * dcode->ean.s4);
        if(alt)
            code = ((code >> 1) & 3) | 0x10; /* compress code space */
        dprintf(2, " (d2=%d(%d) alt=%d)", d2, mid * dcode->ean.s4, alt);
    }
    dprintf(2, " char=%02x", digits[(unsigned char)code]);
    assert(code < 0x14);
    return(code);
}

static inline zebra_symbol_type_t ean_part_end4 (ean_pass_t *pass,
                                                 unsigned char rev)
{
    /* extract parity bits */
    unsigned char par = ((pass->raw[1] & 0x10) >> 1 |
                         (pass->raw[2] & 0x10) >> 2 |
                         (pass->raw[3] & 0x10) >> 3 |
                         (pass->raw[4] & 0x10) >> 4);

    dprintf(2, " par=%x", par);
    if(par && par != 0xf)
        /* invalid parity combination */
        return(ZEBRA_NONE);

    if(!par == !rev) {
        /* reverse sampled digits */
        unsigned char tmp = pass->raw[1];
        pass->raw[1] = pass->raw[4];
        pass->raw[4] = tmp;
        tmp = pass->raw[2];
        pass->raw[2] = pass->raw[3];
        pass->raw[3] = tmp;
    }

    dprintf(2, "\n");
    dprintf(1, "decode4=%x%x%x%x\n",
            pass->raw[1] & 0xf, pass->raw[2] & 0xf,
            pass->raw[3] & 0xf, pass->raw[4] & 0xf);
    if(!par)
        return(ZEBRA_EAN8 | EAN_RIGHT);
    return(ZEBRA_EAN8 | EAN_LEFT);
}

static inline zebra_symbol_type_t ean_part_end7 (ean_pass_t *pass,
                                                 unsigned char rev)
{
    /* calculate parity index */
    unsigned char par = ((!rev)
                         ? ((pass->raw[1] & 0x10) << 1 |
                            (pass->raw[2] & 0x10) |
                            (pass->raw[3] & 0x10) >> 1 |
                            (pass->raw[4] & 0x10) >> 2 |
                            (pass->raw[5] & 0x10) >> 3 |
                            (pass->raw[6] & 0x10) >> 4)
                         : ((pass->raw[1] & 0x10) >> 4 |
                            (pass->raw[2] & 0x10) >> 3 |
                            (pass->raw[3] & 0x10) >> 2 |
                            (pass->raw[4] & 0x10) >> 1 |
                            (pass->raw[5] & 0x10) |
                            (pass->raw[6] & 0x10) << 1));

    /* lookup parity combination */
    pass->raw[0] = parity_decode[par >> 1];
    if(par & 1)
        pass->raw[0] >>= 4;
    pass->raw[0] &= 0xf;
    dprintf(2, " par=%02x(%x)", par, pass->raw[0]);

    if(pass->raw[0] == 0xf)
        /* invalid parity combination */
        return(ZEBRA_NONE);

    if(!par == !rev) {
        /* reverse sampled digits */
        unsigned char i;
        for(i = 1; i < 4; i++) {
            unsigned char tmp = pass->raw[i];
            pass->raw[i] = pass->raw[7 - i];
            pass->raw[7 - i] = tmp;
        }
    }

    dprintf(2, "\n");
    dprintf(1, "decode=%x%x%x%x%x%x%x(%02x)\n",
            pass->raw[0] & 0xf, pass->raw[1] & 0xf,
            pass->raw[2] & 0xf, pass->raw[3] & 0xf,
            pass->raw[4] & 0xf, pass->raw[5] & 0xf,
            pass->raw[6] & 0xf, par);

    if(!par)
        return(ZEBRA_EAN13 | EAN_RIGHT);
    if(par & 0x20)
        return(ZEBRA_EAN13 | EAN_LEFT);
    return(/*ZEBRA_UPCE*/ZEBRA_NONE);
}

/* update state for one of 4 parallel passes */
static inline zebra_symbol_type_t decode_pass (zebra_decoder_t *dcode,
                                               ean_pass_t *pass)
{
    pass->state++;
    unsigned char idx = pass->state & STATE_IDX;
    unsigned char rev = pass->state & 1;

    if(get_color(dcode) == ZEBRA_BAR &&
       (idx == 0x10 || idx == 0x0f) &&
       !aux_end(dcode, (rev) ? 3 : 4)) {
        dprintf(2, " rev=%x", rev);
        zebra_symbol_type_t part = ean_part_end4(pass, rev);
        pass->state = -1;
        return(part);
    }

    if(!(idx & 0x03) && idx <= 0x14) {
        if(!dcode->ean.s4)
            return(0);
        /* validate guard bars before decoding first char of symbol */
        if(!pass->state) {
            pass->state = aux_start(dcode);
            if(pass->state < 0)
                return(0);
            idx = pass->state & STATE_IDX;
        }
        signed char code = decode4(dcode);
        if(code < 0)
            pass->state = -1;
        else {
            dprintf(2, "\n        raw[%x]=%02x =>", idx >> 2,
                    digits[(unsigned char)code]);
            pass->raw[(idx >> 2) + 1] = digits[(unsigned char)code];
            dprintf(2, " raw=%d%d%d%d%d%d%d",
                    pass->raw[0] & 0xf, pass->raw[1] & 0xf,
                    pass->raw[2] & 0xf, pass->raw[3] & 0xf,
                    pass->raw[4] & 0xf, pass->raw[5] & 0xf,
                    pass->raw[6] & 0xf);
        }
    }

    if(get_color(dcode) == ZEBRA_BAR &&
       (idx == 0x18 || idx == 0x17)) {
        zebra_symbol_type_t part = ZEBRA_NONE;
        dprintf(2, " rev=%x", rev);
        if(!aux_end(dcode, (rev) ? 3 : 4))
            part = ean_part_end7(pass, rev);
        else
            dprintf(2, " [invalid end guard]");
        pass->state = -1;
        return(part);
    }
    return(0);
}

static inline signed char ean_check_parity (ean_decoder_t *ean,
                                            int n)
{
    unsigned char chk = 0;
    unsigned char i;
    for(i = 0; i < n; i++) {
        unsigned char d = ean->buf[i];
        assert(d < 10);
        chk += d;
        if((i ^ n) & 1) {
            chk += d << 1;
            if(chk >= 20)
                chk -= 20;
        }
        if(chk >= 10)
            chk -= 10;
    }
    assert(chk < 10);
    if(chk)
        chk = 10 - chk;
    unsigned char d = ean->buf[n];
    assert(d < 10);
    if(chk != d) {
        dprintf(1, "\nchecksum mismatch %d != %d (%s)\n",
                chk, d, dsprintbuf(ean));
        return(-1);
    }
    return(0);
}

static inline zebra_symbol_type_t integrate_partial (ean_decoder_t *ean,
                                                     ean_pass_t *pass,
                                                     zebra_symbol_type_t part)
{
    /* FIXME UPC-E, EAN-8 */
    /* copy raw data into holding buffer */
    /* if same partial is not consistent, reset others */
    dprintf(2, " integrate part=%x (%s)", part, dsprintbuf(ean));
    signed char i, j;
    if(part & ZEBRA_ADDON) {
        /* FIXME TBD */
        for(i = (part == ZEBRA_ADDON5) ? 4 : 1; i >= 0; i--) {
            unsigned char digit = pass->raw[i] & 0xf;
            if(ean->addon && ean->buf[i + 13] != digit) {
                /* partial mismatch - reset collected parts */
                ean->left = ean->right = ean->addon = ZEBRA_NONE;
            }
            ean->buf[i + 13] = digit;
        }
        ean->addon = part;
    }
    else {
        if((ean->left && ((part & ZEBRA_SYMBOL) != ean->left)) ||
           (ean->right && ((part & ZEBRA_SYMBOL) != ean->right))) {
            /* partial mismatch - reset collected parts */
            dprintf(2, " rst(type %x %x)", ean->left, ean->right);
            ean->left = ean->right = ean->addon = ZEBRA_NONE;
        }

        if(part & EAN_RIGHT) {
            part &= ZEBRA_SYMBOL;
            j = (part == ZEBRA_EAN13) ? 12 : 7;
            for(i = (part == ZEBRA_EAN13) ? 6 : 4; i; i--, j--) {
                unsigned char digit = pass->raw[i] & 0xf;
                if(ean->right && ean->buf[j] != digit) {
                    /* partial mismatch - reset collected parts */
                    dprintf(2, " rst(right)");
                    ean->left = ean->right = ean->addon = ZEBRA_NONE;
                }
                ean->buf[j] = digit;
            }
            ean->right = part;
        }
        else /* EAN_LEFT or entire UPC-E symbol */ {
            j = (part == ZEBRA_EAN13) ? 6 : 3;
            for(i = (part == ZEBRA_EAN13) ? 6 : 4; j >= 0; i--, j--) {
                unsigned char digit = pass->raw[i] & 0xf;
                if(ean->left && ean->buf[j] != digit) {
                    /* partial mismatch - reset collected parts */
                    dprintf(2, " rst(left)");
                    ean->left = ean->right = ean->addon = ZEBRA_NONE;
                }
                ean->buf[j] = digit;
            }
            ean->left = part;
        }
    }
    /* FIXME broken for UPC-E, addons */
    part = (ean->left & ean->right);
    if(!part)
        part = ZEBRA_PARTIAL;

    if((part == ZEBRA_EAN13 && ean_check_parity(ean, 12)) ||
       (part == ZEBRA_EAN8 && ean_check_parity(ean, 7)))
        /* invalid parity */
        part = ZEBRA_NONE;

    if(part > ZEBRA_PARTIAL)
        part |= ean->addon;
    dprintf(2, " %x/%x=%x", ean->left, ean->right, part);
    return(part);
}

/* copy result to output buffer */
static inline void postprocess (zebra_decoder_t *dcode,
                                zebra_symbol_type_t sym)
{
    ean_decoder_t *ean = &dcode->ean;
    int i = 0;
    if((sym & ZEBRA_SYMBOL) > ZEBRA_PARTIAL)
        for(; i < ((sym & ZEBRA_SYMBOL) - 1) && ean->buf[i] >= 0; i++)
            dcode->buf[i] = ean->buf[i] + '0';
    int j = i;
    if(sym & ZEBRA_ADDON)
        for(i = 13; ean->buf[i] >= 0; i++)
            dcode->buf[j++] = ean->buf[i] + '0';
    dcode->buf[j] = '\0';
}

zebra_symbol_type_t zebra_decode_ean (zebra_decoder_t *dcode)
{
    /* process upto 4 separate passes */
    zebra_symbol_type_t sym = ZEBRA_NONE;
    unsigned char pass_idx = dcode->idx & 3;

    /* update latest character width */
    dcode->ean.s4 -= get_width(dcode, 4);
    dcode->ean.s4 += get_width(dcode, 0);

    unsigned char i;
    for(i = 0; i < 4; i++) {
        ean_pass_t *pass = &dcode->ean.pass[i];
        if(pass->state >= 0 ||
           i == pass_idx)
        {
            dprintf(2, "      ean[%x/%x]: idx=%x st=%d s=%d",
                    pass_idx, i, dcode->idx, pass->state, dcode->ean.s4);
            zebra_symbol_type_t part = decode_pass(dcode, pass);
            if(part) {
                /* update accumulated data from new partial decode */
                sym = integrate_partial(&dcode->ean, pass, part);
                if(sym) {
                    /* this pass valid => _reset_ all passes */
                    dprintf(2, " sym=%x", sym);
                    dcode->ean.pass[0].state = dcode->ean.pass[1].state = -1;
                    dcode->ean.pass[2].state = dcode->ean.pass[3].state = -1;
                    if(sym > ZEBRA_PARTIAL) {
                        if(!get_lock(dcode))
                            postprocess(dcode, sym);
                        else {
                            dprintf(1, " [locked]");
                            sym = ZEBRA_PARTIAL;
                        }
                    }
                }
            }
            dprintf(2, "\n");
        }
    }
    return(sym);
}