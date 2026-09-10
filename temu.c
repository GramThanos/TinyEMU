/*
 * TinyEMU
 * 
 * Copyright (c) 2016-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifdef _WIN32
/* winsock2.h must precede windows.h so the latter does not pull in the
 * conflicting winsock v1 header. */
#include <winsock2.h>
#include <windows.h>
#include <conio.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#endif
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#ifndef _WIN32
#include <termios.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#endif
#include <sys/stat.h>
#include <signal.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"
#ifdef CONFIG_FS_NET
#include "fs_utils.h"
#include "fs_wget.h"
#endif
#ifdef CONFIG_SLIRP
#include "slirp/libslirp.h"
#endif

#ifndef _WIN32

typedef struct {
    int stdin_fd;
    int console_esc_state;
    BOOL resize_pending;
} STDIODevice;

static struct termios oldtty;
static int old_fd0_flags;
static STDIODevice *global_stdio_device;

static void term_exit(void)
{
    tcsetattr (0, TCSANOW, &oldtty);
    fcntl(0, F_SETFL, old_fd0_flags);
}

static void term_init(BOOL allow_ctrlc)
{
    struct termios tty;

    memset(&tty, 0, sizeof(tty));
    tcgetattr (0, &tty);
    oldtty = tty;
    old_fd0_flags = fcntl(0, F_GETFL);

    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
    if (!allow_ctrlc)
        tty.c_lflag &= ~ISIG;
    tty.c_cflag &= ~(CSIZE|PARENB);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    tcsetattr (0, TCSANOW, &tty);

    atexit(term_exit);
}

static void console_write(void *opaque, const uint8_t *buf, int len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}

static int console_read(void *opaque, uint8_t *buf, int len)
{
    STDIODevice *s = opaque;
    int ret, i, j;
    uint8_t ch;
    
    if (len <= 0)
        return 0;

    ret = read(s->stdin_fd, buf, len);
    if (ret < 0)
        return 0;
    if (ret == 0) {
        /* EOF */
        exit(1);
    }

    j = 0;
    for(i = 0; i < ret; i++) {
        ch = buf[i];
        if (s->console_esc_state) {
            s->console_esc_state = 0;
            switch(ch) {
            case 'x':
                printf("Terminated\n");
                exit(0);
            case 'h':
                printf("\n"
                       "C-a h   print this help\n"
                       "C-a x   exit emulator\n"
                       "C-a C-a send C-a\n"
                       );
                break;
            case 1:
                goto output_char;
            default:
                break;
            }
        } else {
            if (ch == 1) {
                s->console_esc_state = 1;
            } else {
            output_char:
                buf[j++] = ch;
            }
        }
    }
    return j;
}

static void term_resize_handler(int sig)
{
    if (global_stdio_device)
        global_stdio_device->resize_pending = TRUE;
}

static void console_get_size(STDIODevice *s, int *pw, int *ph)
{
    struct winsize ws;
    int width, height;
    /* default values */
    width = 80;
    height = 25;
    if (ioctl(s->stdin_fd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col >= 4 && ws.ws_row >= 4) {
        width = ws.ws_col;
        height = ws.ws_row;
    }
    *pw = width;
    *ph = height;
}

CharacterDevice *console_init(BOOL allow_ctrlc)
{
    CharacterDevice *dev;
    STDIODevice *s;
    struct sigaction sig;

    term_init(allow_ctrlc);

    dev = mallocz(sizeof(*dev));
    s = mallocz(sizeof(*s));
    s->stdin_fd = 0;
    /* Note: the glibc does not properly tests the return value of
       write() in printf, so some messages on stdout may be lost */
    fcntl(s->stdin_fd, F_SETFL, O_NONBLOCK);

    s->resize_pending = TRUE;
    global_stdio_device = s;
    
    /* use a signal to get the host terminal resize events */
    sig.sa_handler = term_resize_handler;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = 0;
    sigaction(SIGWINCH, &sig, NULL);
    
    dev->opaque = s;
    dev->write_data = console_write;
    dev->read_data = console_read;
    return dev;
}

#else /* _WIN32 */

/* Windows console backend. Output goes to the console via WriteFile (binary,
 * with ANSI/VT escape processing enabled); input is polled with _kbhit/_getch
 * because Windows console handles cannot be used with select(). */
typedef struct {
    int console_esc_state;
    BOOL resize_pending;
    int last_w, last_h;
} STDIODevice;

static STDIODevice *global_stdio_device;

static void console_write(void *opaque, const uint8_t *buf, int len)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    (void)opaque;
    WriteFile(h, buf, len, &written, NULL);
}

static int console_read(void *opaque, uint8_t *buf, int len)
{
    STDIODevice *s = opaque;
    int j = 0;

    while (j < len && _kbhit()) {
        int c = _getch();
        uint8_t ch;
        /* extended keys (arrows, function keys) arrive as a 0x00/0xE0 prefix
           followed by a scan code; drop both rather than feed garbage. */
        if (c == 0x00 || c == 0xE0) {
            if (_kbhit())
                _getch();
            continue;
        }
        ch = (uint8_t)c;
        if (s->console_esc_state) {
            s->console_esc_state = 0;
            switch(ch) {
            case 'x':
                printf("Terminated\n");
                exit(0);
            case 'h':
                printf("\n"
                       "C-a h   print this help\n"
                       "C-a x   exit emulator\n"
                       "C-a C-a send C-a\n"
                       );
                break;
            case 1:
                buf[j++] = 1;
                break;
            default:
                break;
            }
        } else {
            if (ch == 1)
                s->console_esc_state = 1;
            else
                buf[j++] = ch;
        }
    }
    return j;
}

static void console_get_size(int *pw, int *ph)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    int width = 80, height = 25;

    if (GetConsoleScreenBufferInfo(h, &info)) {
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        int hgt = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (w >= 4 && hgt >= 4) {
            width = w;
            height = hgt;
        }
    }
    *pw = width;
    *ph = height;
}

CharacterDevice *console_init(BOOL allow_ctrlc)
{
    CharacterDevice *dev;
    STDIODevice *s;
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    (void)allow_ctrlc;

    /* enable ANSI escape sequence processing on output (Windows 10+) */
    if (GetConsoleMode(hout, &mode))
        SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    /* raw input: no line buffering / echo (getch bypasses these regardless) */
    if (GetConsoleMode(hin, &mode))
        SetConsoleMode(hin, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                     ENABLE_PROCESSED_INPUT));

    dev = mallocz(sizeof(*dev));
    s = mallocz(sizeof(*s));
    s->resize_pending = TRUE;
    s->last_w = s->last_h = -1;
    global_stdio_device = s;

    dev->opaque = s;
    dev->write_data = console_write;
    dev->read_data = console_read;
    return dev;
}

#endif /* _WIN32 */

typedef enum {
    BF_MODE_RO,
    BF_MODE_RW,
    BF_MODE_SNAPSHOT,
} BlockDeviceModeEnum;

#define SECTOR_SIZE 512

typedef struct BlockDeviceFile {
    FILE *f;
    int64_t nb_sectors;
    BlockDeviceModeEnum mode;
    uint8_t **sector_table;
} BlockDeviceFile;

static int64_t bf_get_sector_count(BlockDevice *bs)
{
    BlockDeviceFile *bf = bs->opaque;
    return bf->nb_sectors;
}

//#define DUMP_BLOCK_READ

static int bf_read_async(BlockDevice *bs,
                         uint64_t sector_num, uint8_t *buf, int n,
                         BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
    //    printf("bf_read_async: sector_num=%" PRId64 " n=%d\n", sector_num, n);
#ifdef DUMP_BLOCK_READ
    {
        static FILE *f;
        if (!f)
            f = fopen("/tmp/read_sect.txt", "wb");
        fprintf(f, "%" PRId64 " %d\n", sector_num, n);
    }
#endif
    if (!bf->f)
        return -1;
    /* An out-of-range request must never walk off sector_table or issue a
       negative-length read: return an I/O error to the guest instead of
       corrupting/segfaulting the host. This also stops a too-small file
       (e.g. a chunk manifest mistaken for a raw image) from crashing here. */
    if (n < 0 || sector_num + (uint64_t)n > (uint64_t)bf->nb_sectors)
        return -1;
    if (bf->mode == BF_MODE_SNAPSHOT) {
        int i;
        for(i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
                fread(buf, 1, SECTOR_SIZE, bf->f);
            } else {
                memcpy(buf, bf->sector_table[sector_num], SECTOR_SIZE);
            }
            sector_num++;
            buf += SECTOR_SIZE;
        }
    } else {
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fread(buf, 1, n * SECTOR_SIZE, bf->f);
    }
    /* synchronous read */
    return 0;
}

static int bf_write_async(BlockDevice *bs,
                          uint64_t sector_num, const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
    int ret;

    if (n < 0 || sector_num + (uint64_t)n > (uint64_t)bf->nb_sectors)
        return -1;
    switch(bf->mode) {
    case BF_MODE_RO:
        ret = -1; /* error */
        break;
    case BF_MODE_RW:
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fwrite(buf, 1, n * SECTOR_SIZE, bf->f);
        ret = 0;
        break;
    case BF_MODE_SNAPSHOT:
        {
            int i;
            if ((sector_num + n) > bf->nb_sectors)
                return -1;
            for(i = 0; i < n; i++) {
                if (!bf->sector_table[sector_num]) {
                    bf->sector_table[sector_num] = malloc(SECTOR_SIZE);
                }
                memcpy(bf->sector_table[sector_num], buf, SECTOR_SIZE);
                sector_num++;
                buf += SECTOR_SIZE;
            }
            ret = 0;
        }
        break;
    default:
        abort();
    }

    return ret;
}

static BlockDevice *block_device_init(const char *filename,
                                      BlockDeviceModeEnum mode)
{
    BlockDevice *bs;
    BlockDeviceFile *bf;
    int64_t file_size;
    FILE *f;
    const char *mode_str;

    if (mode == BF_MODE_RW) {
        mode_str = "r+b";
    } else {
        mode_str = "rb";
    }
    
    f = fopen(filename, mode_str);
    if (!f) {
        perror(filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);

    bs = mallocz(sizeof(*bs));
    bf = mallocz(sizeof(*bf));

    bf->mode = mode;
    bf->nb_sectors = file_size / 512;
    bf->f = f;

    if (mode == BF_MODE_SNAPSHOT) {
        bf->sector_table = mallocz(sizeof(bf->sector_table[0]) *
                                   bf->nb_sectors);
    }
    
    bs->opaque = bf;
    bs->get_sector_count = bf_get_sector_count;
    bs->read_async = bf_read_async;
    bs->write_async = bf_write_async;
    return bs;
}

/**********************************************************/
/* local split (chunked) block device
 *
 * Serves a disk from the multi-file layout produced by splitimg: a JSON
 * manifest ("blk.txt": { block_size (in KB), n_block }) plus "blk%09u.bin"
 * chunk files of block_size KB each, sitting in the same directory. This is
 * the exact on-disk layout the browser build fetches over HTTP
 * (block_device_init_http). Recognising it for local files lets a config.cfg
 * point drive0 at a chunked image with the native/Docker build too, instead of
 * the manifest being mistaken for a raw image. */

typedef struct BlockDeviceSplit {
    BlockDeviceModeEnum mode;
    int64_t nb_sectors;
    int block_size;         /* sectors per chunk, power of two */
    int n_block;
    char *dir;              /* manifest directory, incl. trailing separator */
    uint8_t **sector_table; /* write overlay (SNAPSHOT), lazily populated */
    FILE *cur_f;            /* cached open chunk file for reads */
    int cur_block;          /* chunk index of cur_f, -1 if none */
} BlockDeviceSplit;

static int64_t bs_split_get_sector_count(BlockDevice *bs)
{
    BlockDeviceSplit *bf = bs->opaque;
    return bf->nb_sectors;
}

static FILE *bs_split_open_chunk(BlockDeviceSplit *bf, int block_num)
{
    char filename[1024];
    if (bf->cur_f && bf->cur_block == block_num)
        return bf->cur_f;
    if (bf->cur_f) {
        fclose(bf->cur_f);
        bf->cur_f = NULL;
        bf->cur_block = -1;
    }
    snprintf(filename, sizeof(filename), "%sblk%09u.bin", bf->dir, block_num);
    bf->cur_f = fopen(filename, "rb");
    if (bf->cur_f)
        bf->cur_block = block_num;
    return bf->cur_f;
}

static int bs_split_read_async(BlockDevice *bs,
                               uint64_t sector_num, uint8_t *buf, int n,
                               BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceSplit *bf = bs->opaque;
    int i;

    if (n < 0 || sector_num + (uint64_t)n > (uint64_t)bf->nb_sectors)
        return -1;
    for(i = 0; i < n; i++) {
        if (bf->sector_table && bf->sector_table[sector_num]) {
            memcpy(buf, bf->sector_table[sector_num], SECTOR_SIZE);
        } else {
            int block_num = sector_num / bf->block_size;
            int offset = sector_num % bf->block_size;
            FILE *f = bs_split_open_chunk(bf, block_num);
            /* a missing or short chunk reads back as zeroes */
            memset(buf, 0, SECTOR_SIZE);
            if (f) {
                fseek(f, (int64_t)offset * SECTOR_SIZE, SEEK_SET);
                if (fread(buf, 1, SECTOR_SIZE, f) != SECTOR_SIZE) {
                    /* leave the zero fill */
                }
            }
        }
        sector_num++;
        buf += SECTOR_SIZE;
    }
    return 0;
}

static int bs_split_write_async(BlockDevice *bs,
                                uint64_t sector_num, const uint8_t *buf, int n,
                                BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceSplit *bf = bs->opaque;
    int i;

    if (n < 0 || sector_num + (uint64_t)n > (uint64_t)bf->nb_sectors)
        return -1;
    switch(bf->mode) {
    case BF_MODE_RO:
        return -1;
    case BF_MODE_SNAPSHOT:
        /* copy-on-write into an in-memory overlay; chunk files untouched */
        for(i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num])
                bf->sector_table[sector_num] = malloc(SECTOR_SIZE);
            memcpy(bf->sector_table[sector_num], buf, SECTOR_SIZE);
            sector_num++;
            buf += SECTOR_SIZE;
        }
        return 0;
    case BF_MODE_RW:
        for(i = 0; i < n; i++) {
            int block_num = sector_num / bf->block_size;
            int offset = sector_num % bf->block_size;
            char filename[1024];
            FILE *f;
            /* drop the read cache if it points at the chunk we are writing */
            if (bf->cur_f && bf->cur_block == block_num) {
                fclose(bf->cur_f);
                bf->cur_f = NULL;
                bf->cur_block = -1;
            }
            snprintf(filename, sizeof(filename), "%sblk%09u.bin",
                     bf->dir, block_num);
            f = fopen(filename, "r+b");
            if (!f)
                return -1;
            fseek(f, (int64_t)offset * SECTOR_SIZE, SEEK_SET);
            fwrite(buf, 1, SECTOR_SIZE, f);
            fclose(f);
            sector_num++;
            buf += SECTOR_SIZE;
        }
        return 0;
    default:
        return -1;
    }
}

/* If filename is a splitimg JSON manifest, build a chunked block device from
   it; otherwise return NULL so the caller falls back to a raw image. */
static BlockDevice *block_device_init_split(const char *filename,
                                            BlockDeviceModeEnum mode)
{
    BlockDevice *bs;
    BlockDeviceSplit *bf;
    FILE *f;
    int64_t file_size;
    char *data;
    const char *p;
    int block_size_kb, n_block, block_size, sep;
    JSONValue cfg, vbs, vnb;

    f = fopen(filename, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);
    /* a manifest is tiny; anything sizeable is a raw image */
    if (file_size <= 0 || file_size > 65536) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    data = malloc(file_size + 1);
    if (fread(data, 1, file_size, f) != (size_t)file_size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    data[file_size] = '\0';

    /* quick reject: JSON objects start with '{' */
    p = data;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '{') {
        free(data);
        return NULL;
    }

    cfg = json_parse_value_len(data, file_size);
    free(data);
    if (json_is_error(cfg)) {
        json_free(cfg);
        return NULL;
    }
    /* must carry both manifest fields to be treated as a split image */
    vbs = json_object_get(cfg, "block_size");
    vnb = json_object_get(cfg, "n_block");
    if (vbs.type != JSON_INT || vnb.type != JSON_INT) {
        json_free(cfg);
        return NULL;
    }
    block_size_kb = vbs.u.int32;
    n_block = vnb.u.int32;
    json_free(cfg);

    block_size = block_size_kb * 2; /* KB -> 512-byte sectors */
    if (block_size_kb <= 0 || (block_size & (block_size - 1)) != 0 ||
        n_block <= 0) {
        vm_error("%s: invalid split-image manifest\n", filename);
        exit(1);
    }

    bs = mallocz(sizeof(*bs));
    bf = mallocz(sizeof(*bf));
    bf->mode = mode;
    bf->block_size = block_size;
    bf->n_block = n_block;
    bf->nb_sectors = (int64_t)block_size * n_block;
    bf->cur_f = NULL;
    bf->cur_block = -1;

    /* directory prefix of the manifest (handles both separators) */
    sep = -1;
    for(p = filename; *p; p++) {
        if (*p == '/' || *p == '\\')
            sep = p - filename;
    }
    bf->dir = mallocz(sep + 2);
    if (sep >= 0)
        memcpy(bf->dir, filename, sep + 1); /* keep trailing separator */

    if (mode == BF_MODE_SNAPSHOT)
        bf->sector_table = mallocz(sizeof(bf->sector_table[0]) *
                                   bf->nb_sectors);

    bs->opaque = bf;
    bs->get_sector_count = bs_split_get_sector_count;
    bs->read_async = bs_split_read_async;
    bs->write_async = bs_split_write_async;
    return bs;
}

#ifndef _WIN32

typedef struct {
    int fd;
    BOOL select_filled;
} TunState;

static void tun_write_packet(EthernetDevice *net,
                             const uint8_t *buf, int len)
{
    TunState *s = net->opaque;
    write(s->fd, buf, len);
}

static void tun_select_fill(EthernetDevice *net, int *pfd_max,
                            fd_set *rfds, fd_set *wfds, fd_set *efds,
                            int *pdelay)
{
    TunState *s = net->opaque;
    int net_fd = s->fd;

    s->select_filled = net->device_can_write_packet(net);
    if (s->select_filled) {
        FD_SET(net_fd, rfds);
        *pfd_max = max_int(*pfd_max, net_fd);
    }
}

static void tun_select_poll(EthernetDevice *net, 
                            fd_set *rfds, fd_set *wfds, fd_set *efds,
                            int select_ret)
{
    TunState *s = net->opaque;
    int net_fd = s->fd;
    uint8_t buf[2048];
    int ret;
    
    if (select_ret <= 0)
        return;
    if (s->select_filled && FD_ISSET(net_fd, rfds)) {
        ret = read(net_fd, buf, sizeof(buf));
        if (ret > 0)
            net->device_write_packet(net, buf, ret);
    }
    
}

/* configure with:
# bridge configuration (connect tap0 to bridge interface br0)
   ip link add br0 type bridge
   ip tuntap add dev tap0 mode tap [user x] [group x]
   ip link set tap0 master br0
   ip link set dev br0 up
   ip link set dev tap0 up

# NAT configuration (eth1 is the interface connected to internet)
   ifconfig br0 192.168.3.1
   echo 1 > /proc/sys/net/ipv4/ip_forward
   iptables -D FORWARD 1
   iptables -t nat -A POSTROUTING -o eth1 -j MASQUERADE

   In the VM:
   ifconfig eth0 192.168.3.2
   route add -net 0.0.0.0 netmask 0.0.0.0 gw 192.168.3.1
*/
static EthernetDevice *tun_open(const char *ifname)
{
    struct ifreq ifr;
    int fd, ret;
    EthernetDevice *net;
    TunState *s;
    
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: could not open /dev/net/tun\n");
        return NULL;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    pstrcpy(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (ret != 0) {
        fprintf(stderr, "Error: could not configure /dev/net/tun\n");
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    net = mallocz(sizeof(*net));
    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;
    s = mallocz(sizeof(*s));
    s->fd = fd;
    net->opaque = s;
    net->write_packet = tun_write_packet;
    net->select_fill = tun_select_fill;
    net->select_poll = tun_select_poll;
    return net;
}

#endif /* !_WIN32 */

#ifdef CONFIG_SLIRP

/*******************************************************/
/* slirp */

static Slirp *slirp_state;

static void slirp_write_packet(EthernetDevice *net,
                               const uint8_t *buf, int len)
{
    Slirp *slirp_state = net->opaque;
    slirp_input(slirp_state, buf, len);
}

int slirp_can_output(void *opaque)
{
    EthernetDevice *net = opaque;
    return net->device_can_write_packet(net);
}

void slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
    EthernetDevice *net = opaque;
    return net->device_write_packet(net, pkt, pkt_len);
}

static void slirp_select_fill1(EthernetDevice *net, int *pfd_max,
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int *pdelay)
{
    Slirp *slirp_state = net->opaque;
    slirp_select_fill(slirp_state, pfd_max, rfds, wfds, efds);
}

static void slirp_select_poll1(EthernetDevice *net, 
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int select_ret)
{
    Slirp *slirp_state = net->opaque;
    slirp_select_poll(slirp_state, rfds, wfds, efds, (select_ret <= 0));
}

static EthernetDevice *slirp_open(void)
{
    EthernetDevice *net;
    struct in_addr net_addr  = { .s_addr = htonl(0x0a000200) }; /* 10.0.2.0 */
    struct in_addr mask = { .s_addr = htonl(0xffffff00) }; /* 255.255.255.0 */
    struct in_addr host = { .s_addr = htonl(0x0a000202) }; /* 10.0.2.2 */
    struct in_addr dhcp = { .s_addr = htonl(0x0a00020f) }; /* 10.0.2.15 */
    struct in_addr dns  = { .s_addr = htonl(0x0a000203) }; /* 10.0.2.3 */
    const char *bootfile = NULL;
    const char *vhostname = NULL;
    int restricted = 0;
    
    if (slirp_state) {
        fprintf(stderr, "Only a single slirp instance is allowed\n");
        return NULL;
    }
    net = mallocz(sizeof(*net));

    slirp_state = slirp_init(restricted, net_addr, mask, host, vhostname,
                             "", bootfile, dhcp, dns, net);
    
    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;
    net->opaque = slirp_state;
    net->write_packet = slirp_write_packet;
    net->select_fill = slirp_select_fill1;
    net->select_poll = slirp_select_poll1;
    
    return net;
}

#endif /* CONFIG_SLIRP */

#define MAX_EXEC_CYCLE 500000
#define MAX_SLEEP_TIME 10 /* in ms */

void virt_machine_run(VirtMachine *m)
{
    fd_set rfds, wfds, efds;
    int fd_max, ret, delay;
    struct timeval tv;
#ifndef _WIN32
    int stdin_fd;
#endif
    
    delay = virt_machine_get_sleep_duration(m, MAX_SLEEP_TIME);
    
    /* wait for an event */
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    fd_max = -1;
    if (m->console_dev && virtio_console_can_write_data(m->console_dev)) {
        STDIODevice *s = m->console->opaque;
#ifndef _WIN32
        stdin_fd = s->stdin_fd;
        FD_SET(stdin_fd, &rfds);
        fd_max = stdin_fd;

        if (s->resize_pending) {
            int width, height;
            console_get_size(s, &width, &height);
            virtio_console_resize_event(m->console_dev, width, height);
            s->resize_pending = FALSE;
        }
#else
        /* console input is polled after select() (not selectable on Windows);
           here we only propagate host console resizes to the guest. */
        int width, height;
        console_get_size(&width, &height);
        if (s->resize_pending || width != s->last_w || height != s->last_h) {
            virtio_console_resize_event(m->console_dev, width, height);
            s->resize_pending = FALSE;
            s->last_w = width;
            s->last_h = height;
        }
#endif
    }
    if (m->net) {
        m->net->select_fill(m->net, &fd_max, &rfds, &wfds, &efds, &delay);
    }
#ifdef CONFIG_FS_NET
    fs_net_set_fdset(&fd_max, &rfds, &wfds, &efds, &delay);
#endif
    tv.tv_sec = delay / 1000;
    tv.tv_usec = (delay % 1000) * 1000;
#ifdef _WIN32
    /* select() with no descriptors errors out immediately on Windows, which
       would busy-loop; sleep for the delay instead. */
    if (fd_max < 0) {
        Sleep(delay);
        ret = 0;
    } else
#endif
    ret = select(fd_max + 1, &rfds, &wfds, &efds, &tv);
    if (m->net) {
        m->net->select_poll(m->net, &rfds, &wfds, &efds, ret);
    }
    if (ret > 0) {
#ifndef _WIN32
        if (m->console_dev && FD_ISSET(stdin_fd, &rfds)) {
            uint8_t buf[128];
            int ret, len;
            len = virtio_console_get_write_len(m->console_dev);
            len = min_int(len, sizeof(buf));
            ret = m->console->read_data(m->console->opaque, buf, len);
            if (ret > 0) {
                virtio_console_write_data(m->console_dev, buf, ret);
            }
        }
#endif
    }
#ifdef _WIN32
    /* Windows console input is not selectable; poll it each iteration. */
    if (m->console_dev && virtio_console_can_write_data(m->console_dev)) {
        uint8_t buf[128];
        int rlen, len;
        len = virtio_console_get_write_len(m->console_dev);
        len = min_int(len, (int)sizeof(buf));
        rlen = m->console->read_data(m->console->opaque, buf, len);
        if (rlen > 0)
            virtio_console_write_data(m->console_dev, buf, rlen);
    }
#endif

#ifdef CONFIG_SDL
    sdl_refresh(m);
#endif
    
    virt_machine_interp(m, MAX_EXEC_CYCLE);
}

/*******************************************************/

static struct option options[] = {
    { "help", no_argument, NULL, 'h' },
    { "ctrlc", no_argument },
    { "rw", no_argument },
    { "ro", no_argument },
    { "append", required_argument },
    { "no-accel", no_argument },
    { "build-preload", required_argument },
    { NULL },
};

static void print_backend_info(void)
{
#if defined(USE_KVM)
    {
        int kvm_fd = open("/dev/kvm", O_RDONLY);
        if (kvm_fd >= 0) {
            printf("x86 backend : KVM (available)\n");
            close(kvm_fd);
        } else {
            printf("x86 backend : KVM (built-in, /dev/kvm not accessible on this host)\n");
        }
    }
#else
    printf("x86 backend : software interpreter (no acceleration)\n");
#endif
}

void help(void)
{
    printf("temu version " CONFIG_VERSION ", Copyright (c) 2016-2018 Fabrice Bellard\n"
           "usage: temu [options] config_file\n"
           "options are:\n"
           "-m ram_size       set the RAM size in MB\n"
           "-rw               allow write access to the disk image (default=snapshot)\n"
           "-ctrlc            the C-c key stops the emulator instead of being sent to the\n"
           "                  emulated software\n"
           "-append cmdline   append cmdline to the kernel command line\n"
           "-no-accel         disable VM acceleration (KVM, x86 machine only)\n"
           "\n"
           "Console keys:\n"
           "Press C-a x to exit the emulator, C-a h to get some help.\n"
           "\n");
    print_backend_info();
    exit(1);
}

#ifdef CONFIG_FS_NET
static BOOL net_completed;

static void net_start_cb(void *arg)
{
    net_completed = TRUE;
}

static BOOL net_poll_cb(void *arg)
{
    return net_completed;
}

#endif

int main(int argc, char **argv)
{
    VirtMachine *s;
    const char *path, *cmdline, *build_preload_file;
    int c, option_index, i, ram_size, accel_enable;
    BOOL allow_ctrlc;
    BlockDeviceModeEnum drive_mode;
    VirtMachineParams p_s, *p = &p_s;

    ram_size = -1;
    allow_ctrlc = FALSE;
    (void)allow_ctrlc;
    drive_mode = BF_MODE_SNAPSHOT;
    accel_enable = -1;
    cmdline = NULL;
    build_preload_file = NULL;
    for(;;) {
        c = getopt_long_only(argc, argv, "hm:", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 0:
            switch(option_index) {
            case 1: /* ctrlc */
                allow_ctrlc = TRUE;
                break;
            case 2: /* rw */
                drive_mode = BF_MODE_RW;
                break;
            case 3: /* ro */
                drive_mode = BF_MODE_RO;
                break;
            case 4: /* append */
                cmdline = optarg;
                break;
            case 5: /* no-accel */
                accel_enable = FALSE;
                break;
            case 6: /* build-preload */
                build_preload_file = optarg;
                break;
            default:
                fprintf(stderr, "unknown option index: %d\n", option_index);
                exit(1);
            }
            break;
        case 'h':
            help();
            break;
        case 'm':
            ram_size = strtoul(optarg, NULL, 0);
            break;
        default:
            exit(1);
        }
    }

    if (optind >= argc) {
        help();
    }

    path = argv[optind++];

    virt_machine_set_defaults(p);
#ifdef CONFIG_FS_NET
    fs_wget_init();
#endif
    virt_machine_load_config_file(p, path, NULL, NULL);
#ifdef CONFIG_FS_NET
    fs_net_event_loop(NULL, NULL);
#endif

    /* override some config parameters */

    if (ram_size > 0) {
        p->ram_size = (uint64_t)ram_size << 20;
    }
    if (accel_enable != -1)
        p->accel_enable = accel_enable;
    if (cmdline) {
        vm_add_cmdline(p, cmdline);
    }
    
    /* open the files & devices */
    for(i = 0; i < p->drive_count; i++) {
        BlockDevice *drive;
        char *fname;
        fname = get_file_path(p->cfg_filename, p->tab_drive[i].filename);
#ifdef CONFIG_FS_NET
        if (is_url(fname)) {
            net_completed = FALSE;
            drive = block_device_init_http(fname, 128 * 1024,
                                           net_start_cb, NULL);
            /* wait until the drive is initialized */
            fs_net_event_loop(net_poll_cb, NULL);
        } else
#endif
        {
            /* recognise a splitimg chunk manifest (e.g. blk.txt); fall back to
               a raw image otherwise */
            drive = block_device_init_split(fname, drive_mode);
            if (!drive)
                drive = block_device_init(fname, drive_mode);
        }
        free(fname);
        p->tab_drive[i].block_dev = drive;
    }

    for(i = 0; i < p->fs_count; i++) {
        FSDevice *fs;
        const char *path;
        path = p->tab_fs[i].filename;
#ifdef CONFIG_FS_NET
        if (is_url(path)) {
            fs = fs_net_init(path, NULL, NULL);
            if (!fs)
                exit(1);
            if (build_preload_file)
                fs_dump_cache_load(fs, build_preload_file);
            fs_net_event_loop(NULL, NULL);
        } else
#endif
        {
#ifdef _WIN32
            fprintf(stderr, "Filesystem access not supported yet\n");
            exit(1);
#else
            char *fname;
            fname = get_file_path(p->cfg_filename, path);
            fs = fs_disk_init(fname);
            if (!fs) {
                fprintf(stderr, "%s: must be a directory\n", fname);
                exit(1);
            }
            free(fname);
#endif
        }
        p->tab_fs[i].fs_dev = fs;
    }

    for(i = 0; i < p->eth_count; i++) {
#ifdef CONFIG_SLIRP
        if (!strcmp(p->tab_eth[i].driver, "user")) {
            p->tab_eth[i].net = slirp_open();
            if (!p->tab_eth[i].net)
                exit(1);
        } else
#endif
#ifndef _WIN32
        if (!strcmp(p->tab_eth[i].driver, "tap")) {
            p->tab_eth[i].net = tun_open(p->tab_eth[i].ifname);
            if (!p->tab_eth[i].net)
                exit(1);
        } else
#endif
        {
            fprintf(stderr, "Unsupported network driver '%s'\n",
                    p->tab_eth[i].driver);
            exit(1);
        }
    }
    
#ifdef CONFIG_SDL
    if (p->display_device) {
        sdl_init(p->width, p->height);
    } else
#endif
    {
        p->console = console_init(allow_ctrlc);
    }
    p->rtc_real_time = TRUE;

    s = virt_machine_init(p);
    if (!s)
        exit(1);

    virt_machine_free_config(p);

    if (s->net) {
        s->net->device_set_carrier(s->net, TRUE);
    }
    
    for(;;) {
        virt_machine_run(s);
    }
    virt_machine_end(s);
    return 0;
}
