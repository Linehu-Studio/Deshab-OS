/*
 * Static mount(2) helper. Guest vdb util-linux/mount is unreadable
 * (runtime ext4 checksum) and persist ld.so hits SHSTK (fault at 0).
 * Built the same way as utsm_exec_daemon, which already runs in-guest.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>

static void wstr(const char *s)
{
    (void)write(2, s, strlen(s));
}

static int do_mount(const char *src, const char *tgt,
                    const char *fstype, unsigned long flags, const void *data)
{
    if (mount(src, tgt, fstype, flags, data) == 0)
        return 0;
    wstr("utsm_bindmount errno=");
    if (errno >= 10) {
        char d[2];
        d[0] = (char)('0' + (errno / 10));
        d[1] = (char)('0' + (errno % 10));
        (void)write(2, d, 2);
    } else {
        char d = (char)('0' + errno);
        (void)write(2, &d, 1);
    }
    wstr("\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        wstr("usage: utsm_bindmount bind|rbind|slave|fstype|msleep|yield|setinterp|umount|nsexec ...\n");
        return 2;
    }
    if (strcmp(argv[1], "bind") == 0 && argc >= 4)
        return do_mount(argv[2], argv[3], NULL, MS_BIND, NULL);
    if (strcmp(argv[1], "rbind") == 0 && argc >= 4)
        return do_mount(argv[2], argv[3], NULL, MS_BIND | MS_REC, NULL);
    if (strcmp(argv[1], "slave") == 0 && argc >= 3)
        return do_mount(NULL, argv[2], NULL, MS_SLAVE, NULL);
    if (strcmp(argv[1], "fstype") == 0 && argc >= 5)
        return do_mount(argv[3], argv[4], argv[2], 0,
                        (argc >= 6) ? argv[5] : NULL);
    if (strcmp(argv[1], "yield") == 0) {
        sched_yield();
        return 0;
    }
    if (strcmp(argv[1], "tail") == 0 && argc >= 3) {
        int fd, maxlines = 40, lines = 0, i;
        off_t sz, start;
        static char buf[16384];
        ssize_t n;
        if (argc >= 4) {
            const char *p = argv[3];
            maxlines = 0;
            while (*p >= '0' && *p <= '9') {
                maxlines = maxlines * 10 + (*p - '0');
                p++;
            }
            if (maxlines < 1)
                maxlines = 40;
        }
        fd = open(argv[2], O_RDONLY);
        if (fd < 0)
            return 1;
        sz = lseek(fd, 0, SEEK_END);
        if (sz < 0)
            sz = 0;
        start = (sz > (off_t)sizeof(buf)) ? sz - (off_t)sizeof(buf) : 0;
        if (lseek(fd, start, SEEK_SET) < 0)
            start = 0;
        n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n <= 0)
            return 1;
        for (i = (int)n - 1; i >= 0; i--) {
            if (buf[i] == '\n')
                lines++;
            if (lines > maxlines) {
                i++;
                break;
            }
        }
        if (i < 0)
            i = 0;
        (void)write(1, buf + i, (size_t)(n - i));
        return 0;
    }
    if (strcmp(argv[1], "msleep") == 0 && argc >= 3) {
        unsigned long ms = 0;
        const char *p = argv[2];
        struct timespec ts;
        while (*p >= '0' && *p <= '9') {
            ms = ms * 10ul + (unsigned long)(*p - '0');
            p++;
        }
        ts.tv_sec = (time_t)(ms / 1000ul);
        ts.tv_nsec = (long)(ms % 1000ul) * 1000000L;
        (void)nanosleep(&ts, 0);
        return 0;
    }
    if (strcmp(argv[1], "chroot") == 0 && argc >= 4) {
        int kfd;
        if (chroot(argv[2]) != 0) {
            wstr("utsm_bindmount: chroot failed\n");
            return 1;
        }
        if (chdir("/") != 0) {
            wstr("utsm_bindmount: chdir / failed\n");
            return 1;
        }
        kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kfd >= 0) {
            const char *m = "[utsm_bindmount] inside chroot, exec next\n";
            (void)write(kfd, m, strlen(m));
            close(kfd);
        }
        execv(argv[3], argv + 3);
        wstr("utsm_bindmount: exec after chroot failed path=");
        wstr(argv[3]);
        wstr(" errno=");
        if (errno >= 10) {
            char d[2];
            d[0] = (char)('0' + (errno / 10));
            d[1] = (char)('0' + (errno % 10));
            (void)write(2, d, 2);
        } else {
            char d = (char)('0' + errno);
            (void)write(2, &d, 1);
        }
        wstr("\n");
        return 127;
    }
    if (strcmp(argv[1], "nsexec") == 0 && argc >= 3) {
        int kfd;
        int u1, u2, u3, pr;
        kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (unshare(CLONE_NEWNS) != 0) {
            wstr("nsexec: unshare failed\n");
            if (kfd >= 0) {
                const char *m = "[nsexec] unshare failed\n";
                (void)write(kfd, m, strlen(m));
                close(kfd);
            }
            return 1;
        }
        pr = mount((const char *)0, "/", (const char *)0,
                   MS_REC | MS_PRIVATE, (const void *)0);
        if (pr != 0) {
            pr = mount((const char *)0, "/", (const char *)0,
                       MS_REC | MS_SLAVE, (const void *)0);
        }
        /* BUG-XORG-SEGV 根修（2026-09-20）：此处 umount 会把 chroot 里的
         * vdb ld.so bind 拆掉，Xorg 落回 persist ld.so —— 其 0x15ded 处
         * NULL+8 segfault（Xorg/dbus 均复现）。12:18 好运行中这些 bind
         * 全部保持激活（shell umount 均 EBUSY），Xorg 用 vdb ld.so 存活。
         * 恢复该行为：nsexec 不再卸载 ld.so bind。 */
        u1 = -1;
        u2 = -1;
        u3 = -1;
        if (kfd >= 0) {
            const char *m = "[nsexec] umount done, exec Xorg\n";
            (void)write(kfd, m, strlen(m));
            if (u1 == 0)
                m = "[nsexec] umount lib64 ld.so ok\n";
            else
                m = "[nsexec] umount lib64 ld.so FAIL\n";
            (void)write(kfd, m, strlen(m));
            if (u2 == 0)
                m = "[nsexec] umount usr/lib ld.so ok\n";
            else
                m = "[nsexec] umount usr/lib ld.so FAIL\n";
            (void)write(kfd, m, strlen(m));
            (void)pr;
            (void)u3;
            close(kfd);
        }
        execv(argv[2], argv + 2);
        wstr("nsexec: exec failed\n");
        return 127;
    }
    if (strcmp(argv[1], "umount") == 0 && argc >= 3) {
        if (umount(argv[2]) == 0)
            return 0;
        wstr("umount errno=");
        if (errno >= 10) {
            char d[2];
            d[0] = (char)('0' + (errno / 10));
            d[1] = (char)('0' + (errno % 10));
            (void)write(2, d, 2);
        } else {
            char d = (char)('0' + errno);
            (void)write(2, &d, 1);
        }
        wstr("\n");
        return 1;
    }
    if (strcmp(argv[1], "setinterp") == 0 && argc >= 4) {
        int fd;
        unsigned char eh[64];
        unsigned long phoff = 0, nlen;
        unsigned short phentsize = 0, phnum = 0, i;
        const char *ni = argv[3];
        nlen = strlen(ni) + 1;
        fd = open(argv[2], O_RDWR);
        if (fd < 0) {
            wstr("setinterp: open failed\n");
            return 1;
        }
        if (read(fd, eh, 64) != 64 || eh[0] != 0x7f || eh[4] != 2) {
            wstr("setinterp: not elf64\n");
            close(fd);
            return 1;
        }
        memcpy(&phoff, eh + 32, 8);
        memcpy(&phentsize, eh + 54, 2);
        memcpy(&phnum, eh + 56, 2);
        for (i = 0; i < phnum; i++) {
            unsigned char ph[56];
            unsigned int type = 0;
            unsigned long off = 0, filesz = 0;
            if (lseek(fd, (off_t)(phoff + (unsigned long)i * phentsize),
                      SEEK_SET) < 0)
                break;
            if (read(fd, ph, 56) != 56)
                break;
            memcpy(&type, ph, 4);
            if (type != 3)
                continue;
            memcpy(&off, ph + 8, 8);
            memcpy(&filesz, ph + 32, 8);
            if (nlen > filesz) {
                wstr("setinterp: path too long\n");
                close(fd);
                return 1;
            }
            if (lseek(fd, (off_t)off, SEEK_SET) < 0) {
                close(fd);
                return 1;
            }
            if (write(fd, ni, nlen) != (ssize_t)nlen) {
                close(fd);
                return 1;
            }
            if (filesz > nlen) {
                unsigned char z[32];
                unsigned long pad = filesz - nlen;
                memset(z, 0, sizeof z);
                while (pad) {
                    unsigned long c = pad > sizeof z ? sizeof z : pad;
                    if (write(fd, z, c) != (ssize_t)c) {
                        close(fd);
                        return 1;
                    }
                    pad -= c;
                }
            }
            close(fd);
            wstr("setinterp: ok\n");
            return 0;
        }
        wstr("setinterp: no PT_INTERP\n");
        close(fd);
        return 1;
    }
    wstr("utsm_bindmount: bad args\n");
    return 2;
}
