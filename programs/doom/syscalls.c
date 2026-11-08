/*
 * newlib's system calls for the DOOM port
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../z6ii/z6ii.h"
#include "doom_z6ii.h"

struct _reent;
static ID malloc_mtx;

void z6ii_malloc_lock_init(void)
{
    malloc_mtx = z6ii_mutex_create();
}

void __malloc_lock(struct _reent *r)
{
    (void)r;
    if (malloc_mtx > 0) {
        tk_loc_mtx(malloc_mtx, TMO_FEVR);
    }
}

void __malloc_unlock(struct _reent *r)
{
    (void)r;
    if (malloc_mtx > 0) {
        tk_unl_mtx(malloc_mtx);
    }
}

extern char __heap_start[], __heap_end[];
static char *heap_top = __heap_start;

void *_sbrk(ptrdiff_t incr)
{
    if (heap_top + incr > __heap_end || heap_top + incr < __heap_start) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *old = heap_top;
    heap_top += incr;
    return old;
}

static void log_bytes(const char *p, int n)
{
    volatile uint32_t *idx = (volatile uint32_t *)DOOM_LOG;
    volatile char *ring = (volatile char *)(DOOM_LOG + 4);
    for (int i = 0; i < n; i++) {
        ring[*idx % (DOOM_LOG_SIZE - 4)] = p[i];
        (*idx)++;
    }
}

#define WAD_FD      3
#define SCRATCH_FD  4
#define SCRATCH_MAX 0x20000u
static uint32_t wad_pos;
static int wad_open;
static char scratch[SCRATCH_MAX];
static uint32_t scratch_len, scratch_pos;
static int scratch_open, scratch_exists;

static int is_scratch(const char *path)
{
    const size_t n = strlen(path);
    return n >= 4 && strcmp(path + n - 4, ".mid") == 0;
}

static int is_wad(const char *path)
{
    static const char want[] = "doom1.wad";
    const size_t n = strlen(path), w = sizeof(want) - 1;
    if (n < w) {
        return 0;
    }
    for (size_t i = 0; i < w; i++) {
        char c = path[n - w + i];
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c != want[i]) {
            return 0;
        }
    }
    return n == w || path[n - w - 1] == '/' || path[n - w - 1] == '\\';
}

int _open(const char *path, int flags, int mode)
{
    (void)mode;
    if (is_wad(path) && (flags & O_ACCMODE) == O_RDONLY && doom_wad_size) {
        wad_pos = 0;
        wad_open = 1;
        return WAD_FD;
    }
    if (is_scratch(path) && !scratch_open && ((flags & O_ACCMODE) != O_RDONLY || scratch_exists)) {
        if ((flags & O_ACCMODE) != O_RDONLY && (flags & O_TRUNC)) {
            scratch_len = 0;
        }
        scratch_exists = 1;
        scratch_pos = (flags & O_APPEND) ? scratch_len : 0;
        scratch_open = 1;
        return SCRATCH_FD;
    }
    errno = ENOENT;
    return -1;
}

int _read(int fd, char *buf, int len)
{
    if (fd == SCRATCH_FD && scratch_open) {
        uint32_t n = scratch_len - scratch_pos;
        if ((uint32_t)len < n) {
            n = (uint32_t)len;
        }
        memcpy(buf, scratch + scratch_pos, n);
        scratch_pos += n;
        return (int)n;
    }
    if (fd != WAD_FD || !wad_open) {
        errno = EBADF;
        return -1;
    }
    uint32_t n = doom_wad_size - wad_pos;
    if ((uint32_t)len < n) {
        n = (uint32_t)len;
    }
    memcpy(buf, (const char *)DOOM_WAD + wad_pos, n);
    wad_pos += n;
    return (int)n;
}

off_t _lseek(int fd, off_t off, int whence)
{
    if (fd == SCRATCH_FD && scratch_open) {
        const off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)scratch_pos : (off_t)scratch_len;
        if (base + off < 0 || base + off > (off_t)scratch_len) {
            errno = EINVAL;
            return -1;
        }
        scratch_pos = (uint32_t)(base + off);
        return (off_t)scratch_pos;
    }
    if (fd != WAD_FD || !wad_open) {
        errno = EBADF;
        return -1;
    }
    const off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)wad_pos : (off_t)doom_wad_size;
    if (base + off < 0 || base + off > (off_t)doom_wad_size) {
        errno = EINVAL;
        return -1;
    }
    wad_pos = (uint32_t)(base + off);
    return (off_t)wad_pos;
}

int _close(int fd)
{
    if (fd == WAD_FD) {
        wad_open = 0;
    }
    if (fd == SCRATCH_FD) {
        scratch_open = 0;
    }
    return 0;
}

int _write(int fd, const char *buf, int len)
{
    if (fd == 1 || fd == 2) {
        log_bytes(buf, len);
        return len;
    }
    if (fd == SCRATCH_FD && scratch_open) {
        if (scratch_pos + (uint32_t)len > SCRATCH_MAX) {
            errno = ENOSPC;
            return -1;
        }
        memcpy(scratch + scratch_pos, buf, (size_t)len);
        scratch_pos += (uint32_t)len;
        if (scratch_pos > scratch_len) {
            scratch_len = scratch_pos;
        }
        return len;
    }
    errno = EBADF;
    return -1;
}

int _fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = fd == WAD_FD || fd == SCRATCH_FD ? S_IFREG : S_IFCHR;
    st->st_size = fd == WAD_FD ? (off_t)doom_wad_size : fd == SCRATCH_FD ? (off_t)scratch_len : 0;
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    if (is_wad(path) && doom_wad_size) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG;
        st->st_size = (off_t)doom_wad_size;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

int _isatty(int fd)
{
    return fd <= 2;
}

int mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = EROFS;
    return -1;
}

int _link(const char *old, const char *new)
{
    (void)old;
    (void)new;
    errno = EROFS;
    return -1;
}

int _unlink(const char *path)
{
    if (is_scratch(path) && scratch_exists && !scratch_open) {
        scratch_exists = 0;
        scratch_len = 0;
        return 0;
    }
    errno = EROFS;
    return -1;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

void _exit(int status)
{
    doom_quit(status);
}
