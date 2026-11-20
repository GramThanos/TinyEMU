/*
 * JS-backed 9p filesystem (docker-like volumes for the web runtime)
 *
 * Implements struct FSDevice (fs.h) on top of a JavaScript in-memory filesystem
 * that lib.js maintains (Module.api.fs9p). The JS tree is loaded from a
 * persistent storage backend (IndexedDB / OPFS / File System Access) BEFORE the
 * VM boots and flushed back at boundaries, so every op here can call into JS
 * synchronously — no Asyncify needed, and it works both on the main thread and
 * in a worker. This mirrors fs_disk.c structurally (path-based FSFile), with the
 * POSIX calls replaced by js_fs_* imports.
 *
 * Cfg:   fs1: { file: "js:<name>", tag: "<mount tag>" }
 * Guest: mount -t 9p <tag> /mnt/<name>  (9p2000.L, trans=virtio)
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "list.h"
#include "fs.h"

/* --- imports implemented in web/js/lib.js (all synchronous) --------------- */
extern int js_fs_mount(const char *name);
extern int js_fs_stat(int vol, const char *path,
                      uint32_t *pmode, uint32_t *pino,
                      double *psize, double *pmtime);
extern int js_fs_readdir(int vol, const char *path, int index,
                         char *name, int name_size,
                         uint32_t *pmode, uint32_t *pino);
extern int js_fs_read(int vol, const char *path, double offset,
                      uint8_t *buf, int count);
extern int js_fs_write(int vol, const char *path, double offset,
                       const uint8_t *buf, int count);
extern int js_fs_create(int vol, const char *path, uint32_t mode, int is_dir);
extern int js_fs_truncate(int vol, const char *path, double size);
extern int js_fs_chmod(int vol, const char *path, uint32_t mode);
extern int js_fs_utime(int vol, const char *path, double mtime_sec);
extern int js_fs_unlink(int vol, const char *path);
extern int js_fs_rename(int vol, const char *from, const char *to);

typedef struct {
    FSDevice common;
    int vol;          /* handle into Module.api.fs9p */
    char *name;
} FSDeviceJS;

static void fs_close(FSDevice *fs, FSFile *f);

struct FSFile {
    uint32_t uid;
    char *path;       /* absolute within the volume, e.g. "/" or "/a/b" */
    BOOL is_opened;
    BOOL is_dir;
};

static void fs_delete(FSDevice *fs, FSFile *f)
{
    if (f->is_opened)
        fs_close(fs, f);
    free(f->path);
    free(f);
}

/* warning: path belongs to fid_create() */
static FSFile *fid_create(FSDevice *s1, char *path, uint32_t uid)
{
    FSFile *f;
    f = mallocz(sizeof(*f));
    f->path = path;
    f->uid = uid;
    return f;
}

static char *compose_path(const char *path, const char *name)
{
    int path_len, name_len;
    char *d;

    path_len = strlen(path);
    name_len = strlen(name);
    if (path_len == 1 && path[0] == '/') {
        /* root: avoid the leading "//" */
        d = malloc(1 + name_len + 1);
        d[0] = '/';
        memcpy(d + 1, name, name_len + 1);
    } else {
        d = malloc(path_len + 1 + name_len + 1);
        memcpy(d, path, path_len);
        d[path_len] = '/';
        memcpy(d + path_len + 1, name, name_len + 1);
    }
    return d;
}

static void mode_to_qid(FSQID *qid, uint32_t mode, uint32_t ino)
{
    switch (mode & P9_S_IFMT) {
    case P9_S_IFDIR: qid->type = P9_QTDIR; break;
    case P9_S_IFLNK: qid->type = P9_QTSYMLINK; break;
    default:         qid->type = P9_QTFILE; break;
    }
    qid->version = 0; /* no client caching */
    qid->path = ino;
}

static void fs_statfs(FSDevice *fs1, FSStatFS *st)
{
    /* Report a large, mostly-free volume; the JS store grows on demand. */
    st->f_bsize = 4096;
    st->f_blocks = 1 << 20;   /* 4 GiB nominal */
    st->f_bfree = 1 << 19;
    st->f_bavail = 1 << 19;
    st->f_files = 1 << 16;
    st->f_ffree = 1 << 15;
}

static int fs_attach(FSDevice *fs1, FSFile **pf,
                     FSQID *qid, uint32_t uid,
                     const char *uname, const char *aname)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    uint32_t mode, ino;
    double size, mtime;
    FSFile *f;

    if (js_fs_stat(fs->vol, "/", &mode, &ino, &size, &mtime) != 0) {
        *pf = NULL;
        return -P9_ENOENT;
    }
    f = fid_create(fs1, strdup("/"), uid);
    mode_to_qid(qid, mode, ino);
    *pf = f;
    return 0;
}

static int fs_walk(FSDevice *fs1, FSFile **pf, FSQID *qids,
                   FSFile *f, int n, char **names)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    char *path, *path1;
    uint32_t mode, ino;
    double size, mtime;
    int i;

    path = strdup(f->path);
    for (i = 0; i < n; i++) {
        path1 = compose_path(path, names[i]);
        if (js_fs_stat(fs->vol, path1, &mode, &ino, &size, &mtime) != 0) {
            free(path1);
            break;
        }
        free(path);
        path = path1;
        mode_to_qid(&qids[i], mode, ino);
    }
    *pf = fid_create(fs1, path, f->uid);
    return i;
}

static int fs_mkdir(FSDevice *fs1, FSQID *qid, FSFile *f,
                    const char *name, uint32_t mode, uint32_t gid)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    char *path;
    uint32_t rmode, ino;
    double size, mtime;
    int ret;

    path = compose_path(f->path, name);
    ret = js_fs_create(fs->vol, path, (mode & P9_S_IRWXUGO) | P9_S_IFDIR, TRUE);
    if (ret != 0) {
        free(path);
        return ret;
    }
    if (js_fs_stat(fs->vol, path, &rmode, &ino, &size, &mtime) != 0) {
        free(path);
        return -P9_EIO;
    }
    free(path);
    mode_to_qid(qid, rmode, ino);
    return 0;
}

static int fs_open(FSDevice *fs1, FSQID *qid, FSFile *f, uint32_t flags,
                   FSOpenCompletionFunc *cb, void *opaque)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    uint32_t mode, ino;
    double size, mtime;

    fs_close(fs1, f);

    if (js_fs_stat(fs->vol, f->path, &mode, &ino, &size, &mtime) != 0)
        return -P9_ENOENT;
    mode_to_qid(qid, mode, ino);

    if (flags & P9_O_TRUNC) {
        if ((mode & P9_S_IFMT) == P9_S_IFREG)
            js_fs_truncate(fs->vol, f->path, 0);
    }
    f->is_opened = TRUE;
    f->is_dir = ((mode & P9_S_IFMT) == P9_S_IFDIR);
    return 0;
}

static int fs_create(FSDevice *fs1, FSQID *qid, FSFile *f, const char *name,
                     uint32_t flags, uint32_t mode, uint32_t gid)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    char *path;
    uint32_t rmode, ino;
    double size, mtime;
    int ret;

    fs_close(fs1, f);

    path = compose_path(f->path, name);
    ret = js_fs_create(fs->vol, path, (mode & P9_S_IRWXUGO) | P9_S_IFREG, FALSE);
    if (ret != 0) {
        free(path);
        return ret;
    }
    if (js_fs_stat(fs->vol, path, &rmode, &ino, &size, &mtime) != 0) {
        free(path);
        return -P9_EIO;
    }
    free(f->path);
    f->path = path;
    f->is_opened = TRUE;
    f->is_dir = FALSE;
    mode_to_qid(qid, rmode, ino);
    return 0;
}

static int fs_readdir(FSDevice *fs1, FSFile *f, uint64_t offset,
                      uint8_t *buf, int count)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    char name[1024];
    uint32_t mode, ino;
    int pos, index, name_len, len, type, d_type;

    if (!f->is_opened || !f->is_dir)
        return -P9_EPROTO;

    index = (int)offset;
    pos = 0;
    for (;;) {
        name_len = js_fs_readdir(fs->vol, f->path, index, name, sizeof(name),
                                 &mode, &ino);
        if (name_len < 0)
            break; /* no more entries */
        len = 13 + 8 + 1 + 2 + name_len;
        if ((pos + len) > count)
            break;
        index++;
        switch (mode & P9_S_IFMT) {
        case P9_S_IFDIR: type = P9_QTDIR;     d_type = 4;  break; /* DT_DIR */
        case P9_S_IFLNK: type = P9_QTSYMLINK; d_type = 10; break; /* DT_LNK */
        default:         type = P9_QTFILE;    d_type = 8;  break; /* DT_REG */
        }
        buf[pos++] = type;
        put_le32(buf + pos, 0); pos += 4;         /* qid.version */
        put_le64(buf + pos, ino); pos += 8;       /* qid.path */
        put_le64(buf + pos, index); pos += 8;     /* next offset */
        buf[pos++] = d_type;
        put_le16(buf + pos, name_len); pos += 2;
        memcpy(buf + pos, name, name_len); pos += name_len;
    }
    return pos;
}

static int fs_read(FSDevice *fs1, FSFile *f, uint64_t offset,
                   uint8_t *buf, int count)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    if (!f->is_opened || f->is_dir)
        return -P9_EPROTO;
    return js_fs_read(fs->vol, f->path, (double)offset, buf, count);
}

static int fs_write(FSDevice *fs1, FSFile *f, uint64_t offset,
                    const uint8_t *buf, int count)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    if (!f->is_opened || f->is_dir)
        return -P9_EPROTO;
    return js_fs_write(fs->vol, f->path, (double)offset, buf, count);
}

static void fs_close(FSDevice *fs1, FSFile *f)
{
    if (!f->is_opened)
        return;
    f->is_opened = FALSE;
}

static int fs_stat(FSDevice *fs1, FSFile *f, FSStat *st)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    uint32_t mode, ino;
    double size, mtime;

    if (js_fs_stat(fs->vol, f->path, &mode, &ino, &size, &mtime) != 0)
        return -P9_ENOENT;
    mode_to_qid(&st->qid, mode, ino);
    st->st_mode = mode;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_nlink = 1;
    st->st_rdev = 0;
    st->st_size = (uint64_t)size;
    st->st_blksize = 4096;
    st->st_blocks = ((uint64_t)size + 511) / 512;
    st->st_atime_sec = (uint64_t)mtime;
    st->st_atime_nsec = 0;
    st->st_mtime_sec = (uint64_t)mtime;
    st->st_mtime_nsec = 0;
    st->st_ctime_sec = (uint64_t)mtime;
    st->st_ctime_nsec = 0;
    return 0;
}

static int fs_setattr(FSDevice *fs1, FSFile *f, uint32_t mask,
                      uint32_t mode, uint32_t uid, uint32_t gid,
                      uint64_t size, uint64_t atime_sec, uint64_t atime_nsec,
                      uint64_t mtime_sec, uint64_t mtime_nsec)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    int ret;

    if (mask & P9_SETATTR_MODE) {
        ret = js_fs_chmod(fs->vol, f->path, mode & P9_S_IRWXUGO);
        if (ret != 0)
            return ret;
    }
    if (mask & P9_SETATTR_SIZE) {
        ret = js_fs_truncate(fs->vol, f->path, (double)size);
        if (ret != 0)
            return ret;
    }
    if (mask & P9_SETATTR_MTIME) {
        double t = (mask & P9_SETATTR_MTIME_SET) ? (double)mtime_sec : 0;
        js_fs_utime(fs->vol, f->path, t);
    }
    /* uid/gid ownership is not modelled by the JS store; ignore silently. */
    return 0;
}

static int fs_unlinkat(FSDevice *fs1, FSFile *f, const char *name)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    char *path;
    int ret;

    path = compose_path(f->path, name);
    ret = js_fs_unlink(fs->vol, path);
    free(path);
    return ret;
}

static int fs_renameat(FSDevice *fs1, FSFile *f, const char *name,
                       FSFile *new_f, const char *new_name)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    char *path, *new_path;
    int ret;

    path = compose_path(f->path, name);
    new_path = compose_path(new_f->path, new_name);
    ret = js_fs_rename(fs->vol, path, new_path);
    free(path);
    free(new_path);
    return ret;
}

/* --- ops the JS store does not model: report unsupported / no-op --------- */

static int fs_link(FSDevice *fs, FSFile *df, FSFile *f, const char *name)
{
    return -P9_ENOTSUP;
}

static int fs_symlink(FSDevice *fs, FSQID *qid, FSFile *f,
                      const char *name, const char *symgt, uint32_t gid)
{
    return -P9_ENOTSUP;
}

static int fs_mknod(FSDevice *fs, FSQID *qid, FSFile *f, const char *name,
                    uint32_t mode, uint32_t major, uint32_t minor, uint32_t gid)
{
    return -P9_ENOTSUP;
}

static int fs_readlink(FSDevice *fs, char *buf, int buf_size, FSFile *f)
{
    return -P9_ENOTSUP;
}

static int fs_lock(FSDevice *fs, FSFile *f, const FSLock *lock)
{
    /* No real locking; advisory locks always succeed. */
    return P9_LOCK_SUCCESS;
}

static int fs_getlock(FSDevice *fs, FSFile *f, FSLock *lock)
{
    lock->type = P9_LOCK_TYPE_UNLCK;
    return 0;
}

static void fs_js_end(FSDevice *fs1)
{
    FSDeviceJS *fs = (FSDeviceJS *)fs1;
    free(fs->name);
}

FSDevice *fs_js_init(const char *name)
{
    FSDeviceJS *fs;
    int vol;

    vol = js_fs_mount(name);
    if (vol < 0)
        return NULL;

    fs = mallocz(sizeof(*fs));
    fs->vol = vol;
    fs->name = strdup(name);

    fs->common.fs_end = fs_js_end;
    fs->common.fs_delete = fs_delete;
    fs->common.fs_statfs = fs_statfs;
    fs->common.fs_attach = fs_attach;
    fs->common.fs_walk = fs_walk;
    fs->common.fs_mkdir = fs_mkdir;
    fs->common.fs_open = fs_open;
    fs->common.fs_create = fs_create;
    fs->common.fs_stat = fs_stat;
    fs->common.fs_setattr = fs_setattr;
    fs->common.fs_close = fs_close;
    fs->common.fs_readdir = fs_readdir;
    fs->common.fs_read = fs_read;
    fs->common.fs_write = fs_write;
    fs->common.fs_link = fs_link;
    fs->common.fs_symlink = fs_symlink;
    fs->common.fs_mknod = fs_mknod;
    fs->common.fs_readlink = fs_readlink;
    fs->common.fs_renameat = fs_renameat;
    fs->common.fs_unlinkat = fs_unlinkat;
    fs->common.fs_lock = fs_lock;
    fs->common.fs_getlock = fs_getlock;

    return (FSDevice *)fs;
}
