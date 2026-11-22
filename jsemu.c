/*
 * JS emulator main
 * 
 * Copyright (c) 2016-2017 Fabrice Bellard
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
#include <emscripten.h>
#include <emscripten/heap.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"
#include "list.h"
#include "fbuf.h"
#ifdef CONFIG_X86EMU
#include "x86_cpu.h"
#endif

void virt_machine_run(void *opaque);

/* provided in lib.js */
extern void console_write(void *opaque, const uint8_t *buf, int len);
extern void console_get_size(int *pw, int *ph);
extern void fb_refresh(void *opaque, void *data,
                       int x, int y, int w, int h, int stride);
extern void net_recv_packet(EthernetDevice *bs,
                            const uint8_t *buf, int len);

static uint8_t console_fifo[1024];
static int console_fifo_windex;
static int console_fifo_rindex;
static int console_fifo_count;
static BOOL console_resize_pending;

static int global_width;
static int global_height;
static VirtMachine *global_vm;
static BOOL global_carrier_state;

/* Run-loop control. The emulator drives itself with a self-rescheduling
 * emscripten_async_call(virt_machine_run, ...) chain that JS cannot suspend from
 * outside, so pause/stop are implemented here and exported for the JS host
 * (see jsLab's lib/vm/EmulatorEngine.js, which cwraps these if present). */
static BOOL global_paused;
static BOOL global_stopped;
/* A virt_machine_run call is pending. Pausing ends the chain only when that
 * call fires, so a resume before then must not start a second chain. */
static BOOL run_scheduled;

static void schedule_run(VirtMachine *m, int delay)
{
    run_scheduled = TRUE;
    emscripten_async_call(virt_machine_run, m, delay);
}

static int console_read(void *opaque, uint8_t *buf, int len)
{
    int out_len, l;
    len = min_int(len, console_fifo_count);
    console_fifo_count -= len;
    out_len = 0;
    while (len != 0) {
        l = min_int(len, sizeof(console_fifo) - console_fifo_rindex);
        memcpy(buf + out_len, console_fifo + console_fifo_rindex, l);
        len -= l;
        out_len += l;
        console_fifo_rindex += l;
        if (console_fifo_rindex == sizeof(console_fifo))
            console_fifo_rindex = 0;
    }
    return out_len;
}

/* called from JS */
void console_queue_char(int c)
{
    if (console_fifo_count < sizeof(console_fifo)) {
        console_fifo[console_fifo_windex] = c;
        if (++console_fifo_windex == sizeof(console_fifo))
            console_fifo_windex = 0;
        console_fifo_count++;
    }
}

/* called from JS */
void display_key_event(int is_down, int key_code)
{
    if (global_vm) {
        vm_send_key_event(global_vm, is_down, key_code);
    }
}

/* called from JS */
static int mouse_last_x, mouse_last_y, mouse_last_buttons;

void display_mouse_event(int dx, int dy, int buttons)
{
    if (global_vm) {
        if (vm_mouse_is_absolute(global_vm) || 1) {
            dx = min_int(dx, global_width - 1);
            dy = min_int(dy, global_height - 1);
            dx = (dx * VIRTIO_INPUT_ABS_SCALE) / global_width;
            dy = (dy * VIRTIO_INPUT_ABS_SCALE) / global_height;
        } else {
            /* relative mouse is not supported */
            dx = 0;
            dy = 0;
        }
        mouse_last_x = dx;
        mouse_last_y = dy;
        mouse_last_buttons = buttons;
        vm_send_mouse_event(global_vm, dx, dy, 0, buttons);
    }
}

/* called from JS */
void display_wheel_event(int dz)
{
    if (global_vm) {
        vm_send_mouse_event(global_vm, mouse_last_x, mouse_last_y, dz,
                            mouse_last_buttons);
    }
}

/* called from JS: suspend the run loop after the current tick */
void vm_pause(void)
{
    global_paused = TRUE;
}

/* called from JS: resume a paused run loop by re-arming the reschedule chain */
void vm_resume(void)
{
    BOOL was_paused = global_paused;
    global_paused = FALSE;
    if (was_paused && !global_stopped && global_vm && !run_scheduled)
        schedule_run(global_vm, 0);
}

/* Snapshots (see jsLab's lib/vm/EmulatorEngine.js). The page saves the whole WASM
 * memory, which holds the machine, between two run-loop ticks, so no C code is
 * running. Restoring copies it into a fresh instance of the same build. */

/* called from JS: statistics for the page. Doubles, since JS numbers are. */
extern int64_t block_net_chunks_fetched;
extern int64_t block_net_bytes_fetched;

/* instructions (or cycles) the CPU has run since boot; -1 if unknown */
double vm_cycles(void)
{
    if (!global_vm || !global_vm->vmc->vm_get_cycles)
        return -1;
    return (double)global_vm->vmc->vm_get_cycles(global_vm);
}

double vm_disk_chunks(void)
{
    return (double)block_net_chunks_fetched;
}

double vm_disk_bytes(void)
{
    return (double)block_net_bytes_fetched;
}

/* blocks the x86 JIT has compiled since boot, counted again when their page
 * is recompiled; -1 when it is off */
double vm_jit_blocks(void)
{
#ifdef CONFIG_X86EMU
    return x86_jit_blocks();
#else
    return -1;
#endif
}

/* called from JS: the emulated timers' clock, saved with a snapshot */
double vm_snapshot_clock(void)
{
    return (double)host_clock_ns();
}

/* called from JS: grow the heap to `size` bytes before a snapshot is copied in.
 * Returns 1 on success. */
int vm_reserve_memory(double size)
{
    return emscripten_resize_heap((size_t)size);
}

/* called from JS once a snapshot's memory is in place: restart the timers'
 * clock where the snapshot left it and start the run loop. The memory says
 * whatever the saved instance's loop was doing; nothing is scheduled here. */
void vm_restore(double clock_ns)
{
    host_clock_set((int64_t)clock_ns);
#ifdef CONFIG_X86EMU
    /* compiled x86 blocks live in the JS instance that saved the snapshot */
    x86_jit_forget();
#endif
    run_scheduled = FALSE;
    global_stopped = FALSE;
    global_paused = TRUE;
    console_resize_pending = TRUE;
    vm_resume();
}

/* called from JS: halt the run loop permanently (the reschedule chain stops on
 * the next tick). Used for a clean inline-mode stop; in worker mode the host
 * also terminates the worker. */
void vm_stop(void)
{
    global_stopped = TRUE;
}

/* called from JS */
void net_write_packet(const uint8_t *buf, int buf_len)
{
    EthernetDevice *net = global_vm->net;
    if (net) {
        net->device_write_packet(net, buf, buf_len);
    }
}

/* called from JS */
void net_set_carrier(BOOL carrier_state)
{
    EthernetDevice *net;
    global_carrier_state = carrier_state;
    if (global_vm && global_vm->net) {
        net = global_vm->net;
        net->device_set_carrier(net, carrier_state);
    }
}

static void fb_refresh1(FBDevice *fb_dev, void *opaque,
                        int x, int y, int w, int h)
{
    int stride = fb_dev->stride;
    fb_refresh(opaque, fb_dev->fb_data + y * stride + x * 4, x, y, w, h,
               stride);
}

static CharacterDevice *console_init(void)
{
    CharacterDevice *dev;
    console_resize_pending = TRUE;
    dev = mallocz(sizeof(*dev));
    dev->write_data = console_write;
    dev->read_data = console_read;
    return dev;
}

typedef struct {
    VirtMachineParams *p;
    int ram_size;
    char *cmdline;
    BOOL has_network;
    char *pwd;
    int fs_index;
    int drive_index;
} VMStartState;

static void init_vm(void *arg);
static void init_vm_fs(void *arg);
static void init_vm_fs_next(void *arg);
static void init_vm_drive(void *arg);
static void init_vm_drive_next(void *arg);

void vm_start(const char *url, int ram_size, const char *cmdline,
              const char *pwd, int width, int height, BOOL has_network)
{
    VMStartState *s;

    s = mallocz(sizeof(*s));
    s->ram_size = ram_size;
    s->cmdline = strdup(cmdline);
    if (pwd)
        s->pwd = strdup(pwd);
    global_width = width;
    global_height = height;
    s->has_network = has_network;
    s->p = mallocz(sizeof(VirtMachineParams));
    virt_machine_set_defaults(s->p);
    virt_machine_load_config_file(s->p, url, init_vm_fs, s);
}

static void init_vm_fs(void *arg)
{
    VMStartState *s = arg;
    s->fs_index = 0;
    init_vm_fs_next(s);
}

/* Initialise filesystems one at a time. A "js:<name>" entry is a JS-backed
 * volume (fs_js.c) whose store is already loaded, so it inits synchronously and
 * we continue in the loop; a network 9p entry loads asynchronously, so we init
 * it with this same function as its completion callback and return, resuming at
 * the next index when the load finishes. */
static void init_vm_fs_next(void *arg)
{
    VMStartState *s = arg;
    VirtMachineParams *p = s->p;

    while (s->fs_index < p->fs_count) {
        int i = s->fs_index;
        const char *fname = p->tab_fs[i].filename;
#ifdef EMSCRIPTEN
        if (strncmp(fname, "js:", 3) == 0) {
            p->tab_fs[i].fs_dev = fs_js_init(fname + 3);
            s->fs_index++;
            continue;   /* synchronous: go straight to the next entry */
        }
#endif
        p->tab_fs[i].fs_dev = fs_net_init(fname, init_vm_fs_next, s);
        if (s->pwd)
            fs_net_set_pwd(p->tab_fs[i].fs_dev, s->pwd);
        s->fs_index++;
        return;         /* asynchronous: resume in the completion callback */
    }
    init_vm_drive(s);
}

static void init_vm_drive(void *arg)
{
    VMStartState *s = arg;
    s->drive_index = 0;
    init_vm_drive_next(s);
}

/* Initialise drives one at a time. A "js:<name>" entry is a JS-backed disk
 * (block_js.c) whose store is already open, so it inits synchronously; an HTTP
 * image loads asynchronously, so we init it with this function as its start
 * callback and resume at the next drive when the header arrives. */
static void init_vm_drive_next(void *arg)
{
    VMStartState *s = arg;
    VirtMachineParams *p = s->p;

    while (s->drive_index < p->drive_count) {
        int i = s->drive_index;
        const char *fname = p->tab_drive[i].filename;
#ifdef EMSCRIPTEN
        if (strncmp(fname, "js:", 3) == 0) {
            p->tab_drive[i].block_dev = block_device_init_js(fname + 3);
            s->drive_index++;
            continue;   /* synchronous */
        }
#endif
        /* Relative to the config file, as the kernel already is (and as temu.c
         * does): left raw, the XHR would resolve it against the script running
         * the engine, which in a worker is not the page. */
        char *url = get_file_path(p->cfg_filename, fname);
        p->tab_drive[i].block_dev =
            block_device_init_http(url, 131072, init_vm_drive_next, s);
        free(url);   /* copied by block_device_init_http and the XHR */
        s->drive_index++;
        return;         /* asynchronous: resume in the start callback */
    }
    init_vm(s);
}

static void init_vm(void *arg)
{
    VMStartState *s = arg;
    VirtMachine *m;
    VirtMachineParams *p = s->p;
    int i;
    
    p->rtc_real_time = TRUE;
    p->ram_size = s->ram_size << 20;
    if (s->cmdline && s->cmdline[0] != '\0') {
        vm_add_cmdline(s->p, s->cmdline);
    }

    if (global_width > 0 && global_height > 0) {
        /* enable graphic output if needed */
        if (!p->display_device)
            p->display_device = strdup("simplefb");
        p->width = global_width;
        p->height = global_height;
    } else {
        p->console = console_init();
    }
    
    if (p->eth_count > 0 && !s->has_network) {
        /* remove the interfaces */
        for(i = 0; i < p->eth_count; i++) {
            free(p->tab_eth[i].ifname);
            free(p->tab_eth[i].driver);
        }
        p->eth_count = 0;
    }

    if (p->eth_count > 0) {
        EthernetDevice *net;
        int i;
        assert(p->eth_count == 1);
        net = mallocz(sizeof(EthernetDevice));
        net->mac_addr[0] = 0x02;
        for(i = 1; i < 6; i++)
            net->mac_addr[i] = (int)(emscripten_random() * 256);
        net->write_packet = net_recv_packet;
        net->opaque = NULL;
        p->tab_eth[0].net = net;
    }

    m = virt_machine_init(p);
    global_vm = m;

    virt_machine_free_config(s->p);

    if (m->net) {
        m->net->device_set_carrier(m->net, global_carrier_state);
    }
    
    free(s->p);
    free(s->cmdline);
    if (s->pwd) {
        memset(s->pwd, 0, strlen(s->pwd));
        free(s->pwd);
    }
    free(s);

    schedule_run(m, 0);
}

/* need to be long enough to hide the non zero delay of setTimeout(_, 0) */
#define MAX_EXEC_TOTAL_CYCLE 3000000
#define MAX_EXEC_CYCLE        200000

#define MAX_SLEEP_TIME 10 /* in ms */

/* A browser stops this loop when it freezes the tab, the computer sleeps or a
 * debugger pauses the worker, but the host's monotonic clock runs on. Left alone,
 * the guest would then see minutes pass in one tick: Linux reports an RCU stall
 * ("timer wakeup didn't happen for N jiffies") and every timer fires late at
 * once. So a gap between two ticks longer than PAUSE_GAP_NS is taken out of the
 * emulated timers' clock, as restoring a snapshot does. The guest's wall clock
 * then lags; the page sets its date again after a pause. Native builds don't
 * need this: their loop isn't stopped from outside. */
#define PAUSE_GAP_NS 5000000000LL
static int64_t last_tick_ns;     /* host_clock_ns() at the end of the last tick; 0: none */

static void skip_pause_gap(void)
{
    int64_t now = host_clock_ns();
    if (last_tick_ns != 0 && now - last_tick_ns > PAUSE_GAP_NS)
        host_clock_set(last_tick_ns + (int64_t)MAX_SLEEP_TIME * 1000000);
}

void virt_machine_run(void *opaque)
{
    VirtMachine *m = opaque;
    int delay, i;
    FBDevice *fb_dev;

    run_scheduled = FALSE;
    /* Stop: end the reschedule chain for good. Pause: end it until vm_resume
     * re-arms it. Either way, do not run a tick or reschedule. */
    if (global_stopped || global_paused)
        return;
    skip_pause_gap();

    if (m->console_dev && virtio_console_can_write_data(m->console_dev)) {
        uint8_t buf[128];
        int ret, len;
        len = virtio_console_get_write_len(m->console_dev);
        len = min_int(len, sizeof(buf));
        ret = m->console->read_data(m->console->opaque, buf, len);
        if (ret > 0)
            virtio_console_write_data(m->console_dev, buf, ret);
        if (console_resize_pending) {
            int w, h;
            console_get_size(&w, &h);
            virtio_console_resize_event(m->console_dev, w, h);
            console_resize_pending = FALSE;
        }
    }

    fb_dev = m->fb_dev;
    if (fb_dev) {
        /* refresh the display */
        fb_dev->refresh(fb_dev, fb_refresh1, NULL);
    }
    
    i = 0;
    for(;;) {
        /* wait for an event: the only asynchronous event is the RTC timer */
        delay = virt_machine_get_sleep_duration(m, MAX_SLEEP_TIME);
        if (delay != 0 || i >= MAX_EXEC_TOTAL_CYCLE / MAX_EXEC_CYCLE)
            break;
        virt_machine_interp(m, MAX_EXEC_CYCLE);
        i++;
    }
    
#ifdef CONFIG_X86EMU
    /* blocks that became hot this tick: compile them in the background */
    x86_jit_flush();
#endif
    last_tick_ns = host_clock_ns();
    schedule_run(m, delay == 0 ? 0 : MAX_SLEEP_TIME);
}

