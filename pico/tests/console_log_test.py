#!/usr/bin/env python3
"""The USB/UART console's output rules, with main.c compiled for the host.

main.c runs unchanged (its main() renamed); the SDK, WiFi, link, relay, NAT
and keyboard modules are stubs. log.c and hidkeys.c are the real ones. The
driver types commands into console_poll() and steps wifi_poll(); the firmware's
stdout is checked here, the typed-character echo goes to stderr.

Checked: logging is off at boot, `log on|off|` behave, a reply prints either
way, the WiFi join after `wifi ...` prints with logging off and later state
changes do not, and every line starts with a prefix listed in log.h.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/console_log_test"
for d in ("pico", "lwip"):
    (BUILD / d).mkdir(parents=True, exist_ok=True)
(BUILD / "pico/stdlib.h").write_text('''
#include <stdint.h>
#include <stdbool.h>
#define PICO_ERROR_TIMEOUT (-1)
extern uint32_t t_now;
static inline uint32_t get_absolute_time(void) { return t_now; }
static inline uint32_t to_ms_since_boot(uint32_t t) { return t; }
static inline void tight_loop_contents(void) {}
int getchar_timeout_us(uint32_t us);
void stdio_init_all(void);
''')
(BUILD / "pico/cyw43_arch.h").write_text('''
#include <stdint.h>
#define CYW43_LINK_DOWN 0
#define CYW43_LINK_JOIN 1
#define CYW43_LINK_NOIP 2
#define CYW43_LINK_UP 3
#define CYW43_LINK_FAIL (-1)
#define CYW43_ITF_STA 0
#define CYW43_AUTH_OPEN 0
#define CYW43_AUTH_WPA2_AES_PSK 1
typedef struct { int x; } cyw43_t;
extern cyw43_t cyw43_state;
int cyw43_arch_init(void);
void cyw43_arch_enable_sta_mode(void);
void cyw43_arch_lwip_begin(void);
void cyw43_arch_lwip_end(void);
int cyw43_tcpip_link_status(cyw43_t *s, int itf);
int cyw43_wifi_leave(cyw43_t *s, int itf);
int cyw43_arch_wifi_connect_async(const char *ssid, const char *pass, uint32_t auth);
''')
(BUILD / "lwip/netif.h").write_text('''
struct netif; typedef struct { int x; } ip4_addr_t;
extern struct netif *netif_default;
const ip4_addr_t *netif_ip4_addr(struct netif *n);
const char *ip4addr_ntoa(const ip4_addr_t *a);
''')
(BUILD / "driver.c").write_text(r'''
#include <stdio.h>
#include <string.h>
#define putchar(c) (fputc((c), stderr))
#define main firmware_main
#include "main.c"
#undef main
#undef putchar
#include "kbd.h"

uint32_t t_now = 1000;
cyw43_t cyw43_state;
struct netif *netif_default;
static int link_status = CYW43_LINK_DOWN;
static const char *input;
int getchar_timeout_us(uint32_t us) { (void)us; return *input ? (unsigned char)*input++ : PICO_ERROR_TIMEOUT; }
void stdio_init_all(void) {}
int cyw43_arch_init(void) { return 0; }
void cyw43_arch_enable_sta_mode(void) {}
void cyw43_arch_lwip_begin(void) {}
void cyw43_arch_lwip_end(void) {}
int cyw43_tcpip_link_status(cyw43_t *s, int itf) { (void)s; (void)itf; return link_status; }
int cyw43_wifi_leave(cyw43_t *s, int itf) { (void)s; (void)itf; return 0; }
int cyw43_arch_wifi_connect_async(const char *a, const char *b, uint32_t c) { (void)a; (void)b; (void)c; return 0; }
const ip4_addr_t *netif_ip4_addr(struct netif *n) { (void)n; static ip4_addr_t a; return &a; }
const char *ip4addr_ntoa(const ip4_addr_t *a) { (void)a; return "10.0.0.5"; }
bool config_load(config_t *c) { memset(c, 0, sizeof(*c)); return true; }
bool config_save(const config_t *c) { (void)c; return true; }
bool config_clear(void) { return true; }
void link_init(void) {}
bool link_push(uint8_t ch, const uint8_t *d, size_t n) { (void)ch; (void)d; (void)n; return true; }
link_rx_t link_on_rx(const uint8_t *f) { (void)f; link_rx_t r; memset(&r, 0, sizeof(r)); return r; }
void link_next_out(uint8_t *f) { (void)f; }
size_t link_queued(uint8_t ch) { (void)ch; return 0; }
link_stats_t link_stats(void) { link_stats_t s; memset(&s, 0, sizeof(s)); return s; }
void port_init(void) {}
void port_set_tx(const uint8_t *f) { (void)f; }
void port_set_idle(bool i) { (void)i; }
bool port_take_rx(uint8_t *f) { (void)f; return false; }
port_stats_t port_stats(void) { port_stats_t s; memset(&s, 0, sizeof(s)); return s; }
void ctrl_reset(void) {}
void ctrl_bytes(const uint8_t *d, size_t n) { (void)d; (void)n; }
void ctrl_set_forwarder(bool (*a)(uint16_t, uint8_t), void (*d)(uint16_t, uint8_t)) { (void)a; (void)d; }
void btkbd_init(void) {}
void btkbd_poll(void) {}
bool btkbd_command(const char *l) { return !strncmp(l, "bt", 2) && (puts("[BT] stub"), true); }
void kbd_init(void) {}
void kbd_poll(void) {}
void kbd_test(bool on) { (void)on; }
bool kbd_type(const char *t) { (void)t; return true; }
void kbd_link_restart(void) {}
void kbd_set_map(const uint8_t m[12]) { (void)m; }
size_t kbd_map_text(char *out, size_t cap) { snprintf(out, cap, "B=z"); return 3; }
void relay_init(void) {}
void relay_poll(void) {}
void relay_configure(const char *h, bool up) { (void)h; (void)up; }
void relay_reset(void) {}
void relay_ip_bytes(const uint8_t *d, size_t n) { (void)d; (void)n; }
const char *relay_state(void) { return "down"; }
void nat_init(void) {}
void nat_reset(void) {}
void nat_configure(bool up) { (void)up; }
void nat_ip_bytes(const uint8_t *d, size_t n) { (void)d; (void)n; }
void nat_poll(void) {}
bool nat_forward_add(uint16_t p, uint8_t c) { (void)p; (void)c; return true; }
void nat_forward_del(uint16_t p, uint8_t c) { (void)p; (void)c; }
nat_stats_t nat_stats(void) { nat_stats_t s; memset(&s, 0, sizeof(s)); return s; }

static void type(const char *text) {
    input = text;
    while (*input) console_poll();
}
static void phase(const char *name) { fflush(stdout); fprintf(stdout, "@@ %s\n", name); }
static void wifi_steps(const int *statuses, unsigned n) {
    for (unsigned i = 0; i < n; ++i) { link_status = statuses[i]; t_now += 100; wifi_poll(); }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    config_load(&settings);
    phase("boot-stats");              print_stats(false);
    phase("boot-stats-command");      type("stats\r");
    phase("log-query");               type("log\r");
    phase("log-on");                  type("log on\r"); print_stats(false);
    phase("log-off");                 type("log off\r"); print_stats(false); type("log\r");
    phase("commands");
    type("mode\rmode nat\rmode relay 10.0.0.9\rwifi?\rtype hi\rtype\rkbd test on\rkbd test off\r");
    type("kbd map\rkbd map B z\rkbd map reset\rkbd map nonsense\rbt scan\rbogus\r");
    type("wifi bad|x\r");
    phase("wifi-join-watched");
    type("wifi Net|pw|\r");
    {   int s[] = {CYW43_LINK_DOWN, CYW43_LINK_JOIN, CYW43_LINK_NOIP, CYW43_LINK_UP};
        wifi_steps(s, 4); }
    phase("wifi-after-join");
    t_now += 20000;
    {   int s[] = {CYW43_LINK_DOWN, CYW43_LINK_DOWN}; wifi_steps(s, 2); }
    t_now += 20000;
    {   int s[] = {CYW43_LINK_DOWN}; wifi_steps(s, 1); }
    phase("wifi-fail-watched");
    type("wifi Other|pw|\r");
    {   int s[] = {CYW43_LINK_DOWN, CYW43_LINK_JOIN, CYW43_LINK_FAIL}; wifi_steps(s, 3); }
    phase("wifi-clear");
    type("wifi clear\r");
    {   int s[] = {CYW43_LINK_DOWN}; wifi_steps(s, 1); }
    phase("wifi-quiet-after-window");
    type("wifi Late|pw|\r");
    t_now += 61000;
    {   int s[] = {CYW43_LINK_DOWN, CYW43_LINK_JOIN, CYW43_LINK_UP}; wifi_steps(s, 3); }
    phase("log-on-again"); type("log on\r"); print_stats(false);
    phase("end");
    return 0;
}
''')
exe = BUILD / "driver"
subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(BUILD), "-I", str(ROOT),
                str(BUILD / "driver.c"), str(ROOT / "log.c"), str(ROOT / "hidkeys.c"),
                "-o", str(exe)], check=True)
run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)

prefixes = re.findall(r'#define P_[A-Z]+\s+"(\[[A-Z]+\] )"', (ROOT / "log.h").read_text())
assert len(prefixes) >= 10, prefixes
phases, cur = {}, None
for line in run.stdout.splitlines():
    if line.startswith("@@ "):
        cur = line[3:]
        phases[cur] = []
    else:
        phases[cur].append(line)
        assert line.startswith(tuple(prefixes)), f"no known prefix: {line!r}"
        assert sum(line.count(p) for p in prefixes) == 1, f"two prefixes: {line!r}"


def has(phase, text):
    return any(text in line for line in phases[phase])


assert phases["boot-stats"] == [], "stats printed at boot with logging off"
assert phases["boot-stats-command"][0].startswith("[SNES] gaps=")
assert phases["boot-stats-command"][1].startswith("[NAT] in=")
assert phases["log-query"] == ["[LOG] off"]
assert phases["log-on"][0] == "[LOG] on" and phases["log-on"][1].startswith("[SNES] gaps=")
assert len(phases["log-on"]) == 3
assert phases["log-off"][0] == "[LOG] off" and phases["log-off"][1] == "[LOG] off"
assert len(phases["log-off"]) == 2, "stats printed after log off"
for text in ("[MODE] nat", "[MODE] relay", "[WIFI] ssid=", "[KBD] sent", "[KBD] test pattern on",
             "[KBD] test pattern off", "[KBD] map: B=z", "[KBD] map <", "[BT] stub",
             "[CON] commands: "):
    assert has("commands", text), text
assert has("commands", "[WIFI] settings saved") is False
watched = phases["wifi-join-watched"]
assert watched[0] == "[WIFI] settings saved"
assert watched[1:] == ["[WIFI] joining", "[WIFI] dhcp", "[WIFI] joined 10.0.0.5"], watched
assert phases["wifi-after-join"] == [], phases["wifi-after-join"]
assert phases["wifi-fail-watched"][0] == "[WIFI] settings saved"
assert phases["wifi-fail-watched"][1:] == ["[WIFI] joining", "[WIFI] join failed (-1)"], \
    phases["wifi-fail-watched"]
assert phases["wifi-clear"][0] == "[WIFI] saved settings cleared"
assert "[WIFI] not configured, type: wifi SSID|PASSWORD|HOST" in phases["wifi-clear"]
assert phases["wifi-quiet-after-window"] == ["[WIFI] settings saved"], \
    phases["wifi-quiet-after-window"]
assert phases["log-on-again"][0] == "[LOG] on"
print(f"console log: {len(prefixes)} prefixes, {sum(map(len, phases.values()))} lines checked")
