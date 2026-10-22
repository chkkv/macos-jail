#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <dirent.h>
#include <limits.h>
#include <grp.h>
#include <util.h>
#include <CommonCrypto/CommonKeyDerivation.h>

#define CACHE_FD_BASE       200
#define AUTH_MAGIC          0x4a41494cU
#define OP_SUDO             1
#define OP_SU             2
#define REQ_PTY             0x1
#define MAX_PASS            1024
#define MAX_ARGS            64
#define MAX_ARG_LEN         4096
#define MAX_TERM            63
#define PBKDF2_ITERATIONS   210000
#define SALT_LEN            16
#define HASH_LEN            32
#define SOCKET_PATH         "/var/run/jail-auth.sock"

enum { ST_OK = 0, ST_AUTH = 1, ST_NOPASS = 2, ST_ERROR = 3 };

struct rate_state {
    int fails;
    time_t last;
};

static volatile sig_atomic_t g_winch = 0;
static volatile sig_atomic_t g_stop = 0;

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void on_winch(int sig) { (void)sig; g_winch = 1; }
static void on_stop(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* small I/O helpers                                                   */
/* ------------------------------------------------------------------ */

static int read_full(int fd, void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char *)buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, (const char *)buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int send_u32(int fd, uint32_t v) {
    unsigned char b[4] = {
        (unsigned char)(v & 0xff),
        (unsigned char)((v >> 8) & 0xff),
        (unsigned char)((v >> 16) & 0xff),
        (unsigned char)((v >> 24) & 0xff)
    };
    return write_full(fd, b, 4);
}

static int recv_u32(int fd, uint32_t *v) {
    unsigned char b[4];
    if (read_full(fd, b, 4) != 0) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int send_fds(int sock, const int *fds, int n) {
    char byte = 'F';
    struct iovec iov;
    struct msghdr msg;
    char control[CMSG_SPACE(sizeof(int) * 3)];
    struct cmsghdr *cm;

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
    cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(cm), fds, sizeof(int) * n);
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fds(int sock, int *fds, int n) {
    char byte;
    struct iovec iov;
    struct msghdr msg;
    char control[CMSG_SPACE(sizeof(int) * 3)];
    struct cmsghdr *cm;

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    if (recvmsg(sock, &msg, 0) <= 0) return -1;
    cm = CMSG_FIRSTHDR(&msg);
    if (cm == NULL || cm->cmsg_level != SOL_SOCKET ||
        cm->cmsg_type != SCM_RIGHTS ||
        cm->cmsg_len < CMSG_LEN(sizeof(int) * n))
        return -1;
    memcpy(fds, CMSG_DATA(cm), sizeof(int) * n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* password hashing                                                    */
/* ------------------------------------------------------------------ */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[n * 2] = 0;
}

static int hex_decode(const char *in, unsigned char *out, size_t max) {
    size_t n = strlen(in);
    if (n == 0 || (n & 1) || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(in[i * 2]);
        int b = hexval(in[i * 2 + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (unsigned char)((a << 4) | b);
    }
    return (int)(n / 2);
}

static int make_hash(const char *password, char *out, size_t outsz) {
    unsigned char salt[SALT_LEN];
    unsigned char dk[HASH_LEN];
    char salt_hex[SALT_LEN * 2 + 1];
    char hash_hex[HASH_LEN * 2 + 1];

    arc4random_buf(salt, sizeof(salt));
    if (CCKeyDerivationPBKDF(kCCPBKDF2, password, strlen(password),
                             salt, sizeof(salt), kCCPRFHmacAlgSHA256,
                             PBKDF2_ITERATIONS, dk, sizeof(dk)) != 0)
        return -1;

    hex_encode(salt, sizeof(salt), salt_hex);
    hex_encode(dk, sizeof(dk), hash_hex);
    if (snprintf(out, outsz, "pbkdf2_sha256$%d$%s$%s",
                 PBKDF2_ITERATIONS, salt_hex, hash_hex) >= (int)outsz)
        return -1;
    return 0;
}

static int ct_equal(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static int verify_hash(const char *password, const char *stored) {
    char copy[1024];
    char *save = NULL;
    char *algo, *iter_s, *salt_s, *hash_s;
    unsigned char salt[64];
    unsigned char expect[64];
    unsigned char dk[64];
    int iter, salt_len, hash_len;

    if (strlen(stored) >= sizeof(copy)) return 0;
    strcpy(copy, stored);
    algo = strtok_r(copy, "$", &save);
    iter_s = strtok_r(NULL, "$", &save);
    salt_s = strtok_r(NULL, "$", &save);
    hash_s = strtok_r(NULL, "$", &save);
    if (!algo || !iter_s || !salt_s || !hash_s) return 0;
    if (strcmp(algo, "pbkdf2_sha256") != 0) return 0;

    iter = atoi(iter_s);
    if (iter < 1000 || iter > 1000000) return 0;
    salt_len = hex_decode(salt_s, salt, sizeof(salt));
    hash_len = hex_decode(hash_s, expect, sizeof(expect));
    if (salt_len <= 0 || hash_len <= 0 || hash_len > (int)sizeof(dk)) return 0;

    if (CCKeyDerivationPBKDF(kCCPBKDF2, password, strlen(password),
                             salt, (size_t)salt_len, kCCPRFHmacAlgSHA256,
                             (uint)iter, dk, (size_t)hash_len) != 0)
        return 0;
    return ct_equal(dk, expect, (size_t)hash_len);
}

static int read_shadow(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = 0;
    return n > 0 ? 0 : -1;
}

static int write_shadow(const char *path, const char *hash) {
    char tmp[PATH_MAX];
    int fd;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror(tmp); return -1; }
    if (write_full(fd, hash, strlen(hash)) != 0 ||
        write_full(fd, "\n", 1) != 0) {
        perror("write shadow");
        close(fd);
        unlink(tmp);
        return -1;
    }
    fchmod(fd, 0600);
    fchown(fd, 0, 0);
    close(fd);
    if (rename(tmp, path) != 0) {
        perror("rename shadow");
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int run_passwd(int argc, char **argv) {
    char password[MAX_PASS + 1];
    char hash[512];
    size_t n = 0;
    char ch;

    if (argc != 2) {
        fprintf(stderr, "usage: jail-passwd <shadow-file>\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "jail-passwd: must be root\n");
        return 1;
    }
    while (n < MAX_PASS && read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == '\n' || ch == '\r') break;
        password[n++] = ch;
    }
    password[n] = 0;
    if (n == 0) {
        fprintf(stderr, "jail-passwd: empty password\n");
        return 1;
    }
    if (make_hash(password, hash, sizeof(hash)) != 0) {
        fprintf(stderr, "jail-passwd: hash failed\n");
        memset(password, 0, sizeof(password));
        return 1;
    }
    memset(password, 0, sizeof(password));
    if (write_shadow(argv[1], hash) != 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* cache exposure (same fixed fd numbers as jail-launcher)             */
/* ------------------------------------------------------------------ */

static int namecmp(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int mount_devfs(const char *jail) {
    char dev[PATH_MAX];
    struct statfs sfs;
    snprintf(dev, sizeof(dev), "%s/dev", jail);
    mkdir(dev, 0755);
    if (statfs(dev, &sfs) != 0 || strcmp(sfs.f_fstypename, "devfs") != 0) {
        if (mount("devfs", dev, 0, NULL) != 0) {
            perror("mount devfs");
            return -1;
        }
    }
    return 0;
}

static int expose_cache(const char *jail) {
    const char *real = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld";
    char jd[PATH_MAX];
    struct dirent **names = NULL;
    int n, slot = 0;

    snprintf(jd, sizeof(jd), "%s/System/Library/dyld", jail);
    mkdir(jd, 0755);
    n = scandir(real, &names, NULL, namecmp);
    if (n < 0) { perror("scandir cache"); return -1; }
    for (int i = 0; i < n; i++) {
        if (names[i]->d_name[0] == '.') { free(names[i]); continue; }
        if (CACHE_FD_BASE + slot >= 284) { free(names[i]); break; }
        char rp[PATH_MAX], lp[PATH_MAX], tgt[64];
        snprintf(rp, sizeof(rp), "%s/%s", real, names[i]->d_name);
        int fd = open(rp, O_RDONLY);
        if (fd < 0) { perror(rp); free(names[i]); continue; }
        int target = CACHE_FD_BASE + slot;
        if (dup2(fd, target) < 0) {
            perror("dup2 cache fd");
            close(fd);
            free(names[i]);
            continue;
        }
        if (fd != target) close(fd);
        fcntl(target, F_SETFD, 0);
        snprintf(lp, sizeof(lp), "%s/%s", jd, names[i]->d_name);
        unlink(lp);
        snprintf(tgt, sizeof(tgt), "/dev/fd/%d", target);
        if (symlink(tgt, lp) != 0 && errno != EEXIST) perror(lp);
        free(names[i]);
        slot++;
    }
    free(names);
    return 0;
}

/* ------------------------------------------------------------------ */
/* daemon                                                              */
/* ------------------------------------------------------------------ */

static void send_status(int fd, int status, const char *msg) {
    uint32_t mlen = msg ? (uint32_t)strlen(msg) : 0;
    send_u32(fd, (uint32_t)status);
    send_u32(fd, mlen);
    if (mlen) write_full(fd, msg, mlen);
}


static void child_exec(const char *root, char **args, const char *term,
                       const char *cwd, int fds[3], int sock) {
    if (sock >= 0) close(sock);
    if (chdir(root) != 0 || chroot(root) != 0) {
        dprintf(STDERR_FILENO, "chroot %s: %s\n", root, strerror(errno));
        _exit(127);
    }
    if (cwd && *cwd) {
        if (chdir(cwd) != 0) chdir("/");
    } else {
        chdir("/");
    }
    setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
    setenv("HOME", "/var/root", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", "/bin/bash", 1);
    if (term && *term) setenv("TERM", term, 1);

    setgroups(0, NULL);
    setgid(0);
    setuid(0);

    if (fds) {
        for (int i = 0; i < 3; i++) {
            if (fds[i] != i) {
                dup2(fds[i], i);
                close(fds[i]);
            }
        }
    }
    execvp(args[0], args);
    dprintf(STDERR_FILENO, "exec %s: %s\n", args[0], strerror(errno));
    _exit(127);
}

static int exit_code(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 127;
}

static void handle_client(int c, const char *root, const char *shadow,
                          uid_t expected_uid, struct rate_state *rate) {
    uint32_t magic, op, argc, passlen, flags, termlen, rows, cols, cwdlen;
    char password[MAX_PASS + 1];
    char term[MAX_TERM + 1];
    char cwd[PATH_MAX];
    char **args = NULL;
    char stored[1024];
    struct timeval tv = { 15, 0 };
    int status = ST_ERROR;
    const char *msg = NULL;

    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uid_t peer_uid; gid_t peer_gid;
    if (getpeereid(c, &peer_uid, &peer_gid) != 0) {
        send_status(c, ST_ERROR, "peer credentials unavailable");
        close(c);
        return;
    }
    if (peer_uid != expected_uid && peer_uid != 0) {
        send_status(c, ST_ERROR, "permission denied");
        close(c);
        return;
    }

    if (recv_u32(c, &magic) || recv_u32(c, &op) || recv_u32(c, &argc) ||
        recv_u32(c, &passlen) || recv_u32(c, &flags) ||
        recv_u32(c, &termlen) || recv_u32(c, &rows) || recv_u32(c, &cols) ||
        recv_u32(c, &cwdlen)) {
        close(c);
        return;
    }
    if (magic != AUTH_MAGIC || (op != OP_SUDO && op != OP_SU) ||
        argc > MAX_ARGS || passlen > MAX_PASS || termlen > MAX_TERM ||
        rows > 10000 || cols > 10000 || cwdlen >= PATH_MAX) {
        send_status(c, ST_ERROR, "bad request");
        close(c);
        return;
    }

    memset(password, 0, sizeof(password));
    if (passlen && read_full(c, password, passlen) != 0) {
        send_status(c, ST_ERROR, "bad password request");
        close(c);
        return;
    }
    password[passlen] = 0;

    memset(term, 0, sizeof(term));
    if (termlen && read_full(c, term, termlen) != 0) {
        send_status(c, ST_ERROR, "bad term request");
        close(c);
        return;
    }
    term[termlen] = 0;

    memset(cwd, 0, sizeof(cwd));
    if (cwdlen && read_full(c, cwd, cwdlen) != 0) {
        send_status(c, ST_ERROR, "bad cwd request");
        close(c);
        return;
    }
    cwd[cwdlen] = 0;

    args = calloc((size_t)argc + 2, sizeof(char *));
    if (!args) {
        send_status(c, ST_ERROR, "out of memory");
        close(c);
        return;
    }
    for (uint32_t i = 0; i < argc; i++) {
        uint32_t len;
        if (recv_u32(c, &len) != 0 || len > MAX_ARG_LEN) {
            send_status(c, ST_ERROR, "bad argument");
            goto out;
        }
        args[i] = malloc((size_t)len + 1);
        if (!args[i]) {
            send_status(c, ST_ERROR, "out of memory");
            goto out;
        }
        if (len && read_full(c, args[i], len) != 0) {
            send_status(c, ST_ERROR, "bad argument");
            goto out;
        }
        args[i][len] = 0;
    }
    args[argc] = NULL;
    if (argc == 0) {
        send_status(c, ST_ERROR, "empty command");
        goto out;
    }

    if (read_shadow(shadow, stored, sizeof(stored)) != 0 || stored[0] == 0) {
        status = ST_NOPASS;
        msg = "no password set; run ./jail.sh set-user-password";
        goto reply;
    }
    if (!verify_hash(password, stored)) {
        if (rate) {
            rate->fails++;
            rate->last = time(NULL);
            int delay = 1 << (rate->fails > 5 ? 5 : (rate->fails - 1));
            if (delay > 30) delay = 30;
            sleep((unsigned)delay);
        }
        status = ST_AUTH;
        msg = "incorrect password";
        goto reply;
    }
    if (rate) rate->fails = 0;

    if (flags & REQ_PTY) {
        struct winsize ws;
        int master = -1;
        pid_t pid;
        memset(&ws, 0, sizeof(ws));
        if (rows == 0) rows = 24;
        if (cols == 0) cols = 80;
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        pid = forkpty(&master, NULL, NULL, &ws);
        if (pid < 0) {
            status = ST_ERROR;
            msg = "forkpty failed";
            goto reply;
        }
        if (pid == 0) {
            child_exec(root, args, term, cwd, NULL, c);
            _exit(127);
        }
        send_status(c, ST_OK, NULL);
        if (send_fds(c, &master, 1) != 0) {
            kill(pid, SIGKILL);
        }
        close(master);
        int st;
        if (waitpid(pid, &st, 0) >= 0) send_u32(c, (uint32_t)exit_code(st));
        close(c);
        goto out;
    } else {
        int fds[3] = { -1, -1, -1 };
        if (recv_fds(c, fds, 3) != 0) {
            status = ST_ERROR;
            msg = "bad stdio fds";
            goto reply;
        }
        pid_t pid = fork();
        if (pid < 0) {
            status = ST_ERROR;
            msg = "fork failed";
            for (int i = 0; i < 3; i++) close(fds[i]);
            goto reply;
        }
        if (pid == 0) {
            child_exec(root, args, term, cwd, fds, c);
            _exit(127);
        }
        for (int i = 0; i < 3; i++) close(fds[i]);
        send_status(c, ST_OK, NULL);
        int st;
        if (waitpid(pid, &st, 0) >= 0) send_u32(c, (uint32_t)exit_code(st));
        close(c);
        goto out;
    }

reply:
    send_status(c, status, msg);
    close(c);
out:
    if (args) {
        for (uint32_t i = 0; i < argc; i++) free(args[i]);
        free(args);
    }
    memset(password, 0, sizeof(password));
}

static int acquire_daemon_lock(const char *root) {
    char path[PATH_MAX];
    int fd;
    struct flock fl;
    snprintf(path, sizeof(path), "%s/var/run/jail-authd.lock", root);
    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int run_daemon(int argc, char **argv) {
    const char *root = NULL, *sock = NULL, *shadow = NULL;
    int uid = -1, gid = -1;
    int l, c;
    struct sockaddr_un sa;
    struct rate_state *rate;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
        else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) sock = argv[++i];
        else if (strcmp(argv[i], "--shadow") == 0 && i + 1 < argc) shadow = argv[++i];
        else if (strcmp(argv[i], "--uid") == 0 && i + 1 < argc) uid = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gid") == 0 && i + 1 < argc) gid = atoi(argv[++i]);
    }
    if (!root || !sock || !shadow || uid < 0) {
        fprintf(stderr, "usage: jail-authd --root DIR --socket PATH --shadow PATH --uid UID [--gid GID]\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "jail-authd: must be root\n");
        return 1;
    }
    chdir("/");
    umask(077);

    int lockfd = acquire_daemon_lock(root);
    if (lockfd < 0) {
        fprintf(stderr, "jail-authd: another instance is already running\n");
        return 1;
    }
    (void)lockfd;

    if (mount_devfs(root) != 0 || expose_cache(root) != 0)
        return 1;

    signal(SIGPIPE, SIG_IGN);
    l = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l < 0) { perror("socket"); return 1; }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sock, sizeof(sa.sun_path) - 1);
    unlink(sock);
    if (bind(l, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("bind"); close(l); return 1;
    }
    chmod(sock, 0660);
    if (gid >= 0) chown(sock, 0, (gid_t)gid);
    if (listen(l, 16) != 0) { perror("listen"); close(l); return 1; }

    rate = mmap(NULL, sizeof(*rate), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANON, -1, 0);
    if (rate == MAP_FAILED) rate = NULL;

    for (;;) {
        c = accept(l, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            sleep(1);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(l);
            handle_client(c, root, shadow, (uid_t)uid, rate);
            _exit(0);
        }
        close(c);
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
}

/* ------------------------------------------------------------------ */
/* client                                                              */
/* ------------------------------------------------------------------ */

static int read_password_line(char *buf, size_t bufsz) {
    size_t n = 0;
    char ch;
    while (n + 1 < bufsz && read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == '\n') break;
        if (ch != '\r') buf[n++] = ch;
    }
    buf[n] = 0;
    return n > 0 ? 0 : -1;
}

static int connect_auth(void) {
    int s;
    struct sockaddr_un sa;
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCKET_PATH, sizeof(sa.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static int relay_pty(int master) {
    struct termios saved, raw;
    struct pollfd pfd[2];
    char buf[4096];
    int stdin_eof = 0;

    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        perror("tcgetattr");
        return -1;
    }
    raw = saved;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        perror("tcsetattr");
        return -1;
    }
    signal(SIGWINCH, on_winch);
    signal(SIGTERM, on_stop);
    signal(SIGHUP, on_stop);

    for (;;) {
        if (g_winch) {
            struct winsize ws;
            g_winch = 0;
            if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
                ioctl(master, TIOCSWINSZ, &ws);
        }
        if (g_stop) break;
        pfd[0].fd = stdin_eof ? -1 : STDIN_FILENO;
        pfd[0].events = POLLIN;
        pfd[1].fd = master;
        pfd[1].events = POLLIN;
        int r = poll(pfd, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                if (write_full(STDOUT_FILENO, buf, (size_t)n) != 0) break;
            } else if (n < 0 && (errno == EIO || errno == EINTR)) {
                if (errno == EIO) break;
            } else {
                break;
            }
        }
        if (pfd[0].revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (write_full(master, buf, (size_t)n) != 0) break;
            } else {
                char eof = 0x04;
                write_full(master, &eof, 1);
                stdin_eof = 1;
            }
        }
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    return 0;
}

static int run_client(int op, int argc, char **argv) {
    char *args[MAX_ARGS + 2];
    int nargs = 0;
    int read_stdin = 0;
    int want_pty;
    char password[MAX_PASS + 1];
    char cwd[PATH_MAX];
    const char *term;
    struct winsize ws;
    int s, master = -1;
    uint32_t status;
    uint32_t msglen;
    char msg[4096];
    const char *prefix = (op == OP_SUDO) ? "sudo" : "su";

    memset(args, 0, sizeof(args));
    if (op == OP_SUDO) {
        int i = 1, shell = 0;
        while (i < argc) {
            if (strcmp(argv[i], "--") == 0) { i++; break; }
            if (argv[i][0] != '-') break;
            if (strcmp(argv[i], "-S") == 0) read_stdin = 1;
            else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-i") == 0) shell = 1;
            else {
                fprintf(stderr, "%s: unsupported option %s\n", prefix, argv[i]);
                return 1;
            }
            i++;
        }
        if (shell) {
            args[nargs++] = "/bin/bash";
            args[nargs++] = "-l";
        } else {
            for (; i < argc && nargs < MAX_ARGS; i++) args[nargs++] = argv[i];
        }
        if (nargs == 0) {
            fprintf(stderr, "usage: sudo [-S] [-s|-i] command [args...]\n");
            return 1;
        }
    } else {
        int i = 1;
        char *cmd = NULL;
        while (i < argc) {
            if (strcmp(argv[i], "-S") == 0) { read_stdin = 1; i++; }
            else if (strcmp(argv[i], "-c") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "su: -c requires a command\n");
                    return 1;
                }
                cmd = argv[i + 1];
                i += 2;
            }
            else if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "root") == 0) {
                i++;
            }
            else {
                fprintf(stderr, "su: only root is supported\n");
                return 1;
            }
        }
        if (cmd) {
            args[nargs++] = "/bin/bash";
            args[nargs++] = "-c";
            args[nargs++] = cmd;
        } else {
            args[nargs++] = "/bin/bash";
            args[nargs++] = "-l";
        }
    }
    args[nargs] = NULL;

    if (read_stdin) {
        if (read_password_line(password, sizeof(password)) != 0) {
            fprintf(stderr, "%s: no password on stdin\n", prefix);
            return 1;
        }
    } else {
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr, "%s: no tty; use -S to read a password from stdin\n", prefix);
            return 1;
        }
        char *p = getpass("Password: ");
        if (!p) {
            fprintf(stderr, "%s: failed to read password\n", prefix);
            return 1;
        }
        strncpy(password, p, sizeof(password) - 1);
        password[sizeof(password) - 1] = 0;
        memset(p, 0, strlen(p));
    }

    want_pty = (!read_stdin && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
    term = getenv("TERM");
    if (!term || !*term) term = "xterm-256color";
    if (strlen(term) > MAX_TERM) term = "xterm-256color";
    memset(&ws, 0, sizeof(ws));
    if (want_pty) {
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
            ws.ws_row = 24;
            ws.ws_col = 80;
        }
        if (ws.ws_row == 0) ws.ws_row = 24;
        if (ws.ws_col == 0) ws.ws_col = 80;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        strcpy(cwd, "/");

    s = connect_auth();
    if (s < 0) {
        fprintf(stderr, "%s: cannot connect to jail-authd (%s); is the jail running?\n",
                prefix, SOCKET_PATH);
        return 1;
    }

    send_u32(s, AUTH_MAGIC);
    send_u32(s, (uint32_t)op);
    send_u32(s, (uint32_t)nargs);
    send_u32(s, (uint32_t)strlen(password));
    send_u32(s, want_pty ? REQ_PTY : 0);
    send_u32(s, (uint32_t)strlen(term));
    send_u32(s, (uint32_t)ws.ws_row);
    send_u32(s, (uint32_t)ws.ws_col);
    send_u32(s, (uint32_t)strlen(cwd));
    write_full(s, password, strlen(password));
    write_full(s, term, strlen(term));
    write_full(s, cwd, strlen(cwd));
    for (int i = 0; i < nargs; i++) {
        uint32_t len = (uint32_t)strlen(args[i]);
        send_u32(s, len);
        write_full(s, args[i], len);
    }
    memset(password, 0, sizeof(password));

    if (!want_pty) {
        int fds[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
        if (send_fds(s, fds, 3) != 0) {
            fprintf(stderr, "%s: cannot pass stdio fds\n", prefix);
            close(s);
            return 1;
        }
    }

    if (recv_u32(s, (uint32_t *)&status) != 0) {
        fprintf(stderr, "%s: lost connection to jail-authd\n", prefix);
        close(s);
        return 1;
    }
    if (recv_u32(s, &msglen) != 0) {
        close(s);
        return 1;
    }
    if (msglen > sizeof(msg) - 1) msglen = sizeof(msg) - 1;
    if (msglen && read_full(s, msg, msglen) != 0) {
        close(s);
        return 1;
    }
    msg[msglen] = 0;
    if (status != ST_OK) {
        fprintf(stderr, "%s: %s\n", prefix, msglen ? msg : "authentication failed");
        close(s);
        return 1;
    }
    if (want_pty) {
        if (recv_fds(s, &master, 1) != 0) {
            fprintf(stderr, "%s: failed to receive pty\n", prefix);
            close(s);
            return 1;
        }
        relay_pty(master);
        close(master);
    }
    uint32_t exit_status;
    if (recv_u32(s, &exit_status) != 0) exit_status = 1;
    close(s);
    return (int)(exit_status & 0xff);
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    const char *prog = base_name(argv[0]);

    if (strcmp(prog, "jail-authd") == 0)
        return run_daemon(argc, argv);
    if (strcmp(prog, "jail-passwd") == 0)
        return run_passwd(argc, argv);
    if (strcmp(prog, "sudo") == 0)
        return run_client(OP_SUDO, argc, argv);
    if (strcmp(prog, "su") == 0)
        return run_client(OP_SU, argc, argv);
    fprintf(stderr, "usage: jail-authd | jail-passwd <file> | sudo ... | su ...\n");
    return 2;
}
