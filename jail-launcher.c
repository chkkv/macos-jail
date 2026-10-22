#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/mount.h>

/*
 * macOS 27 chroot launcher.
 *
 *  - /dev is a devfs mount inside the jail (needed for /dev/fd).
 *  - The real, SIP-protected dyld shared cache on the Preboot volume is
 *    opened before chroot(); the fds are kept across exec and exposed as
 *    <jail>/System/Library/dyld/<name> -> /dev/fd/<n>.
 *  - After chroot, /etc/jail-user is read and the process drops to that
 *    user (setgroups/setgid/setuid) before executing bash or the command.
 *
 * JAIL_ROOT must be set by the wrapper script.
 */
#define CACHE_FD_BASE 200

static int namecmp(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int main(int argc, char **argv) {
    const char *jail = getenv("JAIL_ROOT");
    if (jail == NULL || *jail == '\0') {
        fprintf(stderr, "JAIL_ROOT is not set\n");
        return 1;
    }

    char jd[PATH_MAX], jdev[PATH_MAX];
    snprintf(jd, sizeof(jd), "%s/System/Library/dyld", jail);
    snprintf(jdev, sizeof(jdev), "%s/dev", jail);

    mkdir(jdev, 0755);
    struct statfs sfs;
    if (statfs(jdev, &sfs) != 0 || strcmp(sfs.f_fstypename, "devfs") != 0) {
        if (mount("devfs", jdev, 0, NULL) != 0) {
            perror("mount devfs");
            return 1;
        }
    }

    /* Open the real SIP-protected cache at a fixed fd range so that both
       this process and jail-authd's children can use the same
       /System/Library/dyld -> /dev/fd/<n> links. */
    const char *real = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld";
    struct dirent **names = NULL;
    int nfiles = scandir(real, &names, NULL, namecmp);
    if (nfiles < 0) {
        perror("scandir cache");
        return 1;
    }
    int slot = 0;
    for (int i = 0; i < nfiles; i++) {
        if (names[i]->d_name[0] == '.') {
            free(names[i]);
            continue;
        }
        if (CACHE_FD_BASE + slot >= 284) {
            fprintf(stderr, "too many cache files\n");
            free(names[i]);
            break;
        }
        char rp[PATH_MAX], lp[PATH_MAX], tgt[64];
        snprintf(rp, sizeof(rp), "%s/%s", real, names[i]->d_name);
        int fd = open(rp, O_RDONLY);
        if (fd < 0) {
            perror(rp);
            free(names[i]);
            continue;
        }
        int target = CACHE_FD_BASE + slot;
        if (dup2(fd, target) < 0) {
            perror("dup2 cache fd");
            close(fd);
            free(names[i]);
            continue;
        }
        if (fd != target)
            close(fd);
        fcntl(target, F_SETFD, 0);      /* keep across execve */
        snprintf(lp, sizeof(lp), "%s/%s", jd, names[i]->d_name);
        unlink(lp);
        snprintf(tgt, sizeof(tgt), "/dev/fd/%d", target);
        if (symlink(tgt, lp) != 0 && errno != EEXIST)
            perror(lp);
        free(names[i]);
        slot++;
    }
    free(names);

    if (chdir(jail) != 0 || chroot(jail) != 0 || chdir("/") != 0) {
        perror("chroot");
        return 1;
    }

    /* Drop privileges if /etc/jail-user exists:
       <name> <uid> <gid> <gid,gid,...>   (groups may be "-") */
    char user[64] = {0}, groupbuf[1024] = {0};
    int uid = 0, gid = 0, n = 0;
    FILE *f = fopen("/etc/jail-user", "r");
    if (f != NULL) {
        n = fscanf(f, "%63s %d %d %1023s", user, &uid, &gid, groupbuf);
        fclose(f);
    }

    int have_user = (n >= 3 && uid != 0);
    char home[128];
    if (have_user)
        snprintf(home, sizeof(home), "/Users/%s", user);
    else
        snprintf(home, sizeof(home), "/var/root");

    /* Initial cwd for the jail entry is the user's home. This is done here
       (not in ~/.profile) so a later `bash -l` run from another directory
       keeps its own cwd. */
    if (chdir(home) != 0)
        chdir("/");

    if (have_user) {
        gid_t groups[64];
        int ng = 0;
        if (n >= 4 && strcmp(groupbuf, "-") != 0) {
            char *p = strtok(groupbuf, ",");
            while (p != NULL && ng < 64) {
                groups[ng++] = (gid_t)atoi(p);
                p = strtok(NULL, ",");
            }
        }
        if (ng > 0)
            (void)setgroups(ng, groups);
        if (setgid(gid) != 0) {
            perror("setgid");
            return 1;
        }
        if (setuid(uid) != 0) {
            perror("setuid");
            return 1;
        }
        setenv("HOME", home, 1);
        setenv("USER", user, 1);
        setenv("LOGNAME", user, 1);
    } else {
        setenv("HOME", "/var/root", 1);
    }

    setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);

    if (argc < 2) {
        char *args[] = { "/bin/bash", "-l", NULL };
        execv(args[0], args);
    } else {
        execv(argv[1], &argv[1]);
    }
    perror("execv");
    return 1;
}
