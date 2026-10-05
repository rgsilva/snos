#include <stdio.h>
#include <inttypes.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include <string.h>
#include "config.h"
#include "lwip/netif.h"
#include "port.h"
#include "ctrl.h"
#include "btkbd.h"
#include "kbd.h"
#include "hidkeys.h"
#include "relay.h"
#include "nat.h"
#include "log.h"
static config_t settings;
static bool wifi_ready, wifi_attempt;
static int wifi_status = 99;
static uint32_t wifi_retry_at, wifi_join_at;
static const char *wifi_state = "starting";
static bool nat_mode(void) { return settings.host[0] == '\0'; }
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static void print_stats(bool reply) {
    port_stats_t p = port_stats(); link_stats_t l = link_stats();
    (reply ? printf : log_printf)("gaps=%"PRIu32" frames=%"PRIu32" short=%"PRIu32" clocks=%"PRIu32
           " overrun=%"PRIu32" badsum=%"PRIu32" seq=%u ack=%u resends=%"PRIu32" win=%u"
           " kbd=%u mode=%s %s wifi=%s host=%s\n", p.gaps, p.complete, p.short_frames,
           p.clocks, p.overruns, l.bad_sum, l.seq, l.ack, l.resends, l.window,
           (unsigned)link_queued(CH_KBD), nat_mode() ? "nat" : "relay",
           nat_mode() ? "" : relay_state(), wifi_state,
           settings.host[0] ? settings.host : "(none)");
    if (nat_mode()) { nat_stats_t n = nat_stats();
        (reply ? printf : log_printf)("nat: in=%"PRIu32" out=%"PRIu32" drop=%"PRIu32
            " tcp=%u udp=%u icmp=%u fwd=%u speed=%u\n", n.in, n.out, n.drop, n.tcp,
            n.udp, n.icmp, n.forwards, n.speed); }
}
static void wifi_changed(void) {
    if (wifi_ready) {
        relay_configure(settings.host, false); nat_configure(false); ctrl_set_forwarder(NULL, NULL);
        cyw43_arch_lwip_begin();
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_end();
    }
    wifi_attempt = false; wifi_status = 99; wifi_retry_at = now_ms();
    wifi_state = settings.ssid[0] ? "starting" : "unconfigured";
}
static void wifi_poll(void) {
    uint32_t now = now_ms();
    if (!wifi_ready) {
        if ((int32_t)(now - wifi_retry_at) < 0) return;
        wifi_retry_at = now + 10000;
        int e = cyw43_arch_init();
        if (e) {
            if (wifi_status != e) log_printf("wifi: init failed (%d)\n", e);
            wifi_status = e; wifi_state = "init-failed"; return;
        }
        wifi_ready = true;
        btkbd_init();
        cyw43_arch_enable_sta_mode(); relay_init(); nat_init();
        wifi_status = 99; wifi_retry_at = now;
    }
    if (!settings.ssid[0]) {
        if (wifi_status != 98) log_puts("wifi: not configured, type: wifi SSID|PASSWORD|HOST");
        wifi_status = 98; wifi_state = "unconfigured";
        relay_configure(settings.host, false); nat_configure(false);
        wifi_retry_at = now + 10000;
        return;
    }
    cyw43_arch_lwip_begin();
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_lwip_end();
    if (wifi_attempt && status != CYW43_LINK_UP &&
        (uint32_t)(now - wifi_join_at) >= 30000) status = CYW43_LINK_FAIL;
    if (wifi_attempt || status == CYW43_LINK_UP) {
        if (status != wifi_status) {
            if (status == CYW43_LINK_UP) {
                cyw43_arch_lwip_begin();
                log_printf("wifi: joined %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
                cyw43_arch_lwip_end();
                wifi_state = "up";
            } else if (status < 0 || status == CYW43_LINK_DOWN) {
                log_printf("wifi: join failed (%d)\n", status);
                wifi_state = "failed";
                wifi_attempt = false; wifi_retry_at = now + 10000;
            } else {
                wifi_state = status == CYW43_LINK_NOIP ? "dhcp" : "joining";
                log_printf("wifi: %s\n", wifi_state);
            }
            wifi_status = status;
        }
    }
    if (nat_mode()) { relay_configure(settings.host, false); nat_configure(status == CYW43_LINK_UP); ctrl_set_forwarder(nat_forward_add, nat_forward_del); }
    else { nat_configure(false); ctrl_set_forwarder(NULL, NULL); relay_configure(settings.host, status == CYW43_LINK_UP); }
    if (!wifi_attempt && status != CYW43_LINK_UP && (int32_t)(now - wifi_retry_at) >= 0) {
        int e = cyw43_arch_wifi_connect_async(settings.ssid, settings.pass,
                    settings.pass[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
        wifi_retry_at = now + 10000;
        if (e) {
            log_printf("wifi: join failed (%d)\n", e); wifi_state = "failed";
        } else {
            wifi_attempt = true; wifi_join_at = now; wifi_status = CYW43_LINK_JOIN;
            wifi_state = "joining"; log_puts("wifi: joining");
        }
    }
}
/* `kbd map` shows the keyboard's controller buttons; `kbd map <button>
 * <key>` moves one and `kbd map reset` puts them all back, both saved. */
static void kbd_map_command(const char *args) {
    char button[8], key[HIDKEYS_NAME_MAX], text[160], extra;
    config_t next = settings;
    while (*args == ' ') ++args;
    if (!strcmp(args, "reset")) memset(next.pad_map, 0, sizeof(next.pad_map));
    else if (*args) {
        int b = -1, n = sscanf(args, "%7s %9s %c", button, key, &extra);
        uint8_t usage = n == 2 ? hidkeys_usage_parse(key) : 0;
        if (n != 2 || (b = hidpad_button_parse(button)) < 0 || !usage) {
            puts("kbd: map <B|Y|SELECT|START|UP|DOWN|LEFT|RIGHT|A|X|L|R> <key>, or map reset");
            return;
        }
        next.pad_map[b] = usage;
    }
    if (*args) {
        if (!config_save(&next)) { puts("kbd: flash save failed"); return; }
        settings = next; kbd_set_map(settings.pad_map);
    }
    kbd_map_text(text, sizeof(text));
    printf("kbd map: %s\n", text);
}
static void console_command(char *line) {
    if (btkbd_command(line)) return;
    if (!strcmp(line, "stats")) { print_stats(true); return; }
    if (!strcmp(line, "log") || !strcmp(line, "log on") || !strcmp(line, "log off")) {
        if (!strcmp(line, "log on")) log_set_enabled(true);
        else if (!strcmp(line, "log off")) log_set_enabled(false);
        puts(log_enabled() ? "log: on" : "log: off"); return;
    }
    if (!strncmp(line, "type ", 5)) {
        puts(kbd_type(line + 5) ? "kbd: sent" : "kbd: queue full");
        return;
    }
    if (!strcmp(line, "type")) { puts(kbd_type("") ? "kbd: sent" : "kbd: queue full"); return; }
    if (!strcmp(line, "kbd test on")) { kbd_test(true); puts("kbd: test pattern on"); return; }
    if (!strcmp(line, "kbd test off")) { kbd_test(false); puts("kbd: test pattern off"); return; }
    if (!strcmp(line, "kbd map") || !strncmp(line, "kbd map ", 8)) { kbd_map_command(line + 7); return; }
    if (!strcmp(line, "wifi?")) {
        printf("wifi: ssid=%s password=******** host=%s\n", settings.ssid, settings.host);
        return;
    }
    if (!strcmp(line, "mode")) { puts(nat_mode() ? "mode: nat" : "mode: relay"); return; }
    if (!strcmp(line, "mode nat")) {
        config_t next = settings; next.host[0] = '\0';
        if (!config_save(&next)) { puts("mode: flash save failed"); return; }
        settings = next; wifi_changed(); puts("mode: nat"); return;
    }
    if (!strncmp(line, "mode relay ", 11) && line[11] && strlen(line + 11) <= 63 && !strchr(line + 11, ' ')) {
        config_t next = settings; strcpy(next.host, line + 11);
        if (!config_save(&next)) { puts("mode: flash save failed"); return; }
        settings = next; wifi_changed(); puts("mode: relay"); return;
    }
    if (!strcmp(line, "wifi clear")) {
        if (!config_clear()) { puts("wifi: flash erase failed"); return; }
        memset(&settings, 0, sizeof(settings)); wifi_changed();
        puts("wifi: saved settings cleared"); return;
    }
    if (!strncmp(line, "wifi ", 5)) {
        char *ssid = line + 5, *pass = strchr(ssid, '|');
        char *host = pass ? strchr(pass + 1, '|') : NULL;
        if (pass && host) {
            *pass++ = '\0'; *host++ = '\0';
            if (*ssid && strlen(ssid) <= 32 && strlen(pass) <= 64 &&
                strlen(host) <= 63 && !strchr(host, '|') && !strchr(host, ' ')) {
                config_t next = {0};
                memcpy(next.pad_map, settings.pad_map, sizeof(next.pad_map));
                strcpy(next.ssid, ssid); strcpy(next.pass, pass); strcpy(next.host, host);
                if (!config_save(&next)) { puts("wifi: flash save failed"); return; }
                settings = next; wifi_changed(); puts("wifi: settings saved"); return;
            }
        }
    }
    puts("commands: wifi SSID|PASSWORD|HOST (empty HOST is NAT), wifi?, wifi clear, mode [nat|relay HOST], bt, bt scan, bt pair <n|addr>, bt forget, bt on|off, stats, log, log on|off, type <text>, kbd test on|off, kbd map [<button> <key>|reset]");
}
static void console_poll(void) {
    static char line[168];
    static size_t used;
    static bool overflow, was_cr;
    /* Bound input work so a busy console cannot starve the link. */
    for (unsigned n = 0; n < 32; ++n) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) break;
        if (c == '\n' && was_cr) { was_cr = false; continue; }
        was_cr = c == '\r';
        if (c == '\r' || c == '\n') {
            putchar('\n'); line[used] = '\0';
            if (overflow) puts("wifi: line too long; discarded");
            else if (used) console_command(line);
            used = 0; overflow = false;
        } else if (c == '\b' || c == 127) {
            if (used && !overflow) { --used; printf("\b \b"); }
        } else if (c >= 32 && c <= 255) {
            putchar(c);
            if (used < sizeof(line) - 1 && !overflow) line[used++] = (char)c;
            else overflow = true;
        }
    }
}
int main(void) {
    stdio_init_all();
    config_load(&settings);
    link_init();
    uint8_t rx[LINK_FRAME], tx[LINK_FRAME];
    link_next_out(tx); port_set_tx(tx); port_init(); kbd_init(); kbd_set_map(settings.pad_map);
    uint32_t stats_at = to_ms_since_boot(get_absolute_time()) + 2000;
    uint32_t last_valid_at = 0;
    for (;;) {
        bool moved = port_take_rx(rx);
        if (moved) {
            link_rx_t result = link_on_rx(rx);
            if (result.valid) { last_valid_at = to_ms_since_boot(get_absolute_time()); port_set_idle(false); }
            if (result.reset) { ctrl_reset(); relay_reset(); nat_reset(); kbd_link_restart(); log_puts("link: SNES restarted"); }
            for (size_t i = 0; i < result.count; ++i) {
                link_record_t *r = &result.records[i];
                if (r->ch == CH_CTRL) ctrl_bytes(r->data, r->len);
                else if (r->ch == CH_IP) { if (nat_mode()) nat_ip_bytes(r->data, r->len); else relay_ip_bytes(r->data, r->len); }
            }
        }
        kbd_poll(); btkbd_poll(); console_poll(); wifi_poll(); if (nat_mode()) nat_poll(); else relay_poll();
        if (moved) { link_next_out(tx); port_set_tx(tx); }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (last_valid_at && (int32_t)(now - last_valid_at) > 2000) { last_valid_at = 0; port_set_idle(true); }
        if ((int32_t)(now - stats_at) >= 0) {
            stats_at = now + 2000;
            print_stats(false);
        }
        tight_loop_contents();
    }
}
