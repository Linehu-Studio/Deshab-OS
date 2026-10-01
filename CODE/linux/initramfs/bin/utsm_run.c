/*
 * Shebang trampoline: run a script with persist bash + persist glibc.
 * vdb /usr/bin/bash needs libreadline, and virtio often returns a bad
 * ext4 inode checksum for that file. persist userland is on vdc.
 *
 * Built like utsm_exec_daemon: -static -fcf-protection=none.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static void kmsg(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    (void)write(fd, s, strlen(s));
    close(fd);
}

static int exists(const char *p)
{
    return p && access(p, X_OK) == 0;
}

int main(int argc, char **argv)
{
    static const char *ld_cands[] = {
        "/mnt/persist/usr/lib/ld-linux-x86-64.so.2",
        "/mnt/persist/lib64/ld-linux-x86-64.so.2",
        "/mnt/persist/lib/ld-linux-x86-64.so.2",
        0
    };
    static const char *bash_cands[] = {
        "/mnt/persist/usr/bin/bash",
        "/mnt/persist/bin/bash",
        0
    };
    const char *libpath =
        "/mnt/persist/usr/lib:/mnt/persist/lib64:/mnt/persist/lib";
    const char *path =
        "/mnt/persist/usr/bin:/mnt/persist/bin:/usr/local/bin:/usr/bin:/bin";
    const char *ld = 0;
    const char *bash = 0;
    char *nargv[64];
    int n = 0;
    int i;
    char line[192];

    kmsg("[utsm_run] start\n");
    if (argc < 2) {
        kmsg("[utsm_run] usage: utsm_run script [args]\n");
        return 2;
    }

    /* Already inside persist chroot: /mnt/persist is gone, bash is local. */
    if (access("/etc/deshab-kde-root", F_OK) == 0 ||
        access("/mnt/persist/usr/bin/bash", X_OK) != 0) {
        kmsg("[utsm_run] inside persist, exec /usr/bin/bash\n");
        nargv[n++] = "/usr/bin/bash";
        for (i = 1; i < argc && n < 62; i++)
            nargv[n++] = argv[i];
        nargv[n] = 0;
        execv("/usr/bin/bash", nargv);
        kmsg("[utsm_run] inside bash execv failed\n");
        return 127;
    }

    setenv("PATH", path, 1);
    setenv("LD_LIBRARY_PATH", libpath, 1);
    setenv("GLIBC_TUNABLES", "glibc.cpu.hwcaps=-IBT,-SHSTK", 1);

    for (i = 0; ld_cands[i]; i++) {
        if (exists(ld_cands[i])) {
            ld = ld_cands[i];
            break;
        }
    }
    for (i = 0; bash_cands[i]; i++) {
        if (exists(bash_cands[i])) {
            bash = bash_cands[i];
            break;
        }
    }

    /* persist ld.so (as program or as PT_INTERP) SIGSEGVs at -40 in
     * _dl_find_dso_for_object. Use vdb's RAM-backed interpreter with
     * persist libraries via LD_LIBRARY_PATH. */
    (void)ld;

    if (bash) {
        snprintf(line, sizeof(line), "[utsm_run] persist bash %s\n", bash);
        kmsg(line);
        nargv[n++] = (char *)bash;
        for (i = 1; i < argc && n < 62; i++)
            nargv[n++] = argv[i];
        nargv[n] = 0;
        execve(bash, nargv, environ);
        kmsg("[utsm_run] persist bash execve failed\n");
    }

    kmsg("[utsm_run] fallback /usr/bin/bash\n");
    n = 0;
    nargv[n++] = "/usr/bin/bash";
    for (i = 1; i < argc && n < 62; i++)
        nargv[n++] = argv[i];
    nargv[n] = 0;
    execv("/usr/bin/bash", nargv);
    kmsg("[utsm_run] fallback execv failed\n");
    return 127;
}
