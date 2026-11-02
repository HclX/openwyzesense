// ows-host-<app>: one app on the host board, in real time, as a process. Several processes share an air over
// local UDP, so a host gateway and host sensors pair and report as they do on the air:
//
//   ows-host-gateway --console pty --pty-link /tmp/gw.tty --nv gw.nv     # the host protocol on a pseudo-terminal
//   ows-host-sensor --nv sensor.nv [--slot-a a.img] [--slot-b b.img]     # the sensor's text console on stdin/out
//
// The gateway's pseudo-terminal is a serial port to anything that talks to a dongle: tools/ws_bridge.py,
// tools/host_link.py. On a stdio console, lines starting with "!" are for the simulated hardware, not the app:
//   !input event <0|1>     the switch (reed / PIR)         !reset    a reset (the NV page is kept)
//   !input button <0|1>    the pairing button              !quit
//
// The air: each process binds the first free UDP port of 16 from --air (127.0.0.1) and sends every frame it
// transmits to the others, with its start and end on CLOCK_MONOTONIC (one clock for every process here); a
// receiver delivers it at its end if its radio listened all along, as in the in-process simulation.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "boot_slot.h"
#include "host_board.h"
#include "input.h"

#define STR2(x)           #x
#define STR(x)            STR2(x)
#define APP_HEADER2(name) STR(name##_app.h)
#define APP_HEADER(name)  APP_HEADER2(name)  // expands name first (## would not)
#define APP_T(name)       name##_app_t
#define APP_T2(name)      APP_T(name)
#define HOST_APP2(name)   HOST_APP(name)
#include APP_HEADER(OWS_HOST_APP)

#define AIR_PORTS 16
#define AIR_MAGIC 0x4153574Fu  // "OWSA"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t sender;  // its port
    uint64_t start_us, end_us;
    int8_t rssi;
    uint8_t len;
    uint8_t frame[RADIO_MAX_FRAME];
} air_datagram_t;

static int air_fd = -1, air_base, air_port;
static volatile sig_atomic_t quit;

static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

static int air_open(int base)
{
    for (int i = 0; i < AIR_PORTS; i++) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = {
            .sin_family = AF_INET, .sin_port = htons((uint16_t)(base + i)), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            air_fd = fd, air_base = base, air_port = base + i;
            return 0;
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    return -1;
}

static void air_send(const host_air_t *f)
{
    air_datagram_t d = {AIR_MAGIC, (uint16_t)air_port, f->start_us, f->end_us, f->rssi, f->len, {0}};
    memcpy(d.frame, f->frame, f->len);
    for (int i = 0; i < AIR_PORTS; i++) {
        struct sockaddr_in a = {.sin_family = AF_INET,
                                .sin_port = htons((uint16_t)(air_base + i)),
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (air_base + i != air_port) {
            sendto(air_fd, &d, offsetof(air_datagram_t, frame) + f->len, 0, (struct sockaddr *)&a, sizeof a);
        }
    }
}

static void air_read(void)
{
    air_datagram_t d;
    ssize_t n;
    if (air_fd < 0) {
        return;
    }
    while ((n = recv(air_fd, &d, sizeof d, 0)) > 0) {
        if ((size_t)n < offsetof(air_datagram_t, frame) || d.magic != AIR_MAGIC || d.len > RADIO_MAX_FRAME ||
            (size_t)n != offsetof(air_datagram_t, frame) + d.len) {
            continue;
        }
        host_air_t f = {HOST_REMOTE + (d.sender - air_base), d.start_us, d.end_us, d.rssi, d.len, {0}};
        memcpy(f.frame, d.frame, d.len);
        host_air_receive(&f);
    }
}

// --- the NV page in a file ---

static const char *nv_path;

static void nv_load(host_dev_t *d)
{
    FILE *f = nv_path ? fopen(nv_path, "rb") : NULL;
    if (!f) {
        return;
    }
    if (fread(d->page, 1, sizeof d->page, f) != sizeof d->page) {
        memset(d->page, 0xFF, sizeof d->page);
    }
    fclose(f);
}

static void nv_save(host_dev_t *d)
{
    if (!nv_path || !d->nv_dirty) {
        return;
    }
    d->nv_dirty = false;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", nv_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    bool ok = fwrite(d->page, 1, sizeof d->page, f) == sizeof d->page;
    if (fclose(f) == 0 && ok) {
        rename(tmp, nv_path);
    }
}

static int slot_load(host_dev_t *d, int slot, const char *path)
{
    uint8_t img[SLOT_SIZE + 1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    size_t n = fread(img, 1, sizeof img, f);
    bool err = ferror(f) != 0;
    fclose(f);
    if (err || n == 0 || n > SLOT_SIZE) {
        fprintf(stderr, "%s: expected a non-empty slot image of at most %u bytes\n", path, (unsigned)SLOT_SIZE);
        return -1;
    }
    d->slots = true;
    host_install_slot(d, slot, img, n);
    return 0;
}

// --- the console ---

static int con_in = 0, con_out = 1, pty_slave = -1;
static const char *pty_link;
static bool stdio_console = true;

static int open_pty(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (m < 0 || grantpt(m) || unlockpt(m)) {
        return -1;
    }
    const char *name = ptsname(m);
    pty_slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);  // held open: the master never sees a hang-up
    struct termios t;
    if (pty_slave < 0 || tcgetattr(pty_slave, &t)) {
        return -1;
    }
    cfmakeraw(&t);
    tcsetattr(pty_slave, TCSANOW, &t);
    if (pty_link) {
        unlink(pty_link);
        if (symlink(name, pty_link)) {
            perror(pty_link);
        }
    }
    fprintf(stderr, "console on %s%s%s\n", name, pty_link ? " = " : "", pty_link ? pty_link : "");
    con_in = con_out = m;
    return 0;
}

static void console_out(host_dev_t *d)
{
    uint8_t b[4096];
    size_t n;
    while ((n = host_console_take(d, b, sizeof b))) {
        for (size_t off = 0; off < n;) {
            ssize_t w = write(con_out, b + off, n - off);
            if (w <= 0) {
                break;  // nobody reading the pseudo-terminal: drop, as a UART would
            }
            off += (size_t)w;
        }
    }
}

// A stdio line: "!..." for the simulated hardware, else the app's
static void stdio_line(host_dev_t *d, char *line)
{
    char what[16];
    int level;
    if (line[0] != '!') {
        host_console_send(d, line, strlen(line));
        host_console_send(d, "\n", 1);
    } else if (sscanf(line, "!input %15s %d", what, &level) == 2 &&
               (!strcmp(what, "event") || !strcmp(what, "button"))) {
        host_input(d, !strcmp(what, "event") ? INPUT_EVENT : INPUT_BUTTON, level != 0);
    } else if (!strcmp(line, "!reset")) {
        host_boot(d);
    } else if (!strcmp(line, "!quit")) {
        quit = 1;
    } else {
        fprintf(stderr, "unknown: %s\n", line);
    }
}

static int console_in(host_dev_t *d)
{
    static char line[512];
    static size_t n_line;
    uint8_t b[512];
    ssize_t n = read(con_in, b, sizeof b);
    if (n == 0 && stdio_console) {
        return -1;  // end of input
    }
    if (n <= 0) {
        return 0;
    }
    if (!stdio_console) {
        host_console_send(d, b, (size_t)n);
        return 0;
    }
    for (ssize_t i = 0; i < n; i++) {
        if (b[i] == '\n' || b[i] == '\r') {
            line[n_line] = 0;
            if (n_line) {
                stdio_line(d, line);
            }
            n_line = 0;
        } else if (n_line < sizeof line - 1) {
            line[n_line++] = (char)b[i];
        }
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--air PORT] [--ieee HEX] [--mac XXXXXXXX] [--nv FILE] [--slot-a FILE] [--slot-b FILE]\n"
            "          [--rssi DBM] [--console stdio|pty] [--pty-link PATH]\n"
            "          [--stay (stdio: keep running after end of input)]\n",
            argv0);
    exit(2);
}

int main(int argc, char **argv)
{
    static APP_T2(OWS_HOST_APP) app;
    const char *slot_path[2] = {NULL, NULL};
    int base = 47800;
    bool stay_on_eof = false;
    host_reset();
    host_dev_t *d = host_add(STR(OWS_HOST_APP), &app, HOST_APP2(OWS_HOST_APP));
    d->ieee ^= ((uint64_t)getpid() << 16) ^ ((uint64_t)getpid() * 0x9E37u & 0xFFFF);  // distinct per process
    // (the MACs come from the low bits) unless --ieee
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--air") && v) {
            base = atoi(v), i++;
        } else if (!strcmp(a, "--ieee") && v) {
            d->ieee = strtoull(v, NULL, 16), i++;
        } else if (!strcmp(a, "--mac") && v && strlen(v) == 8) {
            memcpy(d->mac, v, 8), d->has_mac = true, i++;
        } else if (!strcmp(a, "--nv") && v) {
            nv_path = v, i++;
        } else if (!strcmp(a, "--slot-a") && v) {
            slot_path[0] = v, i++;
        } else if (!strcmp(a, "--slot-b") && v) {
            slot_path[1] = v, i++;
        } else if (!strcmp(a, "--rssi") && v) {
            d->rssi = (int8_t)atoi(v), i++;
        } else if (!strcmp(a, "--console") && v) {
            stdio_console = strcmp(v, "pty") != 0, i++;
        } else if (!strcmp(a, "--pty-link") && v) {
            pty_link = v, i++;
        } else if (!strcmp(a, "--stay")) {
            stay_on_eof = true;
        } else {
            usage(argv[0]);
        }
    }
    for (int s = 0; s < 2; s++) {
        if (slot_path[s] && slot_load(d, s, slot_path[s])) {
            return 1;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (air_open(base)) {
        fprintf(stderr, "no free air port in %d-%d\n", base, base + AIR_PORTS - 1);
        return 1;
    }
    if (!stdio_console && open_pty()) {
        perror("pty");
        return 1;
    }
    host_on_tx = air_send;
    fprintf(stderr, "%s: air port %d (of %d-%d), ieee %016llX\n", STR(OWS_HOST_APP), air_port, base,
            base + AIR_PORTS - 1, (unsigned long long)d->ieee);
    nv_load(d);
    host_set_now_us(mono_us());
    host_boot(d);
    nv_save(d);
    console_out(d);

    bool in_open = true;
    while (!quit) {
        host_set_now_us(mono_us());
        uint64_t next = host_poll();
        console_out(d);
        nv_save(d);
        uint64_t now = mono_us();
        int timeout = next <= now ? 0 : next - now > 1000000 ? 1000 : (int)((next - now + 999) / 1000);
        struct pollfd p[2] = {{air_fd, POLLIN, 0}, {con_in, POLLIN, 0}};
        if (poll(p, in_open ? 2 : 1, timeout) < 0 && errno != EINTR) {
            break;
        }
        if (p[0].revents & POLLIN) {
            air_read();
        }
        if (in_open && (p[1].revents & (POLLIN | POLLHUP))) {
            host_set_now_us(mono_us());
            if (console_in(d) < 0) {
                in_open = false;
                if (!stay_on_eof) {
                    quit = 1;
                }
            }
        }
    }
    nv_save(d);
    if (pty_link) {
        unlink(pty_link);
    }
    return 0;
}
