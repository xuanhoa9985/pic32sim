/*
 * SD/MMC card emulation.
 *
 * Copyright (C) 2011-2014 Serge Vakulenko <serge@vak.ru>
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */
#include <stdio.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "globals.h"

//#define TRACE       printf
#ifndef TRACE
#define TRACE(...)  /*empty*/
#endif

/*
 * Definitions for MMC/SDC commands.
 */
#define CMD_GO_IDLE         (0x40+0)    /* CMD0 */
#define	CMD_SEND_OP_SDC     (0x40+41)   /* ACMD41 (SDC) */
#define CMD_SET_BLEN        (0x40+16)
#define CMD_SEND_IF_COND    (0x40+8)
#define CMD_SEND_CSD        (0x40+9)
#define CMD_STOP            (0x40+12)
#define CMD_READ_SINGLE     (0x40+17)
#define CMD_READ_MULTIPLE   (0x40+18)
#define CMD_SET_WBECNT      (0x40+23)   /* ACMD23 */
#define CMD_WRITE_SINGLE    (0x40+24)
#define CMD_WRITE_MULTIPLE  (0x40+25)
#define CMD_APP             (0x40+55)   /* CMD55 */

#define DATA_START_BLOCK        0xFE    /* start data for single block */
#define STOP_TRAN_TOKEN         0xFD    /* stop token for write multiple */
#define WRITE_MULTIPLE_TOKEN    0xFC    /* start data for write multiple */

/* SD card private data */
struct sdcard {
    const char *name;                   /* Device name */
    unsigned kbytes;                    /* Disk size */
    int unit;                           /* Index (sd0 or sd1) */
    int fd;                             /* Image file */
    int select;                         /* Selected */
    int read_multiple;                  /* Read-multiple mode */
    unsigned blen;                      /* Block length */
    unsigned wbecnt;                    /* Write block erase count */
    unsigned offset;                    /* Read/write offset */
    unsigned count;                     /* Byte count */
    unsigned limit;                     /* Reply length */
    unsigned char buf [1024 + 16];
};
typedef struct sdcard sdcard_t;

static sdcard_t sdcard[2];

unsigned sdcard_gpio_port0;     // GPIO port number of CS0 signal
unsigned sdcard_gpio_port1;     // GPIO port number of CS1 signal
unsigned sdcard_gpio_cs0;       // GPIO pin mask of CS0 signal
unsigned sdcard_gpio_cs1;       // GPIO pin mask of CS1 signal

static void read_data (int fd, unsigned offset,
    unsigned char *buf, unsigned blen)
{
    /* Fill uninitialized blocks by FF: simulate real flash media. */
    memset (buf, 0xFF, blen);

    if (lseek (fd, offset, 0) != offset) {
        printf ("sdcard: seek failed, offset %#x\n", offset);
        return;
    }
    if (read (fd, buf, blen) < 0) {
        printf ("sdcard: read failed, offset %#x\n", offset);
        return;
    }
}

static void write_data (int fd, unsigned offset,
    unsigned char *buf, unsigned blen)
{
    if (lseek (fd, offset, 0) != offset) {
        printf ("sdcard: seek failed, offset %#x\n", offset);
        return;
    }
    if (write (fd, buf, blen) != blen) {
        printf ("sdcard: write failed, offset %#x\n", offset);
        return;
    }
}

static void card_reset (sdcard_t *d)
{
    d->select = 0;
    d->blen = 512;
    d->count = 0;
}

/*
 * Reset sdcard.
 */
void sdcard_reset()
{
    card_reset (&sdcard[0]);
    card_reset (&sdcard[1]);
}

/*
 * Initialize SD card.
 */
void sdcard_init (int unit, const char *name, const char *filename, int cs_port, int cs_pin)
{
    sdcard_t *d = &sdcard[unit];
    struct stat st;

    memset (d, 0, sizeof (*d));
    d->name = name;
    if (! filename) {
        /* No SD card installed. */
        return;
    }
    if (unit == 0) {
        sdcard_gpio_port0 = cs_port;
        sdcard_gpio_cs0 = (cs_pin >= 0) ? (1 << cs_pin) : 0;
    } else {
        sdcard_gpio_port1 = cs_port;
        sdcard_gpio_cs1 = (cs_pin >= 0) ? (1 << cs_pin) : 0;
    }

    d->fd = open (filename, O_RDWR);
    if (d->fd < 0) {
        /* Fatal: no image available. */
        perror (filename);
        exit (1);
    }
    fstat (d->fd, &st);
    d->kbytes = st.st_size / 1024;
    printf("Card%u image '%s', %d kbytes\n", unit, filename, d->kbytes);
}

void sdcard_select (int unit, int on)
{
    sdcard_t *d = &sdcard[unit];

    if (on) {
        TRACE ("sdcard%d: (((\n", unit);
        d->select = 1;
        d->count = 0;
    } else {
        TRACE ("sdcard%d: )))\n", unit);
        d->select = 0;
    }
}

/*
 * Data i/o: send byte to device.
 * Return received byte.
 */
unsigned sdcard_io (unsigned data)
{
    sdcard_t *d = sdcard[0].select ? &sdcard[0] :
                  sdcard[1].select ? &sdcard[1] : 0;
    unsigned reply;

    if (! d || ! d->fd) {
        //TRACE ("sdcard: unselected i/o\n");
        return 0xFF;
    }
    data = (unsigned char) data;
    reply = 0xFF;
    if (d->count == 0) {
        d->buf[0] = data;
        if (data != 0xFF)
            d->count++;
    } else {
        switch (d->buf[0]) {
        case CMD_GO_IDLE:               /* CMD0: reset */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7)
                reply = 0x01;
            break;
        case CMD_APP:                   /* CMD55: application prefix */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                reply = 0;
                d->count = 0;
            }
            break;
        case CMD_SEND_OP_SDC:           /* ACMD41: initialization */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7)
                reply = 0;
            break;
        case CMD_SET_BLEN:              /* Set block length */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                d->blen = d->buf[1] << 24 | d->buf[2] << 16 |
                    d->buf[3] << 8 | d->buf[4];
                TRACE ("sdcard%d: set block length %u bytes\n", d->unit, d->blen);
                reply = (d->blen > 0 && d->blen <= 1024) ? 0 : 4;
            }
            break;
        case CMD_SET_WBECNT:            /* Set write block erase count */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                d->wbecnt = d->buf[1] << 24 | d->buf[2] << 16 |
                    d->buf[3] << 8 | d->buf[4];
                TRACE ("sdcard%d: set write block erase count %u\n", d->unit, d->wbecnt);
                reply = 0;
                d->count = 0;
            }
            break;
        case CMD_SEND_CSD:              /* Get card data */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                /* Send reply */
                TRACE ("sdcard%d: send media size %u sectors\n",
                    d->unit, d->kbytes * 2);
                reply = 0;
                d->limit = 16 + 3;
                d->count = 1;
                d->buf[0] = 0;
                d->buf[1] = DATA_START_BLOCK;
                d->buf[2+0] = 1 << 6;     /* SDC ver 2.00 */
                d->buf[2+1] = 0;
                d->buf[2+2] = 0;
                d->buf[2+3] = 0;
                d->buf[2+4] = 0;
                d->buf[2+5] = 0;
                d->buf[2+6] = 0;
                d->buf[2+7] = 0;
                d->buf[2+8] = (d->kbytes / 512 - 1) >> 8;
                d->buf[2+9] = d->kbytes / 512 - 1;
                d->buf[2+10] = 0;
                d->buf[2+11] = 0;
                d->buf[2+12] = 0;
                d->buf[2+13] = 0;
                d->buf[2+14] = 0;
                d->buf[2+15] = 0;
                d->buf[d->limit - 1] = 0xFF;
                d->buf[d->limit] = 0xFF;
            }
            break;
        case CMD_READ_SINGLE:           /* Read block */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                /* Send reply */
                reply = 0;
                d->offset = d->buf[1] << 24 | d->buf[2] << 16 |
                    d->buf[3] << 8 | d->buf[4];
                TRACE ("sdcard%d: read offset %#x, length %u kbytes\n",
                    d->unit, d->offset, d->blen);
                d->limit = d->blen + 3;
                d->count = 1;
                d->buf[0] = 0;
                d->buf[1] = DATA_START_BLOCK;
                read_data (d->fd, d->offset, &d->buf[2], d->blen);
                d->buf[d->limit - 1] = 0xFF;
                d->buf[d->limit] = 0xFF;
            }
            break;
        case CMD_READ_MULTIPLE:         /* Read multiple blocks */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                /* Send reply */
                reply = 0;
                d->read_multiple = 1;
                d->offset = d->buf[1] << 24 | d->buf[2] << 16 |
                    d->buf[3] << 8 | d->buf[4];
                TRACE ("sdcard%d: read offset %#x, length %u kbytes\n",
                    d->unit, d->offset, d->blen);
                d->limit = d->blen + 3;
                d->count = 1;
                d->buf[0] = 0;
                d->buf[1] = DATA_START_BLOCK;
                read_data (d->fd, d->offset, &d->buf[2], d->blen);
                d->buf[d->limit - 1] = 0xFF;
                d->buf[d->limit] = 0xFF;
            }
            break;
        case CMD_WRITE_SINGLE:          /* Write block */
            if (d->count >= sizeof (d->buf))
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                /* Accept command */
                reply = 0;
                d->offset = d->buf[1] << 24 | d->buf[2] << 16 |
                    d->buf[3] << 8 | d->buf[4];
                TRACE ("sdcard%d: write offset %#x\n", d->unit, d->offset);
            } else if (d->count == 7 + d->blen + 2 + 2) {
                if (d->buf[7] == DATA_START_BLOCK) {
                    /* Accept data */
                    reply = 0x05;
                    d->offset = d->buf[1] << 24 | d->buf[2] << 16 |
                        d->buf[3] << 8 | d->buf[4];
                    write_data (d->fd, d->offset, &d->buf[8], d->blen);
                    TRACE ("sdcard%d: write data, length %u kbytes\n", d->unit, d->blen);
                } else {
                    /* Reject data */
                    reply = 4;
                    TRACE ("sdcard%d: reject write data, tag=%02x\n", d->unit, d->buf[7]);
                }
            }
            break;
        case CMD_WRITE_MULTIPLE:        /* Write multiple blocks */
            if (d->count >= 7)
                break;
            d->buf [d->count++] = data;
            if (d->count == 7) {
                /* Accept command */
                reply = 0;
                d->offset = d->buf[1] << 24 | d->buf[2] << 16 |
                    d->buf[3] << 8 | d->buf[4];
                TRACE ("sdcard%d: write multiple offset %#x\n", d->unit, d->offset);
                d->count = 0;
            }
            break;
        case WRITE_MULTIPLE_TOKEN:      /* Data for write-miltiple */
            if (d->count >= sizeof (d->buf))
                break;
            d->buf [d->count++] = data;
            if (d->count == 2 + d->blen + 2) {
                /* Accept data */
                reply = 0x05;
                write_data (d->fd, d->offset, &d->buf[1], d->blen);
                TRACE ("sdcard%d: write sector %u, length %u kbytes\n",
                    d->unit, d->offset / 512, d->blen);
                d->offset += 512;
                d->count = 0;
            }
            break;
        case CMD_STOP:                  /* Stop read-multiple sequence */
            if (d->count > 1)
                break;
            d->read_multiple = 0;
            reply = 0;
            break;
        case CMD_SEND_IF_COND:          /* Stop read-multiple sequence */
            if (d->count > 1)
                break;
            d->read_multiple = 0;
            reply = 4;                  /* Unknown command */
            break;
        case 0:                         /* Reply */
            if (d->count <= d->limit) {
                reply = d->buf [d->count++];
                break;
            }
            if (d->read_multiple) {
                /* Next read-multiple block. */
                d->offset += d->blen;
                d->count = 1;
                read_data (d->fd, d->offset, &d->buf[2], d->blen);
                reply = 0;
            }
            break;
        default:                        /* Ignore */
            break;
        }
    }
    if (trace_flag) {
        TRACE ("sdcard%d: send %02x, reply %02x\n", d->unit, data, reply);
    }
    return reply;
}
