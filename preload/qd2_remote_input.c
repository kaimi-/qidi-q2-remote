#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef EVIOCGPROP
#define EVIOCGPROP(len) _IOC(_IOC_READ, 'E', 0x09, len)
#endif

#define TARGET_PATH             "/dev/input/event0"
#define DEFAULT_SOCKET_PATH     "/run/qd2-remote-input.sock"
#define DEFAULT_DEVICE_NAME     "QD Remote Pointer"
#define SCREEN_WIDTH            480
#define SCREEN_HEIGHT           272
#define MT_TRACKING_ID_MAX      65535

#define BITS_PER_LONG           (sizeof(long) * 8)
#define NBITS(x)                (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define BIT_SET(arr, bit)       ((arr)[(bit) / BITS_PER_LONG] |= (1UL << ((bit) % BITS_PER_LONG)))

typedef int  (*open_fn)(const char *, int, ...);
typedef int  (*openat_fn)(int, const char *, int, ...);
typedef int  (*close_fn)(int);
typedef int  (*ioctl_fn)(int, unsigned long, ...);
typedef int  (*fcntl_fn)(int, int, ...);

static open_fn    real_open;
static open_fn    real_open64;
static openat_fn  real_openat;
static openat_fn  real_openat64;
static close_fn   real_close;
static ioctl_fn   real_ioctl;
static fcntl_fn   real_fcntl;

struct fake_input_ctx {
    int               fake_read_fd;
    int               fake_write_fd;
    atomic_int        active;
    pthread_t         ipc_thread;
    pthread_mutex_t   emit_lock;
    int32_t           cur_tracking_id;
    int               last_x;
    int               last_y;
    bool              ipc_thread_started;
};

static struct fake_input_ctx g_ctx = {
    .fake_read_fd       = -1,
    .fake_write_fd      = -1,
    .active             = 0,
    .cur_tracking_id    = 0,
    .last_x             = 0,
    .last_y             = 0,
    .ipc_thread_started = false,
};

static pthread_once_t  g_init_once = PTHREAD_ONCE_INIT;
static bool            g_debug     = false;

static void dbg(const char *fmt, ...)
{
    if (!g_debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[qd2_preload] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void resolve_symbols(void)
{
    real_open     = (open_fn)   dlsym(RTLD_NEXT, "open");
    real_open64   = (open_fn)   dlsym(RTLD_NEXT, "open64");
    real_openat   = (openat_fn) dlsym(RTLD_NEXT, "openat");
    real_openat64 = (openat_fn) dlsym(RTLD_NEXT, "openat64");
    real_close    = (close_fn)  dlsym(RTLD_NEXT, "close");
    real_ioctl    = (ioctl_fn)  dlsym(RTLD_NEXT, "ioctl");
    real_fcntl    = (fcntl_fn)  dlsym(RTLD_NEXT, "fcntl");
}

static const char *socket_path(void)
{
    const char *p = getenv("QD_REMOTE_INPUT_SOCK");
    return (p && *p) ? p : DEFAULT_SOCKET_PATH;
}

static void library_init(void)
{
    resolve_symbols();
    g_debug = getenv("QD_REMOTE_DEBUG") != NULL;
    pthread_mutex_init(&g_ctx.emit_lock, NULL);
    dbg("initialized, target=%s sock=%s", TARGET_PATH, socket_path());
}

static void ensure_init(void)
{
    pthread_once(&g_init_once, library_init);
}

static bool path_matches_target(const char *path)
{
    if (!path) return false;
    if (strcmp(path, TARGET_PATH) == 0) return true;
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        if (strcmp(resolved, TARGET_PATH) == 0) return true;
    }
    return false;
}

static int emit_event_locked(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    memset(&ev, 0, sizeof(ev));
    ev.time.tv_sec  = tv.tv_sec;
    ev.time.tv_usec = tv.tv_usec;
    ev.type  = type;
    ev.code  = code;
    ev.value = value;

    ssize_t n = write(g_ctx.fake_write_fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) {
        dbg("emit_event short write: %zd errno=%d", n, errno);
        return -1;
    }
    return 0;
}

static int emit_syn_locked(void)
{
    return emit_event_locked(EV_SYN, SYN_REPORT, 0);
}

static int clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void cmd_down(int x, int y)
{
    x = clamp(x, 0, SCREEN_WIDTH  - 1);
    y = clamp(y, 0, SCREEN_HEIGHT - 1);

    pthread_mutex_lock(&g_ctx.emit_lock);
    g_ctx.cur_tracking_id = (g_ctx.cur_tracking_id + 1) & MT_TRACKING_ID_MAX;
    g_ctx.last_x = x;
    g_ctx.last_y = y;

    emit_event_locked(EV_ABS, ABS_MT_TRACKING_ID, g_ctx.cur_tracking_id);
    emit_event_locked(EV_ABS, ABS_MT_POSITION_X, x);
    emit_event_locked(EV_ABS, ABS_MT_POSITION_Y, y);
    emit_event_locked(EV_ABS, ABS_X, x);
    emit_event_locked(EV_ABS, ABS_Y, y);
    emit_event_locked(EV_KEY, BTN_TOUCH, 1);
    emit_event_locked(EV_KEY, BTN_LEFT,  1);
    emit_syn_locked();
    pthread_mutex_unlock(&g_ctx.emit_lock);
}

static void cmd_move(int x, int y)
{
    x = clamp(x, 0, SCREEN_WIDTH  - 1);
    y = clamp(y, 0, SCREEN_HEIGHT - 1);

    pthread_mutex_lock(&g_ctx.emit_lock);
    g_ctx.last_x = x;
    g_ctx.last_y = y;

    emit_event_locked(EV_ABS, ABS_MT_POSITION_X, x);
    emit_event_locked(EV_ABS, ABS_MT_POSITION_Y, y);
    emit_event_locked(EV_ABS, ABS_X, x);
    emit_event_locked(EV_ABS, ABS_Y, y);
    emit_syn_locked();
    pthread_mutex_unlock(&g_ctx.emit_lock);
}

static void cmd_up(void)
{
    pthread_mutex_lock(&g_ctx.emit_lock);
    emit_event_locked(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit_event_locked(EV_KEY, BTN_TOUCH, 0);
    emit_event_locked(EV_KEY, BTN_LEFT,  0);
    emit_syn_locked();
    pthread_mutex_unlock(&g_ctx.emit_lock);
}

static void cmd_click(int x, int y)
{
    cmd_down(x, y);
    struct timespec ts = { 0, 30 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    cmd_up();
}

static void cmd_key(int code, int value)
{
    pthread_mutex_lock(&g_ctx.emit_lock);
    emit_event_locked(EV_KEY, (uint16_t)code, value);
    emit_syn_locked();
    pthread_mutex_unlock(&g_ctx.emit_lock);
}

static void process_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;

    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';

    char cmd[16] = {0};
    int  x = 0, y = 0, a = 0, b = 0;

    int n = sscanf(line, "%15s %d %d %d %d", cmd, &x, &y, &a, &b);
    if (n < 1) return;

    if (strcmp(cmd, "down") == 0 && n >= 3) {
        cmd_down(x, y);
    } else if (strcmp(cmd, "move") == 0 && n >= 3) {
        cmd_move(x, y);
    } else if (strcmp(cmd, "up") == 0) {
        cmd_up();
    } else if (strcmp(cmd, "click") == 0 && n >= 3) {
        cmd_click(x, y);
    } else if (strcmp(cmd, "key") == 0 && n >= 3) {
        cmd_key(x, y);
    } else {
        dbg("unknown command: %s", cmd);
    }
}

static int connect_ipc(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        real_close(fd);
        return -1;
    }
    return fd;
}

static void *ipc_thread_main(void *arg)
{
    (void)arg;
    char buf[512];
    size_t used = 0;

    while (atomic_load(&g_ctx.active)) {
        int sock = connect_ipc();
        if (sock < 0) {
            sleep(1);
            continue;
        }
        dbg("ipc connected");
        used = 0;

        while (atomic_load(&g_ctx.active)) {
            ssize_t n = read(sock, buf + used, sizeof(buf) - used - 1);
            if (n <= 0) {
                dbg("ipc disconnected n=%zd", n);
                break;
            }
            used += (size_t)n;
            buf[used] = '\0';

            char *start = buf;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                process_line(start);
                start = nl + 1;
            }
            size_t remain = (size_t)(buf + used - start);
            if (remain > 0 && start != buf) {
                memmove(buf, start, remain);
            } else if (remain == sizeof(buf) - 1) {
                remain = 0;
            }
            used = remain;
        }
        real_close(sock);
        sleep(1);
    }
    return NULL;
}

static void start_ipc_thread_if_needed(void)
{
    if (g_ctx.ipc_thread_started) return;
    atomic_store(&g_ctx.active, 1);
    if (pthread_create(&g_ctx.ipc_thread, NULL, ipc_thread_main, NULL) == 0) {
        g_ctx.ipc_thread_started = true;
    } else {
        dbg("failed to start ipc thread");
    }
}

static int open_fake_event0(int flags)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return -1;
    }
    g_ctx.fake_read_fd  = fds[0];
    g_ctx.fake_write_fd = fds[1];

    if (flags & O_NONBLOCK) {
        int rf = real_fcntl(g_ctx.fake_read_fd, F_GETFL);
        real_fcntl(g_ctx.fake_read_fd, F_SETFL, rf | O_NONBLOCK);
    }
    int wf = real_fcntl(g_ctx.fake_write_fd, F_GETFL);
    real_fcntl(g_ctx.fake_write_fd, F_SETFL, wf | O_NONBLOCK);

    start_ipc_thread_if_needed();
    dbg("handed fake evdev fd=%d (write=%d)", g_ctx.fake_read_fd, g_ctx.fake_write_fd);
    return g_ctx.fake_read_fd;
}

int open(const char *pathname, int flags, ...)
{
    ensure_init();

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (path_matches_target(pathname)) {
        dbg("open(%s) intercepted", pathname);
        int fd = open_fake_event0(flags);
        if (fd < 0) errno = ENOMEM;
        return fd;
    }
    return real_open(pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...)
{
    ensure_init();

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (path_matches_target(pathname)) {
        dbg("open64(%s) intercepted", pathname);
        int fd = open_fake_event0(flags);
        if (fd < 0) errno = ENOMEM;
        return fd;
    }
    if (real_open64) return real_open64(pathname, flags, mode);
    return real_open(pathname, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    ensure_init();

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (pathname[0] == '/' && path_matches_target(pathname)) {
        dbg("openat(%s) intercepted", pathname);
        int fd = open_fake_event0(flags);
        if (fd < 0) errno = ENOMEM;
        return fd;
    }
    return real_openat(dirfd, pathname, flags, mode);
}

int openat64(int dirfd, const char *pathname, int flags, ...)
{
    ensure_init();

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (pathname[0] == '/' && path_matches_target(pathname)) {
        dbg("openat64(%s) intercepted", pathname);
        int fd = open_fake_event0(flags);
        if (fd < 0) errno = ENOMEM;
        return fd;
    }
    if (real_openat64) return real_openat64(dirfd, pathname, flags, mode);
    return real_openat(dirfd, pathname, flags, mode);
}

int close(int fd)
{
    ensure_init();
    if (fd >= 0 && fd == g_ctx.fake_read_fd) {
        dbg("close(fake_read_fd=%d)", fd);
        int write_fd = g_ctx.fake_write_fd;
        g_ctx.fake_read_fd  = -1;
        g_ctx.fake_write_fd = -1;
        int rc = real_close(fd);
        if (write_fd >= 0) real_close(write_fd);
        return rc;
    }
    return real_close(fd);
}

static int handle_evio_ioctl(int fd, unsigned long req, void *argp)
{
    (void)fd;
    unsigned int type = _IOC_TYPE(req);
    unsigned int nr   = _IOC_NR(req);
    unsigned int size = _IOC_SIZE(req);

    if (type != 'E') {
        errno = EINVAL;
        return -1;
    }

    if (req == EVIOCGVERSION) {
        *(int *)argp = 0x010001;
        return 0;
    }
    if (req == EVIOCGID) {
        struct input_id *id = argp;
        id->bustype = BUS_VIRTUAL;
        id->vendor  = 0x0001;
        id->product = 0x0001;
        id->version = 0x0001;
        return 0;
    }

    if (nr == _IOC_NR(EVIOCGNAME(0))) {
        const char *name = DEFAULT_DEVICE_NAME;
        size_t name_len  = strlen(name) + 1;
        size_t copy_len  = size < name_len ? size : name_len;
        memcpy(argp, name, copy_len);
        return (int)copy_len;
    }

    if (nr == _IOC_NR(EVIOCGPHYS(0))) {
        const char *phys = "qd2-remote/input0";
        size_t plen = strlen(phys) + 1;
        size_t clen = size < plen ? size : plen;
        memcpy(argp, phys, clen);
        return (int)clen;
    }

    if (nr == _IOC_NR(EVIOCGPROP(0))) {
        memset(argp, 0, size);
        return (int)size;
    }

    if (nr >= _IOC_NR(EVIOCGBIT(0, 0)) && nr < _IOC_NR(EVIOCGBIT(0, 0)) + EV_MAX) {
        unsigned int ev = nr - _IOC_NR(EVIOCGBIT(0, 0));
        memset(argp, 0, size);
        unsigned long *bits = argp;
        size_t nbytes = size;
        size_t nlongs = nbytes / sizeof(long);
        if (nlongs == 0) return (int)size;

        if (ev == 0) {
            BIT_SET(bits, EV_SYN);
            BIT_SET(bits, EV_KEY);
            BIT_SET(bits, EV_REL);
            BIT_SET(bits, EV_ABS);
        } else if (ev == EV_KEY) {
            if ((size_t)(BTN_LEFT  / BITS_PER_LONG) < nlongs) BIT_SET(bits, BTN_LEFT);
            if ((size_t)(BTN_TOUCH / BITS_PER_LONG) < nlongs) BIT_SET(bits, BTN_TOUCH);
        } else if (ev == EV_REL) {
            BIT_SET(bits, REL_X);
            BIT_SET(bits, REL_Y);
        } else if (ev == EV_ABS) {
            BIT_SET(bits, ABS_X);
            BIT_SET(bits, ABS_Y);
            if ((size_t)(ABS_MT_POSITION_X  / BITS_PER_LONG) < nlongs) BIT_SET(bits, ABS_MT_POSITION_X);
            if ((size_t)(ABS_MT_POSITION_Y  / BITS_PER_LONG) < nlongs) BIT_SET(bits, ABS_MT_POSITION_Y);
            if ((size_t)(ABS_MT_TRACKING_ID / BITS_PER_LONG) < nlongs) BIT_SET(bits, ABS_MT_TRACKING_ID);
        }
        return (int)size;
    }

    if (nr >= _IOC_NR(EVIOCGABS(0)) && nr < _IOC_NR(EVIOCGABS(0)) + ABS_MAX) {
        struct input_absinfo *abs = argp;
        memset(abs, 0, sizeof(*abs));
        unsigned int axis = nr - _IOC_NR(EVIOCGABS(0));
        switch (axis) {
            case ABS_X:
            case ABS_MT_POSITION_X:
                abs->minimum = 0;
                abs->maximum = SCREEN_WIDTH - 1;
                break;
            case ABS_Y:
            case ABS_MT_POSITION_Y:
                abs->minimum = 0;
                abs->maximum = SCREEN_HEIGHT - 1;
                break;
            case ABS_MT_TRACKING_ID:
                abs->minimum = 0;
                abs->maximum = MT_TRACKING_ID_MAX;
                break;
            default:
                abs->minimum = 0;
                abs->maximum = 0;
                break;
        }
        return 0;
    }

    if (req == EVIOCGRAB) {
        return 0;
    }

    errno = EINVAL;
    return -1;
}

int ioctl(int fd, unsigned long request, ...)
{
    ensure_init();

    void *argp;
    va_list ap;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);

    if (fd >= 0 && fd == g_ctx.fake_read_fd) {
        int rc = handle_evio_ioctl(fd, request, argp);
        dbg("fake ioctl req=0x%lx -> %d errno=%d", request, rc, errno);
        return rc;
    }
    return real_ioctl(fd, request, argp);
}

int fcntl(int fd, int cmd, ...)
{
    ensure_init();

    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    return real_fcntl(fd, cmd, arg);
}
