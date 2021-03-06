/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/sendfile.h>

#include "util.h"
#include "btrfs-util.h"
#include "copy.h"

#define COPY_BUFFER_SIZE (16*1024)

int copy_bytes(int fdf, int fdt, off_t max_bytes, bool try_reflink) {
        bool try_sendfile = true;
        int r;

        assert(fdf >= 0);
        assert(fdt >= 0);

        /* Try btrfs reflinks first. */
        if (try_reflink && max_bytes == (off_t) -1) {
                r = btrfs_reflink(fdf, fdt);
                if (r >= 0)
                        return r;
        }

        for (;;) {
                size_t m = COPY_BUFFER_SIZE;
                ssize_t n;

                if (max_bytes != (off_t) -1) {

                        if (max_bytes <= 0)
                                return -EFBIG;

                        if ((off_t) m > max_bytes)
                                m = (size_t) max_bytes;
                }

                /* First try sendfile(), unless we already tried */
                if (try_sendfile) {

                        n = sendfile(fdt, fdf, NULL, m);
                        if (n < 0) {
                                if (errno != EINVAL && errno != ENOSYS)
                                        return -errno;

                                try_sendfile = false;
                                /* use fallback below */
                        } else if (n == 0) /* EOF */
                                break;
                        else if (n > 0)
                                /* Succcess! */
                                goto next;
                }

                /* As a fallback just copy bits by hand */
                {
                        char buf[m];

                        n = read(fdf, buf, m);
                        if (n < 0)
                                return -errno;
                        if (n == 0) /* EOF */
                                break;

                        r = loop_write(fdt, buf, (size_t) n, false);
                        if (r < 0)
                                return r;
                }

        next:
                if (max_bytes != (off_t) -1) {
                        assert(max_bytes >= n);
                        max_bytes -= n;
                }
        }

        return 0;
}

static int fd_copy_symlink(int df, const char *from, const struct stat *st, int dt, const char *to) {
        _cleanup_free_ char *target = NULL;
        int r;

        assert(from);
        assert(st);
        assert(to);

        r = readlinkat_malloc(df, from, &target);
        if (r < 0)
                return r;

        if (symlinkat(target, dt, to) < 0)
                return -errno;

        if (fchownat(dt, to, st->st_uid, st->st_gid, AT_SYMLINK_NOFOLLOW) < 0)
                return -errno;

        return 0;
}

static int fd_copy_regular(int df, const char *from, const struct stat *st, int dt, const char *to) {
        _cleanup_close_ int fdf = -1, fdt = -1;
        int r, q;

        assert(from);
        assert(st);
        assert(to);

        fdf = openat(df, from, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
        if (fdf < 0)
                return -errno;

        fdt = openat(dt, to, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW, st->st_mode & 07777);
        if (fdt < 0)
                return -errno;

        r = copy_bytes(fdf, fdt, (off_t) -1, true);
        if (r < 0) {
                unlinkat(dt, to, 0);
                return r;
        }

        if (fchown(fdt, st->st_uid, st->st_gid) < 0)
                r = -errno;

        if (fchmod(fdt, st->st_mode & 07777) < 0)
                r = -errno;

        q = close(fdt);
        fdt = -1;

        if (q < 0) {
                r = -errno;
                unlinkat(dt, to, 0);
        }

        return r;
}

static int fd_copy_fifo(int df, const char *from, const struct stat *st, int dt, const char *to) {
        int r;

        assert(from);
        assert(st);
        assert(to);

        r = mkfifoat(dt, to, st->st_mode & 07777);
        if (r < 0)
                return -errno;

        if (fchownat(dt, to, st->st_uid, st->st_gid, AT_SYMLINK_NOFOLLOW) < 0)
                r = -errno;

        if (fchmodat(dt, to, st->st_mode & 07777, 0) < 0)
                r = -errno;

        return r;
}

static int fd_copy_node(int df, const char *from, const struct stat *st, int dt, const char *to) {
        int r;

        assert(from);
        assert(st);
        assert(to);

        r = mknodat(dt, to, st->st_mode, st->st_rdev);
        if (r < 0)
                return -errno;

        if (fchownat(dt, to, st->st_uid, st->st_gid, AT_SYMLINK_NOFOLLOW) < 0)
                r = -errno;

        if (fchmodat(dt, to, st->st_mode & 07777, 0) < 0)
                r = -errno;

        return r;
}

static int fd_copy_directory(
                int df,
                const char *from,
                const struct stat *st,
                int dt,
                const char *to,
                dev_t original_device,
                bool merge) {

        _cleanup_close_ int fdf = -1, fdt = -1;
        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;
        bool created;
        int r;

        assert(st);
        assert(to);

        if (from)
                fdf = openat(df, from, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
        else
                fdf = fcntl(df, F_DUPFD_CLOEXEC, 3);

        d = fdopendir(fdf);
        if (!d)
                return -errno;
        fdf = -1;

        r = mkdirat(dt, to, st->st_mode & 07777);
        if (r >= 0)
                created = true;
        else if (errno == EEXIST && merge)
                created = false;
        else
                return -errno;

        fdt = openat(dt, to, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
        if (fdt < 0)
                return -errno;

        r = 0;

        if (created) {
                if (fchown(fdt, st->st_uid, st->st_gid) < 0)
                        r = -errno;

                if (fchmod(fdt, st->st_mode & 07777) < 0)
                        r = -errno;
        }

        FOREACH_DIRENT(de, d, return -errno) {
                struct stat buf;
                int q;

                if (fstatat(dirfd(d), de->d_name, &buf, AT_SYMLINK_NOFOLLOW) < 0) {
                        r = -errno;
                        continue;
                }

                if (buf.st_dev != original_device)
                        continue;

                if (S_ISREG(buf.st_mode))
                        q = fd_copy_regular(dirfd(d), de->d_name, &buf, fdt, de->d_name);
                else if (S_ISDIR(buf.st_mode))
                        q = fd_copy_directory(dirfd(d), de->d_name, &buf, fdt, de->d_name, original_device, merge);
                else if (S_ISLNK(buf.st_mode))
                        q = fd_copy_symlink(dirfd(d), de->d_name, &buf, fdt, de->d_name);
                else if (S_ISFIFO(buf.st_mode))
                        q = fd_copy_fifo(dirfd(d), de->d_name, &buf, fdt, de->d_name);
                else if (S_ISBLK(buf.st_mode) || S_ISCHR(buf.st_mode))
                        q = fd_copy_node(dirfd(d), de->d_name, &buf, fdt, de->d_name);
                else
                        q = -ENOTSUP;

                if (q == -EEXIST && merge)
                        q = 0;

                if (q < 0)
                        r = q;
        }

        return r;
}

int copy_tree_at(int fdf, const char *from, int fdt, const char *to, bool merge) {
        struct stat st;

        assert(from);
        assert(to);

        if (fstatat(fdf, from, &st, AT_SYMLINK_NOFOLLOW) < 0)
                return -errno;

        if (S_ISREG(st.st_mode))
                return fd_copy_regular(fdf, from, &st, fdt, to);
        else if (S_ISDIR(st.st_mode))
                return fd_copy_directory(fdf, from, &st, fdt, to, st.st_dev, merge);
        else if (S_ISLNK(st.st_mode))
                return fd_copy_symlink(fdf, from, &st, fdt, to);
        else if (S_ISFIFO(st.st_mode))
                return fd_copy_fifo(fdf, from, &st, fdt, to);
        else if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))
                return fd_copy_node(fdf, from, &st, fdt, to);
        else
                return -ENOTSUP;
}

int copy_tree(const char *from, const char *to, bool merge) {
        return copy_tree_at(AT_FDCWD, from, AT_FDCWD, to, merge);
}

int copy_directory_fd(int dirfd, const char *to, bool merge) {

        struct stat st;

        assert(dirfd >= 0);
        assert(to);

        if (fstat(dirfd, &st) < 0)
                return -errno;

        if (!S_ISDIR(st.st_mode))
                return -ENOTDIR;

        return fd_copy_directory(dirfd, NULL, &st, AT_FDCWD, to, st.st_dev, merge);
}

int copy_file_fd(const char *from, int fdt, bool try_reflink) {
        _cleanup_close_ int fdf = -1;

        assert(from);
        assert(fdt >= 0);

        fdf = open(from, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fdf < 0)
                return -errno;

        return copy_bytes(fdf, fdt, (off_t) -1, try_reflink);
}

int copy_file(const char *from, const char *to, int flags, mode_t mode) {
        int fdt, r;

        assert(from);
        assert(to);

        fdt = open(to, flags|O_WRONLY|O_CREAT|O_CLOEXEC|O_NOCTTY, mode);
        if (fdt < 0)
                return -errno;

        r = copy_file_fd(from, fdt, true);
        if (r < 0) {
                close(fdt);
                unlink(to);
                return r;
        }

        if (close(fdt) < 0) {
                unlink_noerrno(to);
                return -errno;
        }

        return 0;
}
