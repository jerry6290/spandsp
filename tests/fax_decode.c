/*
 * SpanDSP - a series of DSP components for telephony
 *
 * fax_decode.c - a simple FAX audio decoder
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2005 Steve Underwood
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*! \page fax_decode_page FAX decoder
\section fax_decode_page_sec_1 What does it do?
???.

\section fax_decode_tests_page_sec_2 How does it work?
???.
*/

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sndfile.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES

#include "spandsp.h"

#define SAMPLES_PER_CHUNK   160

#define DISBIT1     0x01
#define DISBIT2     0x02
#define DISBIT3     0x04
#define DISBIT4     0x08
#define DISBIT5     0x10
#define DISBIT6     0x20
#define DISBIT7     0x40
#define DISBIT8     0x80

enum
{
    FAX_NONE,
    FAX_V27TER_RX,
    FAX_V29_RX,
    FAX_V17_RX
};

static const struct
{
    int bit_rate;
    int modem_type;
    int which;
    uint8_t dcs_code;
} fallback_sequence[] =
{
    {14400, T30_MODEM_V17,    T30_SUPPORT_V17,    DISBIT6},
    {12000, T30_MODEM_V17,    T30_SUPPORT_V17,    (DISBIT6 | DISBIT4)},
    { 9600, T30_MODEM_V17,    T30_SUPPORT_V17,    (DISBIT6 | DISBIT3)},
    { 9600, T30_MODEM_V29,    T30_SUPPORT_V29,    DISBIT3},
    { 7200, T30_MODEM_V17,    T30_SUPPORT_V17,    (DISBIT6 | DISBIT4 | DISBIT3)},
    { 7200, T30_MODEM_V29,    T30_SUPPORT_V29,    (DISBIT4 | DISBIT3)},
    { 4800, T30_MODEM_V27TER, T30_SUPPORT_V27TER, DISBIT4},
    { 2400, T30_MODEM_V27TER, T30_SUPPORT_V27TER, 0},
    {    0, 0, 0, 0}
};

bool decode_test = false;
int rx_bits = 0;

t30_state_t t30_dummy;
t4_rx_state_t t4_rx_state;
bool t4_up = false;

hdlc_rx_state_t hdlcrx;

int fast_trained = FAX_NONE;

uint8_t ecm_data[256][260];
int16_t ecm_len[256];

int line_encoding = T4_COMPRESSION_T4_1D;
int x_resolution = T4_X_RESOLUTION_R8;
int y_resolution = T4_Y_RESOLUTION_STANDARD;
int image_width = 1728;
int octets_per_ecm_frame = 256;
bool error_correcting_mode = false;
int current_fallback = 0;
bool end_of_page_detected = false;

static void decode_20digit_msg(const uint8_t *pkt, int len)
{
    int p;
    int k;
    char msg[T30_MAX_IDENT_LEN + 1];

    if (len > T30_MAX_IDENT_LEN + 3)
    {
        fprintf(stderr, "XXX %d %d\n", len, T30_MAX_IDENT_LEN + 1);
        msg[0] = '\0';
        return;
    }
    /*endif*/
    pkt += 2;
    p = len - 2;
    /* Strip trailing spaces */
    while (p > 1  &&  pkt[p - 1] == ' ')
        p--;
    /*endwhile*/
    /* The string is actually backwards in the message */
    k = 0;
    while (p > 1)
        msg[k++] = pkt[--p];
    /*endwhile*/
    msg[k] = '\0';
    fprintf(stderr, "%s is: \"%s\"\n", t30_frametype(pkt[0]), msg);
}
/*- End of function --------------------------------------------------------*/

static void print_frame(const char *io, const uint8_t *fr, int frlen)
{
    int i;
    int type;
    const char *country;
    const char *vendor;
    const char *model;

    if (frlen == 0)
    {
        return;
    }
    /*endif*/
    fprintf(stderr, "%s %s:", io, t30_frametype(fr[2]));
    for (i = 2;  i < frlen;  i++)
    {
        fprintf(stderr, " %02x", fr[i]);
    }
    /*endfor*/
    fprintf(stderr, "\n");
    type = fr[2] & 0xFE;
    if (type == T30_DIS  ||  type == T30_DTC  ||  type == T30_DCS)
    {
        t30_decode_dis_dtc_dcs(&t30_dummy, fr, frlen);
    }
    /*endif*/
    if (type == T30_CSI  ||  type == T30_TSI  ||  type == T30_PWD  ||  type == T30_SEP  ||  type == T30_SUB  ||  type == T30_SID)
    {
        decode_20digit_msg(fr, frlen);
    }
    /*endif*/
    if (type == T30_NSF  ||  type == T30_NSS  ||  type == T30_NSC)
    {
        if (t35_decode(&fr[3], frlen - 3, &country, &vendor, &model))
        {
            if (country)
                fprintf(stderr, "The remote was made in '%s'\n", country);
            /*endif*/
            if (vendor)
                fprintf(stderr, "The remote was made by '%s'\n", vendor);
            /*endif*/
            if (model)
                fprintf(stderr, "The remote is a '%s'\n", model);
            /*endif*/
        }
        /*endif*/
        if (type == T30_NSS  ||  type == T30_NSC)
        {
            fprintf(stderr, "WARNING: The FAX machines may be switching into a proprietary mode, which this software cannot decode\n");
        }
        /*endif*/
    }
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

static int find_fallback_entry(int dcs_code)
{
    int i;

    /* The table is short, and not searched often, so a brain-dead linear scan seems OK */
    for (i = 0;  fallback_sequence[i].bit_rate;  i++)
    {
        if (fallback_sequence[i].dcs_code == dcs_code)
            break;
        /*endif*/
    }
    /*endfor*/
    if (fallback_sequence[i].bit_rate == 0)
        return -1;
    /*endif*/
    return i;
}
/*- End of function --------------------------------------------------------*/

static int check_rx_dcs(const uint8_t *msg, int len)
{
    static const int widths[3][4] =
    {
        { 864, 1024, 1216, -1}, /* R4 resolution - no longer used in recent versions of T.30 */
        {1728, 2048, 2432, -1}, /* R8 resolution */
        {3456, 4096, 4864, -1}  /* R16 resolution */
    };
    uint8_t dcs_frame[T30_MAX_DIS_DTC_DCS_LEN];

    /* Check DCS frame from remote */
    if (len < 6)
    {
        printf("Short DCS frame\n");
        return -1;
    }
    /*endif*/

    /* Make a local copy of the message, padded to the maximum possible length with zeros. This allows
       us to simply pick out the bits, without worrying about whether they were set from the remote side. */
    if (len > T30_MAX_DIS_DTC_DCS_LEN)
    {
        memcpy(dcs_frame, msg, T30_MAX_DIS_DTC_DCS_LEN);
    }
    else
    {
        memcpy(dcs_frame, msg, len);
        if (len < T30_MAX_DIS_DTC_DCS_LEN)
            memset(dcs_frame + len, 0, T30_MAX_DIS_DTC_DCS_LEN - len);
        /*endif*/
    }
    /*endif*/

    octets_per_ecm_frame = (dcs_frame[6] & DISBIT4)  ?  256  :  64;

    if ((dcs_frame[8] & DISBIT1))
        y_resolution = T4_Y_RESOLUTION_SUPERFINE;
    else if (dcs_frame[4] & DISBIT7)
        y_resolution = T4_Y_RESOLUTION_FINE;
    else
        y_resolution = T4_Y_RESOLUTION_STANDARD;
    /*endif*/

    if ((dcs_frame[8] & DISBIT3))
    {
        x_resolution = T4_X_RESOLUTION_R16;
        y_resolution = T4_Y_RESOLUTION_SUPERFINE;
    }
    else
    {
        x_resolution = T4_X_RESOLUTION_R8;
    }
    /*endif*/

    image_width = widths[(dcs_frame[8] & DISBIT3)  ?  2  :  1][dcs_frame[5] & (DISBIT2 | DISBIT1)];

    /* Check which compression we will use. */
    if ((dcs_frame[12] & DISBIT7))
        line_encoding = T4_COMPRESSION_T85_L0;
    else if ((dcs_frame[12] & DISBIT6))
        line_encoding = T4_COMPRESSION_T85;
    else if ((dcs_frame[6] & DISBIT7))
        line_encoding = T4_COMPRESSION_T6;
    else if ((dcs_frame[4] & DISBIT8))
        line_encoding = T4_COMPRESSION_T4_2D;
    else
        line_encoding = T4_COMPRESSION_T4_1D;
    /*endif*/
    fprintf(stderr, "Selected compression %d\n", line_encoding);

    if ((current_fallback = find_fallback_entry(dcs_frame[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3))) < 0)
        printf("Remote asked for a modem standard we do not support\n");
    /*endif*/
    error_correcting_mode = ((dcs_frame[6] & DISBIT3) != 0);
    return 0;
}
/*- End of function --------------------------------------------------------*/

static void hdlc_accept(void *user_data, const uint8_t *msg, int len, int ok)
{
    int type;
    int frame_no;
    int i;

    if (len < 0)
    {
        /* Special conditions */
        fprintf(stderr, "HDLC status is %s (%d)\n", signal_status_to_str(len), len);
        return;
    }
    /*endif*/

    if (ok)
    {
        if (msg[0] != 0xFF  ||  !(msg[1] == 0x03  ||  msg[1] == 0x13))
        {
            fprintf(stderr, "Bad frame header - %02x %02x\n", msg[0], msg[1]);
            return;
        }
        /*endif*/
        print_frame("HDLC: ", msg, len);
        type = msg[2] & 0xFE;
        switch (type)
        {
        case T4_FCD:
            if (len <= 4 + 256)
            {
                frame_no = msg[3];
                /* Just store the actual image data, and record its length */
                memcpy(&ecm_data[frame_no][0], &msg[4], len - 4);
                ecm_len[frame_no] = (int16_t) (len - 4);
            }
            /*endif*/
            break;
        case T30_DCS:
            check_rx_dcs(msg, len);
            break;
        }
        /*endswitch*/
    }
    else
    {
        fprintf(stderr, "Bad HDLC frame ");
        for (i = 0;  i < len;  i++)
            fprintf(stderr, " %02x", msg[i]);
        /*endfor*/
        fprintf(stderr, "\n");
    }
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

static void t4_begin(void)
{
    int i;

    //printf("Begin T.4 - %d %d %d %d\n", line_encoding, x_resolution, y_resolution, image_width);
    t4_rx_set_rx_encoding(&t4_rx_state, line_encoding);
    t4_rx_set_x_resolution(&t4_rx_state, x_resolution);
    t4_rx_set_y_resolution(&t4_rx_state, y_resolution);
    t4_rx_set_image_width(&t4_rx_state, image_width);

    t4_rx_start_page(&t4_rx_state);
    t4_up = true;
    end_of_page_detected = false;

    for (i = 0;  i < 256;  i++)
        ecm_len[i] = -1;
    /*endfor*/
}
/*- End of function --------------------------------------------------------*/

static void t4_end(void)
{
    t4_stats_t stats;
    int i;

    if (!t4_up)
        return;
    /*endif*/
    if (error_correcting_mode)
    {
        for (i = 0;  i < 256;  i++)
        {
            if (ecm_len[i] > 0)
                t4_rx_put(&t4_rx_state, ecm_data[i], ecm_len[i]);
            /*endif*/
            fprintf(stderr, "%d", (ecm_len[i] <= 0)  ?  0  :  1);
        }
        /*endfor*/
        fprintf(stderr, "\n");
    }
    /*endif*/
    t4_rx_end_page(&t4_rx_state);
    t4_rx_get_transfer_statistics(&t4_rx_state, &stats);
    fprintf(stderr, "Pages = %d\n", stats.pages_transferred);
    fprintf(stderr, "Image size = %dx%d\n", stats.width, stats.length);
    fprintf(stderr, "Image resolution = %dx%d\n", stats.x_resolution, stats.y_resolution);
    fprintf(stderr, "Bad rows = %d\n", stats.bad_rows);
    fprintf(stderr, "Longest bad row run = %d\n", stats.longest_bad_row_run);
    t4_up = false;
}
/*- End of function --------------------------------------------------------*/

static void v21_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
        fprintf(stderr, "V.21 rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_CARRIER_DOWN:
            //t4_end();
            break;
        }
        /*endswitch*/
        return;
    }
    /*endif*/
    //fprintf(stderr, "V.21 Rx bit %d - %d\n", rx_bits++, bit);
    if (fast_trained == FAX_NONE)
        hdlc_rx_put_bit(&hdlcrx, bit);
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

static void v17_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
        fprintf(stderr, "V.17 rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_TRAINING_SUCCEEDED:
            fast_trained = FAX_V17_RX;
            t4_begin();
            break;
        case SIG_STATUS_CARRIER_DOWN:
            t4_end();
            if (fast_trained == FAX_V17_RX)
                fast_trained = FAX_NONE;
            /*endif*/
            break;
        }
        /*endswitch*/
        return;
    }
    /*endif*/
    if (error_correcting_mode)
    {
        hdlc_rx_put_bit(&hdlcrx, bit);
    }
    else
    {
        if (t4_rx_put_bit(&t4_rx_state, bit))
        {
            t4_end();
            if (!end_of_page_detected)
                fprintf(stderr, "End of page detected\n");
            /*endif*/
            end_of_page_detected = true;
        }
        /*endif*/
    }
    /*endif*/
    //printf("V.17 Rx bit %d - %d\n", rx_bits++, bit);
}
/*- End of function --------------------------------------------------------*/

static void v29_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
        fprintf(stderr, "V.29 rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_TRAINING_SUCCEEDED:
            fast_trained = FAX_V29_RX;
            t4_begin();
            break;
        case SIG_STATUS_CARRIER_DOWN:
            t4_end();
            if (fast_trained == FAX_V29_RX)
                fast_trained = FAX_NONE;
            /*endif*/
            break;
        }
        /*endswitch*/
        return;
    }
    /*endif*/
    if (error_correcting_mode)
    {
        hdlc_rx_put_bit(&hdlcrx, bit);
    }
    else
    {
        if (t4_rx_put_bit(&t4_rx_state, bit))
        {
            t4_end();
            if (!end_of_page_detected)
                fprintf(stderr, "End of page detected\n");
            /*endif*/
            end_of_page_detected = true;
        }
        /*endif*/
    }
    /*endif*/
    //printf("V.29 Rx bit %d - %d\n", rx_bits++, bit);
}
/*- End of function --------------------------------------------------------*/

static void v27ter_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
        fprintf(stderr, "V.27ter rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_TRAINING_SUCCEEDED:
            fast_trained = FAX_V27TER_RX;
            t4_begin();
            break;
        case SIG_STATUS_CARRIER_DOWN:
            t4_end();
            if (fast_trained == FAX_V27TER_RX)
                fast_trained = FAX_NONE;
            /*endif*/
            break;
        }
        /*endswitch*/
        return;
    }
    /*endif*/
    if (error_correcting_mode)
    {
        hdlc_rx_put_bit(&hdlcrx, bit);
    }
    else
    {
        if (t4_rx_put_bit(&t4_rx_state, bit))
        {
            t4_end();
            if (!end_of_page_detected)
                fprintf(stderr, "End of page detected\n");
            /*endif*/
            end_of_page_detected = true;
        }
        /*endif*/
    }
    /*endif*/
    //printf("V.27ter Rx bit %d - %d\n", rx_bits++, bit);
}
/*- End of function --------------------------------------------------------*/

static void decode_t30_message(bool t30_decode_reversed)
{
    int i;
    unsigned int hex;
    char buf[1024];
    uint8_t bytes[1024];

    /* Decode T.30 messages entered as a string of hex byte values in the form xx xx xx */
    while (fgets(buf, 1024, stdin))
    {
        for (i = 0;  i < 256;  i++)
        {
            if (sscanf(&buf[3*i], "%x", &hex) != 1)
                break;
            /*endif*/
            if (t30_decode_reversed)
                hex = bit_reverse8(hex);
            /*endif*/
            bytes[i] = hex;
        }
        /*endfor*/
        print_frame("", bytes, i);
    }
    /*endwhile*/
}
/*- End of function --------------------------------------------------------*/

int main(int argc, char *argv[])
{
    fsk_rx_state_t *fsk;
    v17_rx_state_t *v17;
    v29_rx_state_t *v29;
    v27ter_rx_state_t *v27ter_4800;
    v27ter_rx_state_t *v27ter_2400;
    int16_t amp[SAMPLES_PER_CHUNK];
    SNDFILE *inhandle;
    SF_INFO info;
    int len;
    const char *filename;
    logging_state_t *logging;
    int opt;
    bool t30_decode_reversed;
    bool t30_decode;

    filename = "fax_samp.wav";
    t30_decode_reversed = false;
    t30_decode = false;
    while ((opt = getopt(argc, argv, "c:ertw:x:y:")) != -1)
    {
        switch (opt)
        {
        case 'c':
            /* Force, for when there is no DCS to set this properly */
            if (strcmp(optarg, "T41D") == 0)
            {
                line_encoding = T4_COMPRESSION_T4_1D;
            }
            else if (strcmp(optarg, "T42D") == 0)
            {
                line_encoding = T4_COMPRESSION_T4_2D;
            }
            else if (strcmp(optarg, "T6") == 0)
            {
                line_encoding = T4_COMPRESSION_T6;
            }
            else if (strcmp(optarg, "T85") == 0)
            {
                line_encoding = T4_COMPRESSION_T85;
            }
#if defined(SPANDSP_SUPPORT_T88)
            else if (strcmp(optarg, "T88") == 0)
            {
                line_encoding = T4_COMPRESSION_T88;
            }
#endif
            else if (strcmp(optarg, "T81") == 0)
            {
                line_encoding = T4_COMPRESSION_T42_T81;
            }
            else if (strcmp(optarg, "T43") == 0)
            {
                line_encoding = T4_COMPRESSION_T43;
            }
#if defined(SPANDSP_SUPPORT_T45)
            else if (strcmp(optarg, "T45") == 0)
            {
                line_encoding = T4_COMPRESSION_T45;
            }
#endif
            else
            {
                printf("Unrecognised line compression.\n");
                exit(2);
            }
            /*endif*/
            break;
        case 'e':
            /* Force, for when there is no DCS to set this properly */
            error_correcting_mode = true;
            break;
        case 'r':
            t30_decode_reversed = true;
            break;
        case 't':
            t30_decode = true;
            break;
        case 'w':
            /* Force, for when there is no DCS to set this properly */
            image_width = 1728;
            break;
        case 'x':
            /* Force, for when there is no DCS to set this properly */
            if (strcmp(optarg, "R4") == 0)
            {
                x_resolution = T4_X_RESOLUTION_R4;
            }
            else if (strcmp(optarg, "R8") == 0)
            {
                x_resolution = T4_X_RESOLUTION_R8;
            }
            else if (strcmp(optarg, "R16") == 0)
            {
                x_resolution = T4_X_RESOLUTION_R16;
            }
            else
            {
                printf("Unrecognised X-resolution.\n");
                exit(2);
            }
            /*endif*/
            break;
        case 'y':
            /* Force, for when there is no DCS to set this properly */
            if (strcmp(optarg, "standard") == 0)
            {
                y_resolution = T4_Y_RESOLUTION_STANDARD;
            }
            else if (strcmp(optarg, "fine") == 0)
            {
                y_resolution = T4_Y_RESOLUTION_FINE;
            }
            else if (strcmp(optarg, "superfine") == 0)
            {
                y_resolution = T4_Y_RESOLUTION_SUPERFINE;
            }
            else
            {
                printf("Unrecognised Y-resolution.\n");
                exit(2);
            }
            /*endif*/
            break;
        }
        /*endswitch*/
    }
    /*endwhile*/
    argc -= optind;
    argv += optind;

    if (argc > 0)
        filename = argv[0];
    /*endif*/

    memset(&t30_dummy, 0, sizeof(t30_dummy));
    logging = t30_get_logging_state(&t30_dummy);
    span_log_init(logging, SPAN_LOG_NONE, NULL);
    span_log_set_protocol(logging, "T.30");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_SHOW_SAMPLE_TIME | SPAN_LOG_FLOW);

    if (t30_decode)
    {
        decode_t30_message(t30_decode_reversed);
        exit(0);
    }
    /*endif*/

    memset(&info, 0, sizeof(info));
    if ((inhandle = sf_open(filename, SFM_READ, &info)) == NULL)
    {
        fprintf(stderr, "    Cannot open audio file '%s' for reading\n", filename);
        exit(2);
    }
    /*endif*/
    if (info.samplerate != SAMPLE_RATE)
    {
        fprintf(stderr, "    Unexpected sample rate in audio file '%s'\n", filename);
        exit(2);
    }
    /*endif*/
    if (info.channels != 1)
    {
        fprintf(stderr, "    Unexpected number of channels in audio file '%s'\n", filename);
        exit(2);
    }
    /*endif*/

    hdlc_rx_init(&hdlcrx, false, true, 5, hdlc_accept, NULL);
    fsk = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH2], FSK_FRAME_MODE_SYNC, v21_put_bit, NULL);
    v17 = v17_rx_init(NULL, 14400, v17_put_bit, NULL);
    v29 = v29_rx_init(NULL, 9600, v29_put_bit, NULL);
    //v29 = v29_rx_init(NULL, 7200, v29_put_bit, NULL);
    v27ter_4800 = v27ter_rx_init(NULL, 4800, v27ter_put_bit, NULL);
    v27ter_2400 = v27ter_rx_init(NULL, 2400, v27ter_put_bit, NULL);

    fsk_rx_set_signal_cutoff(fsk, -45.5);
    v17_rx_set_signal_cutoff(v17, -45.5);
    v29_rx_set_signal_cutoff(v29, -45.5);
    v27ter_rx_set_signal_cutoff(v27ter_4800, -40.0);
    v27ter_rx_set_signal_cutoff(v27ter_2400, -40.0);

#if 1
    logging = v17_rx_get_logging_state(v17);
    span_log_set_protocol(logging, "V.17");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_SHOW_SAMPLE_TIME | SPAN_LOG_FLOW);

    logging = v29_rx_get_logging_state(v29);
    span_log_set_protocol(logging, "V.29");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_SHOW_SAMPLE_TIME | SPAN_LOG_FLOW);

    logging = v27ter_rx_get_logging_state(v27ter_4800);
    span_log_set_protocol(logging, "V.27ter-4800");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_SHOW_SAMPLE_TIME | SPAN_LOG_FLOW);

    logging = v27ter_rx_get_logging_state(v27ter_2400);
    span_log_set_protocol(logging, "V.27ter-2400");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_SHOW_SAMPLE_TIME | SPAN_LOG_FLOW);
#endif

    if (t4_rx_init(&t4_rx_state, "fax_decode.tif", T4_COMPRESSION_T4_2D) == NULL)
    {
        fprintf(stderr, "Failed to init\n");
        exit(0);
    }
    /*endif*/

    for (;;)
    {
        len = sf_readf_short(inhandle, amp, SAMPLES_PER_CHUNK);
        if (len < SAMPLES_PER_CHUNK)
            break;
        /*endif*/
        fsk_rx(fsk, amp, len);
        v17_rx(v17, amp, len);
        v29_rx(v29, amp, len);
        v27ter_rx(v27ter_4800, amp, len);
        v27ter_rx(v27ter_2400, amp, len);

        logging = t30_get_logging_state(&t30_dummy);
        span_log_bump_samples(logging, len);
        logging = v17_rx_get_logging_state(v17);
        span_log_bump_samples(logging, len);
        logging = v29_rx_get_logging_state(v29);
        span_log_bump_samples(logging, len);
        logging = v27ter_rx_get_logging_state(v27ter_4800);
        span_log_bump_samples(logging, len);
        logging = v27ter_rx_get_logging_state(v27ter_2400);
        span_log_bump_samples(logging, len);
    }
    /*endfor*/
    t4_rx_release(&t4_rx_state);

    if (sf_close(inhandle))
    {
        fprintf(stderr, "    Cannot close audio file '%s'\n", filename);
        exit(2);
    }
    /*endif*/
    return 0;
}
/*- End of function --------------------------------------------------------*/
/*- End of file ------------------------------------------------------------*/
