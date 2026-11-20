/*
 * JS-backed virtio-block device (persistent disks for the web runtime)
 *
 * Implements struct BlockDevice (virtio.h) on top of a JavaScript sector store
 * (Module.api.blk, backed by IndexedDB / OPFS — see web/js/lib.js and the
 * volume backends). Unlike the 9p volume, block I/O keeps the virtio async
 * contract: read_async/write_async return 1 ("pending") and js_block_complete()
 * is invoked from JS when the backend promise resolves. No Asyncify needed, and
 * virtio serializes to one in-flight request at a time (virtio.c), so a single
 * pending (cb, opaque) slot suffices — same shape as block_net.c.
 *
 * Cfg:   drive0: { file: "js:<name>" }
 * Guest: mkfs.ext4 /dev/vdX && mount /dev/vdX /mnt/<name>   (first use)
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <emscripten.h>

#include "cutils.h"
#include "virtio.h"

#define SECTOR_SIZE 512

/* --- imports implemented in web/js/lib.js -------------------------------- */
extern int    js_block_mount(const char *name);       /* -> vol handle or -1 */
extern double js_block_sectors(int vol);              /* sector count (sync) */
extern void   js_block_read(void *dev, int vol, double sector,
                            uint8_t *buf, int nsectors);
extern void   js_block_write(void *dev, int vol, double sector,
                             const uint8_t *buf, int nsectors);

typedef struct {
    BlockDevice *bs;
    int vol;
    int64_t sector_count;
    /* one in-flight request (virtio serializes), like block_net.c */
    BlockDeviceCompletionFunc *cb;
    void *opaque;
} BlockDeviceJS;

static int64_t bjs_get_sector_count(BlockDevice *bs)
{
    BlockDeviceJS *bf = bs->opaque;
    return bf->sector_count;
}

static int bjs_read_async(BlockDevice *bs, uint64_t sector_num, uint8_t *buf,
                          int n, BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceJS *bf = bs->opaque;
    if (n <= 0) {
        if (cb) cb(opaque, 0);
        return 0;
    }
    bf->cb = cb;
    bf->opaque = opaque;
    js_block_read(bf, bf->vol, (double)sector_num, buf, n);
    return 1;   /* always async: js_block_complete() fires later */
}

static int bjs_write_async(BlockDevice *bs, uint64_t sector_num,
                           const uint8_t *buf, int n,
                           BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceJS *bf = bs->opaque;
    if (n <= 0) {
        if (cb) cb(opaque, 0);
        return 0;
    }
    bf->cb = cb;
    bf->opaque = opaque;
    js_block_write(bf, bf->vol, (double)sector_num, buf, n);
    return 1;
}

/* called from JS when a read/write promise resolves (ret: 0 ok, <0 -errno) */
EMSCRIPTEN_KEEPALIVE
void js_block_complete(void *dev, int ret)
{
    BlockDeviceJS *bf = dev;
    BlockDeviceCompletionFunc *cb = bf->cb;
    void *opaque = bf->opaque;
    bf->cb = NULL;
    bf->opaque = NULL;
    if (cb)
        cb(opaque, ret);
}

BlockDevice *block_device_init_js(const char *name)
{
    BlockDevice *bs;
    BlockDeviceJS *bf;
    int vol;
    double sectors;

    vol = js_block_mount(name);
    if (vol < 0)
        return NULL;
    sectors = js_block_sectors(vol);
    if (sectors <= 0)
        return NULL;

    bs = mallocz(sizeof(*bs));
    bf = mallocz(sizeof(*bf));
    bf->bs = bs;
    bf->vol = vol;
    bf->sector_count = (int64_t)sectors;

    bs->opaque = bf;
    bs->get_sector_count = bjs_get_sector_count;
    bs->read_async = bjs_read_async;
    bs->write_async = bjs_write_async;
    return bs;
}
