#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "lanstat.h"
#include "web_assets.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HTTP_MAX_CLIENTS 16
#define HTTP_REQ_BUF_SIZE 2048
#define HTTP_TIMEOUT_MS 4000
#define SB_MAX_CAP (2 * 1024 * 1024)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} lanstat_sb_t;

typedef struct {
    int fd;
    bool writing;
    uint64_t last_active_ms;
    char req_buf[HTTP_REQ_BUF_SIZE];
    size_t req_len;
    char hdr_buf[512];
    size_t hdr_len;
    size_t hdr_sent;
    const uint8_t *body_ptr;
    size_t body_len;
    size_t body_sent;
    bool body_owned;
} http_client_t;

struct lanstat_http {
    int listen_fd;
    http_client_t clients[HTTP_MAX_CLIENTS];
};

static void sb_init(lanstat_sb_t *sb, size_t init_cap) {
    sb->cap = init_cap > 0 ? init_cap : 4096;
    sb->data = malloc(sb->cap);
    sb->len = 0;
    sb->failed = (sb->data == NULL);
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static bool sb_ensure(lanstat_sb_t *sb, size_t extra) {
    if (sb->failed) {
        return false;
    }
    if (sb->len + extra + 1 <= sb->cap) {
        return true;
    }
    size_t next_cap = sb->cap;
    while (next_cap < sb->len + extra + 1) {
        if (next_cap >= SB_MAX_CAP) {
            sb->failed = true;
            return false;
        }
        next_cap *= 2;
    }
    char *next = realloc(sb->data, next_cap);
    if (!next) {
        sb->failed = true;
        return false;
    }
    sb->data = next;
    sb->cap = next_cap;
    return true;
}

static void sb_append_n(lanstat_sb_t *sb, const char *str, size_t len) {
    if (!sb_ensure(sb, len)) {
        return;
    }
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

#define sb_append_lit(sb, lit) sb_append_n((sb), (lit), sizeof(lit) - 1)

static void sb_appendf(lanstat_sb_t *sb, const char *fmt, ...) {
    if (sb->failed) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        sb->failed = true;
        return;
    }
    if ((size_t)needed >= sb->cap - sb->len) {
        if (!sb_ensure(sb, (size_t)needed)) {
            va_end(args_copy);
            return;
        }
        vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args_copy);
    }
    va_end(args_copy);
    sb->len += (size_t)needed;
}

static void sb_append_json_str(lanstat_sb_t *sb, const char *str) {
    sb_append_lit(sb, "\"");
    if (str) {
        for (size_t idx = 0; str[idx] != '\0'; idx++) {
            unsigned char ch = (unsigned char)str[idx];
            if (ch == '"') {
                sb_append_lit(sb, "\\\"");
            } else if (ch == '\\') {
                sb_append_lit(sb, "\\\\");
            } else if (ch == '\b') {
                sb_append_lit(sb, "\\b");
            } else if (ch == '\f') {
                sb_append_lit(sb, "\\f");
            } else if (ch == '\n') {
                sb_append_lit(sb, "\\n");
            } else if (ch == '\r') {
                sb_append_lit(sb, "\\r");
            } else if (ch == '\t') {
                sb_append_lit(sb, "\\t");
            } else if (ch < 0x20 || ch == 0x7f) {
                sb_appendf(sb, "\\u%04x", ch);
            } else {
                sb_append_n(sb, (const char *)&ch, 1);
            }
        }
    }
    sb_append_lit(sb, "\"");
}

static void append_ring_json(lanstat_sb_t *sb, const lanstat_ring_t *ring, bool include_partial) {
    sb_append_lit(sb, "[");
    size_t start = (ring->count < ring->capacity) ? 0 : ring->head;
    bool first = true;
    for (size_t idx = 0; idx < ring->count; idx++) {
        size_t pos = (start + idx) % ring->capacity;
        if (!first) {
            sb_append_lit(sb, ",");
        }
        first = false;
        sb_appendf(sb, "[%" PRIu64 ",%" PRIu64 "]",
                   ring->samples[pos].rx_bps, ring->samples[pos].tx_bps);
    }
    if (include_partial && ring->accum_secs > 0) {
        uint64_t live_rx = (ring->accum_rx_bytes * 8ULL) / ring->accum_secs;
        uint64_t live_tx = (ring->accum_tx_bytes * 8ULL) / ring->accum_secs;
        if (!first) {
            sb_append_lit(sb, ",");
        }
        sb_appendf(sb, "[%" PRIu64 ",%" PRIu64 "]", live_rx, live_tx);
    }
    sb_append_lit(sb, "]");
}

static void append_ring_total_bytes(lanstat_sb_t *sb, const lanstat_ring_t *ring) {
    uint64_t rx_bytes = ring->accum_rx_bytes;
    uint64_t tx_bytes = ring->accum_tx_bytes;
    size_t start = (ring->count < ring->capacity) ? 0 : ring->head;
    for (size_t idx = 0; idx < ring->count; idx++) {
        size_t pos = (start + idx) % ring->capacity;
        rx_bytes += (ring->samples[pos].rx_bps * (uint64_t)ring->period_secs) / 8ULL;
        tx_bytes += (ring->samples[pos].tx_bps * (uint64_t)ring->period_secs) / 8ULL;
    }
    sb_appendf(sb, "[%" PRIu64 ",%" PRIu64 "]", rx_bytes, tx_bytes);
}

static int netmask_prefix_len(uint32_t mask_be) {
    uint32_t mask_host = ntohl(mask_be);
    int bits = 0;
    while (mask_host & 0x80000000U) {
        bits++;
        mask_host <<= 1;
    }
    return bits;
}

char *lanstat_build_stats_json(const lanstat_state_t *state,
                               const char *detail_ip,
                               uint64_t now_ms,
                               size_t *out_len) {
    lanstat_sb_t sb;
    sb_init(&sb, 8192);

    char subnet_str[48] = "-";
    char router_ip_str[INET_ADDRSTRLEN] = "";
    if (state->iface.has_ipv4) {
        struct in_addr router_addr = { .s_addr = state->iface.if_ipv4 };
        inet_ntop(AF_INET, &router_addr, router_ip_str, sizeof(router_ip_str));
        if (state->iface.if_netmask != 0) {
            struct in_addr net_addr = { .s_addr = state->iface.if_ipv4 & state->iface.if_netmask };
            char net_str[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &net_addr, net_str, sizeof(net_str))) {
                snprintf(subnet_str, sizeof(subnet_str), "%s/%d",
                         net_str, netmask_prefix_len(state->iface.if_netmask));
            }
        }
    }

    uint64_t uptime_secs = (now_ms >= state->start_mono_ms) ? (now_ms - state->start_mono_ms) / 1000ULL : 0;
    time_t now_sec = time(NULL);
    struct tm tm_now;
    char server_time[16] = "";
    if (localtime_r(&now_sec, &tm_now) != NULL) {
        strftime(server_time, sizeof(server_time), "%H:%M:%S", &tm_now);
    }

    sb_appendf(&sb, "{\"version\":");
    sb_append_json_str(&sb, LANSTAT_VERSION);
    sb_appendf(&sb, ",\"iface\":");
    sb_append_json_str(&sb, state->iface.iface);
    sb_appendf(&sb, ",\"subnet\":");
    sb_append_json_str(&sb, subnet_str);
    sb_appendf(&sb, ",\"router_ip\":");
    sb_append_json_str(&sb, router_ip_str);
    sb_appendf(&sb,
               ",\"uptime_secs\":%" PRIu64
               ",\"server_time\":\"%s\""
               ",\"capture_ok\":%s"
               ",\"ifaces\":[",
               uptime_secs,
               server_time,
               state->capture_failed ? "false" : "true");
    if (state->iface_list[0]) {
        const char *p = state->iface_list;
        bool first_if = true;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            if (len > 0 && len < 32) {
                char tmp_if[32];
                memcpy(tmp_if, p, len);
                tmp_if[len] = '\0';
                sb_appendf(&sb, "%s\"%s\"", first_if ? "" : ",", tmp_if);
                first_if = false;
            }
            if (!comma) {
                break;
            }
            p = comma + 1;
        }
    }
    sb_appendf(&sb,
               "]"
               ",\"rss_bytes\":%" PRIu64
               ",\"mem_total_bytes\":%" PRIu64
               ",\"cpu_pct_x100\":%d"
               ",\"total_rx_bytes\":%" PRIu64
               ",\"total_tx_bytes\":%" PRIu64
               ",\"total_lan_rx_bytes\":%" PRIu64
               ",\"total_lan_tx_bytes\":%" PRIu64
               ",\"total_rx_bps\":%" PRIu64
               ",\"total_tx_bps\":%" PRIu64
               ",\"total_lan_rx_bps\":%" PRIu64
               ",\"total_lan_tx_bps\":%" PRIu64
               ",\"raw_wire_bytes\":%" PRIu64
               ",\"raw_packets\":%" PRIu64
               ",\"kern_captured\":%" PRIu64
               ",\"kern_dropped\":%" PRIu64
               ",\"hosts\":[",
               state->rss_bytes,
               state->mem_total_bytes,
               state->last_sys_cpu_ticks > 0 ? (int)state->cpu_pct_x100 : -1,
               state->total_rx_bytes,
               state->total_tx_bytes,
               state->total_lan_rx_bytes,
               state->total_lan_tx_bytes,
               state->total_rx_bps,
               state->total_tx_bps,
               state->total_lan_rx_bps,
               state->total_lan_tx_bps,
               state->raw_wire_bytes,
               state->raw_packets,
               state->kern_captured,
               state->kern_dropped);

    bool first_host = true;
    for (size_t idx = 0; idx < state->max_hosts; idx++) {
        const lanstat_host_t *host = &state->hosts[idx];
        if (!host->used) {
            continue;
        }
        char ip_str[INET6_ADDRSTRLEN];
        char mac_str[20] = "";
        lanstat_format_ip(host, ip_str, sizeof(ip_str));
        if (host->has_mac) {
            lanstat_format_mac(host->mac, mac_str, sizeof(mac_str));
        }
        uint64_t last_seen = (now_ms >= host->last_seen_ms) ? (now_ms - host->last_seen_ms) / 1000ULL : 0;

        if (!first_host) {
            sb_append_lit(&sb, ",");
        }
        first_host = false;
        sb_appendf(&sb, "{\"ip\":");
        sb_append_json_str(&sb, ip_str);
        sb_appendf(&sb, ",\"mac\":");
        sb_append_json_str(&sb, mac_str);
        sb_appendf(&sb, ",\"name\":");
        sb_append_json_str(&sb, host->name);
        sb_appendf(&sb,
                   ",\"family\":%d"
                   ",\"lan\":%s"
                   ",\"router\":%s"
                   ",\"rx_bytes\":%" PRIu64
                   ",\"tx_bytes\":%" PRIu64
                   ",\"lan_rx_bytes\":%" PRIu64
                   ",\"lan_tx_bytes\":%" PRIu64
                   ",\"rx_bps\":%" PRIu64
                   ",\"tx_bps\":%" PRIu64
                   ",\"lan_rx_bps\":%" PRIu64
                   ",\"lan_tx_bps\":%" PRIu64
                   ",\"rx_pkts\":%" PRIu64
                   ",\"tx_pkts\":%" PRIu64
                   ",\"rx_pps\":%u"
                   ",\"tx_pps\":%u"
                   ",\"last_seen\":%" PRIu64 "}",
                   (host->family == AF_INET) ? 4 : 6,
                   host->is_lan ? "true" : "false",
                   host->is_router ? "true" : "false",
                   host->rx_bytes,
                   host->tx_bytes,
                   host->lan_rx_bytes,
                   host->lan_tx_bytes,
                   host->rx_bps,
                   host->tx_bps,
                   host->lan_rx_bps,
                   host->lan_tx_bps,
                   host->rx_pkts,
                   host->tx_pkts,
                   host->rx_pps,
                   host->tx_pps,
                   last_seen);
    }

    sb_append_lit(&sb, "],\"rings\":{\"sec\":");
    append_ring_json(&sb, &state->ring_sec, false);
    sb_append_lit(&sb, ",\"min\":");
    append_ring_json(&sb, &state->ring_min, true);
    sb_append_lit(&sb, ",\"hour\":");
    append_ring_json(&sb, &state->ring_hour, true);
    sb_append_lit(&sb, ",\"day\":");
    append_ring_json(&sb, &state->ring_day, true);
    sb_append_lit(&sb, "},\"ring_totals\":{\"sec\":");
    append_ring_total_bytes(&sb, &state->ring_sec);
    sb_append_lit(&sb, ",\"min\":");
    append_ring_total_bytes(&sb, &state->ring_min);
    sb_append_lit(&sb, ",\"hour\":");
    append_ring_total_bytes(&sb, &state->ring_hour);
    sb_append_lit(&sb, ",\"day\":");
    append_ring_total_bytes(&sb, &state->ring_day);
    sb_append_lit(&sb, "}");

    if (detail_ip && detail_ip[0] != '\0') {
        const lanstat_host_t *detail = lanstat_find_host_by_str(state, detail_ip);
        if (detail) {
            char norm_ip[INET6_ADDRSTRLEN];
            lanstat_format_ip(detail, norm_ip, sizeof(norm_ip));
            sb_appendf(&sb, ",\"detail\":{\"ip\":");
            sb_append_json_str(&sb, norm_ip);
            sb_append_lit(&sb, ",\"sec\":[");
            size_t cnt = state->host_hist_count;
            size_t start = (cnt < HIST_SEC_LEN) ? 0 : state->host_hist_head;
            for (size_t idx = 0; idx < cnt; idx++) {
                size_t pos = (start + idx) % HIST_SEC_LEN;
                if (idx > 0) {
                    sb_append_lit(&sb, ",");
                }
                sb_appendf(&sb, "[%" PRIu64 ",%" PRIu64 "]",
                           (uint64_t)detail->hist_rx_Bps[pos] * 8ULL,
                           (uint64_t)detail->hist_tx_Bps[pos] * 8ULL);
            }
            sb_append_lit(&sb, "]}");
        }
    }

    sb_append_lit(&sb, "}");

    if (sb.failed) {
        free(sb.data);
        *out_len = 0;
        return NULL;
    }
    *out_len = sb.len;
    return sb.data;
}

static char *build_graphs_xml(const lanstat_state_t *state, uint64_t now_ms, size_t *out_len) {
    lanstat_sb_t sb;
    sb_init(&sb, 2048);
    uint64_t uptime_secs = (now_ms >= state->start_mono_ms) ? (now_ms - state->start_mono_ms) / 1000ULL : 0;
    sb_appendf(&sb,
               "<graphs tp=\"%" PRIu64 "\" tb=\"%" PRIu64 "\" pc=\"%" PRIu64 "\" pd=\"%" PRIu64 "\" rf=\"%" PRIu64 " secs\">\n<seconds>\n",
               state->raw_packets,
               state->raw_wire_bytes,
               state->kern_captured,
               state->kern_dropped,
               uptime_secs);
    size_t start = (state->ring_sec.count < state->ring_sec.capacity) ? 0 : state->ring_sec.head;
    for (size_t idx = 0; idx < state->ring_sec.count; idx++) {
        size_t pos = (start + idx) % state->ring_sec.capacity;
        sb_appendf(&sb, "<e p=\"%zu\" i=\"%" PRIu64 "\" o=\"%" PRIu64 "\"/>\n",
                   idx,
                   state->ring_sec.samples[pos].rx_bps / 8ULL,
                   state->ring_sec.samples[pos].tx_bps / 8ULL);
    }
    sb_append_lit(&sb, "</seconds>\n</graphs>\n");
    if (sb.failed) {
        free(sb.data);
        *out_len = 0;
        return NULL;
    }
    *out_len = sb.len;
    return sb.data;
}

static bool set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_client(http_client_t *cli) {
    if (cli->fd >= 0) {
        close(cli->fd);
        cli->fd = -1;
    }
    if (cli->body_owned && cli->body_ptr) {
        free((void *)(uintptr_t)cli->body_ptr);
    }
    memset(cli, 0, sizeof(*cli));
    cli->fd = -1;
}

static bool strcasestr_ascii(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return true;
    }
    for (; *haystack; haystack++) {
        size_t idx = 0;
        while (idx < nlen && haystack[idx] != '\0' &&
               tolower((unsigned char)haystack[idx]) == tolower((unsigned char)needle[idx])) {
            idx++;
        }
        if (idx == nlen) {
            return true;
        }
    }
    return false;
}

static int hex_val(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void extract_query_param(const char *query, const char *key, char *out, size_t out_len) {
    out[0] = '\0';
    if (!query) {
        return;
    }
    size_t key_len = strlen(key);
    const char *cursor = query;
    while (*cursor) {
        const char *amp = strchr(cursor, '&');
        size_t seg_len = amp ? (size_t)(amp - cursor) : strlen(cursor);
        if (seg_len > key_len + 1 &&
            strncmp(cursor, key, key_len) == 0 &&
            cursor[key_len] == '=') {
            const char *val = cursor + key_len + 1;
            size_t val_len = seg_len - (key_len + 1);
            size_t out_pos = 0;
            for (size_t idx = 0; idx < val_len && out_pos + 1 < out_len; idx++) {
                if (val[idx] == '+') {
                    out[out_pos++] = ' ';
                } else if (val[idx] == '%' && idx + 2 < val_len) {
                    int hi = hex_val(val[idx + 1]);
                    int lo = hex_val(val[idx + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out[out_pos++] = (char)((hi << 4) | lo);
                        idx += 2;
                        continue;
                    }
                    out[out_pos++] = val[idx];
                } else {
                    out[out_pos++] = val[idx];
                }
            }
            out[out_pos] = '\0';
            return;
        }
        if (!amp) {
            break;
        }
        cursor = amp + 1;
    }
}

static void prepare_error_response(http_client_t *cli, int code, const char *reason, const char *msg) {
    size_t msg_len = strlen(msg);
    int hlen = snprintf(cli->hdr_buf, sizeof(cli->hdr_buf),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        code, reason, msg_len);
    cli->hdr_len = (hlen > 0) ? (size_t)hlen : 0;
    cli->body_ptr = (const uint8_t *)msg;
    cli->body_len = msg_len;
    cli->body_owned = false;
    cli->writing = true;
}

static bool client_accepts_gzip(const char *headers) {
    if (!headers) {
        return false;
    }
    const char *cursor = headers;
    while (*cursor) {
        const char *eol = strchr(cursor, '\n');
        size_t line_len = eol ? (size_t)(eol - cursor) : strlen(cursor);
        if (line_len > 16) {
            const char *prefix = "accept-encoding:";
            bool match = true;
            for (size_t idx = 0; idx < 16; idx++) {
                if (tolower((unsigned char)cursor[idx]) != prefix[idx]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                char line_buf[256];
                size_t copy_len = line_len < sizeof(line_buf) - 1 ? line_len : sizeof(line_buf) - 1;
                memcpy(line_buf, cursor, copy_len);
                line_buf[copy_len] = '\0';
                return strcasestr_ascii(line_buf, "gzip");
            }
        }
        if (!eol) {
            break;
        }
        cursor = eol + 1;
    }
    return false;
}

static void handle_request(http_client_t *cli, const lanstat_state_t *state, uint64_t now_ms) {
    cli->req_buf[cli->req_len] = '\0';
    char *line_end = strstr(cli->req_buf, "\r\n");
    if (!line_end) {
        line_end = strchr(cli->req_buf, '\n');
    }
    if (!line_end) {
        prepare_error_response(cli, 400, "Bad Request", "Bad Request\n");
        return;
    }
    *line_end = '\0';

    char method[16] = "";
    char target[512] = "";
    if (sscanf(cli->req_buf, "%15s %511s", method, target) != 2) {
        prepare_error_response(cli, 400, "Bad Request", "Bad Request\n");
        return;
    }
    bool is_head = (strcmp(method, "HEAD") == 0);
    bool is_post = (strcmp(method, "POST") == 0);
    if (strcmp(method, "GET") != 0 && !is_head && !(is_post && strncmp(target, "/api/iface", 10) == 0)) {
        prepare_error_response(cli, 405, "Method Not Allowed", "Method Not Allowed\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query) {
        *query++ = '\0';
    }

    const char *headers_part = line_end + 1;
    if (headers_part[0] == '\n') {
        headers_part++;
    }
    bool accepts_gzip = client_accepts_gzip(headers_part);

    if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
        const uint8_t *payload = accepts_gzip ? web_index_html_gz : web_index_html;
        size_t payload_len = accepts_gzip ? web_index_html_gz_len : web_index_html_len;
        int hlen = snprintf(cli->hdr_buf, sizeof(cli->hdr_buf),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "%s"
                            "Vary: Accept-Encoding\r\n"
                            "Cache-Control: no-cache\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            accepts_gzip ? "Content-Encoding: gzip\r\n" : "",
                            payload_len);
        cli->hdr_len = (hlen > 0) ? (size_t)hlen : 0;
        cli->body_ptr = is_head ? NULL : payload;
        cli->body_len = is_head ? 0 : payload_len;
        cli->body_owned = false;
        cli->writing = true;
        return;
    }

    if (strcmp(target, "/api/stats") == 0) {
        char detail_ip[64];
        extract_query_param(query, "detail", detail_ip, sizeof(detail_ip));
        size_t json_len = 0;
        char *json = lanstat_build_stats_json(state, detail_ip, now_ms, &json_len);
        if (!json) {
            prepare_error_response(cli, 500, "Internal Server Error", "OOM\n");
            return;
        }
        int hlen = snprintf(cli->hdr_buf, sizeof(cli->hdr_buf),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json; charset=utf-8\r\n"
                            "Cache-Control: no-store\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            json_len);
        cli->hdr_len = (hlen > 0) ? (size_t)hlen : 0;
        if (is_head) {
            free(json);
            cli->body_ptr = NULL;
            cli->body_len = 0;
            cli->body_owned = false;
        } else {
            cli->body_ptr = (const uint8_t *)json;
            cli->body_len = json_len;
            cli->body_owned = true;
        }
        cli->writing = true;
        return;
    }

    if (strcmp(target, "/graphs.xml") == 0) {
        size_t xml_len = 0;
        char *xml = build_graphs_xml(state, now_ms, &xml_len);
        if (!xml) {
            prepare_error_response(cli, 500, "Internal Server Error", "OOM\n");
            return;
        }
        int hlen = snprintf(cli->hdr_buf, sizeof(cli->hdr_buf),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/xml; charset=utf-8\r\n"
                            "Cache-Control: no-store\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            xml_len);
        cli->hdr_len = (hlen > 0) ? (size_t)hlen : 0;
        if (is_head) {
            free(xml);
            cli->body_ptr = NULL;
            cli->body_len = 0;
            cli->body_owned = false;
        } else {
            cli->body_ptr = (const uint8_t *)xml;
            cli->body_len = xml_len;
            cli->body_owned = true;
        }
        cli->writing = true;
        return;
    }

    if (strcmp(target, "/favicon.ico") == 0) {
        int hlen = snprintf(cli->hdr_buf, sizeof(cli->hdr_buf),
                            "HTTP/1.1 204 No Content\r\n"
                            "Cache-Control: max-age=86400\r\n"
                            "Connection: close\r\n\r\n");
        cli->hdr_len = (hlen > 0) ? (size_t)hlen : 0;
        cli->body_ptr = NULL;
        cli->body_len = 0;
        cli->body_owned = false;
        cli->writing = true;
        return;
    }

    if (is_post && strcmp(target, "/api/iface") == 0) {
        char new_iface[32] = "";
        extract_query_param(query, "name", new_iface, sizeof(new_iface));
        bool valid = new_iface[0] != '\0';
        for (size_t i = 0; valid && new_iface[i]; i++) {
            unsigned char ch = (unsigned char)new_iface[i];
            if (!(isalnum(ch) || ch == '_' || ch == '.' || ch == '-')) {
                valid = false;
            }
        }
        bool found = false;
        if (valid && state->iface_list[0]) {
            const char *p = state->iface_list;
            size_t nlen = strlen(new_iface);
            while (*p) {
                const char *comma = strchr(p, ',');
                size_t len = comma ? (size_t)(comma - p) : strlen(p);
                if (len == nlen && memcmp(p, new_iface, len) == 0) {
                    found = true;
                    break;
                }
                if (!comma) {
                    break;
                }
                p = comma + 1;
            }
        }
        if (!valid || !found) {
            prepare_error_response(cli, 400, "Bad Request", "{\"ok\":false,\"error\":\"invalid interface\"}\n");
            return;
        }
        lanstat_state_t *mut_state = (lanstat_state_t *)state;
        snprintf(mut_state->pending_iface, sizeof(mut_state->pending_iface), "%s", new_iface);
        mut_state->iface_switch_requested = true;
        char ok_body[96];
        int blen = snprintf(ok_body, sizeof(ok_body), "{\"ok\":true,\"iface\":\"%s\"}\n", new_iface);
        int hlen = snprintf(cli->hdr_buf, sizeof(cli->hdr_buf),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json; charset=utf-8\r\n"
                            "Cache-Control: no-store\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Content-Length: %d\r\n"
                            "Connection: close\r\n\r\n",
                            blen);
        cli->hdr_len = (hlen > 0) ? (size_t)hlen : 0;
        cli->body_ptr = malloc((size_t)blen);
        if (cli->body_ptr) {
            memcpy((void *)cli->body_ptr, ok_body, (size_t)blen);
            cli->body_len = (size_t)blen;
            cli->body_owned = true;
        }
        cli->writing = true;
        return;
    }

    prepare_error_response(cli, 404, "Not Found", "Not Found\n");
}

lanstat_http_t *lanstat_http_open(const char *bind_ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!bind_ip || bind_ip[0] == '\0' || strcmp(bind_ip, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        close(fd);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return NULL;
    }
    set_nonblock(fd);

    lanstat_http_t *http = calloc(1, sizeof(*http));
    if (!http) {
        close(fd);
        return NULL;
    }
    http->listen_fd = fd;
    for (size_t idx = 0; idx < HTTP_MAX_CLIENTS; idx++) {
        http->clients[idx].fd = -1;
    }
    return http;
}

void lanstat_http_close(lanstat_http_t *http) {
    if (!http) {
        return;
    }
    for (size_t idx = 0; idx < HTTP_MAX_CLIENTS; idx++) {
        close_client(&http->clients[idx]);
    }
    if (http->listen_fd >= 0) {
        close(http->listen_fd);
    }
    free(http);
}

int lanstat_http_prepare_poll(lanstat_http_t *http, struct pollfd *pfds, int max_pfds) {
    if (!http || max_pfds <= 0) {
        return 0;
    }
    int count = 0;
    pfds[count].fd = http->listen_fd;
    pfds[count].events = POLLIN;
    pfds[count].revents = 0;
    count++;

    for (size_t idx = 0; idx < HTTP_MAX_CLIENTS && count < max_pfds; idx++) {
        if (http->clients[idx].fd >= 0) {
            pfds[count].fd = http->clients[idx].fd;
            pfds[count].events = http->clients[idx].writing ? POLLOUT : POLLIN;
            pfds[count].revents = 0;
            count++;
        }
    }
    return count;
}

static void accept_clients(lanstat_http_t *http, uint64_t now_ms) {
    for (;;) {
        int client_fd = accept(http->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            break;
        }
        set_nonblock(client_fd);
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        size_t slot = SIZE_MAX;
        size_t oldest_slot = 0;
        uint64_t oldest_ts = UINT64_MAX;
        for (size_t idx = 0; idx < HTTP_MAX_CLIENTS; idx++) {
            if (http->clients[idx].fd < 0) {
                slot = idx;
                break;
            }
            if (http->clients[idx].last_active_ms < oldest_ts) {
                oldest_ts = http->clients[idx].last_active_ms;
                oldest_slot = idx;
            }
        }
        if (slot == SIZE_MAX) {
            close_client(&http->clients[oldest_slot]);
            slot = oldest_slot;
        }
        http->clients[slot].fd = client_fd;
        http->clients[slot].last_active_ms = now_ms;
    }
}

static void step_write(http_client_t *cli) {
    while (cli->hdr_sent < cli->hdr_len) {
        ssize_t sent = send(cli->fd, cli->hdr_buf + cli->hdr_sent, cli->hdr_len - cli->hdr_sent, 0);
        if (sent > 0) {
            cli->hdr_sent += (size_t)sent;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            close_client(cli);
            return;
        }
    }
    while (cli->body_sent < cli->body_len) {
        ssize_t sent = send(cli->fd, cli->body_ptr + cli->body_sent, cli->body_len - cli->body_sent, 0);
        if (sent > 0) {
            cli->body_sent += (size_t)sent;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            close_client(cli);
            return;
        }
    }
    close_client(cli);
}

void lanstat_http_step(lanstat_http_t *http,
                       lanstat_state_t *state,
                       const struct pollfd *pfds,
                       int nfds,
                       uint64_t now_ms) {
    if (!http) {
        return;
    }
    for (int p_idx = 0; p_idx < nfds; p_idx++) {
        if (pfds[p_idx].fd == http->listen_fd) {
            if (pfds[p_idx].revents & POLLIN) {
                accept_clients(http, now_ms);
            }
            continue;
        }

        http_client_t *cli = NULL;
        for (size_t c_idx = 0; c_idx < HTTP_MAX_CLIENTS; c_idx++) {
            if (http->clients[c_idx].fd == pfds[p_idx].fd) {
                cli = &http->clients[c_idx];
                break;
            }
        }
        if (!cli) {
            continue;
        }

        if (pfds[p_idx].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close_client(cli);
            continue;
        }

        if (!cli->writing && (pfds[p_idx].revents & POLLIN)) {
            size_t avail = sizeof(cli->req_buf) - 1 - cli->req_len;
            if (avail == 0) {
                prepare_error_response(cli, 431, "Request Header Fields Too Large", "Header too large\n");
                step_write(cli);
                continue;
            }
            ssize_t got = recv(cli->fd, cli->req_buf + cli->req_len, avail, 0);
            if (got > 0) {
                cli->req_len += (size_t)got;
                cli->req_buf[cli->req_len] = '\0';
                cli->last_active_ms = now_ms;
                if (strstr(cli->req_buf, "\r\n\r\n") || strstr(cli->req_buf, "\n\n")) {
                    handle_request(cli, state, now_ms);
                    step_write(cli);
                }
            } else if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_client(cli);
            }
        } else if (cli->writing && (pfds[p_idx].revents & POLLOUT)) {
            cli->last_active_ms = now_ms;
            step_write(cli);
        }
    }

    for (size_t idx = 0; idx < HTTP_MAX_CLIENTS; idx++) {
        if (http->clients[idx].fd >= 0 &&
            now_ms >= http->clients[idx].last_active_ms &&
            now_ms - http->clients[idx].last_active_ms > HTTP_TIMEOUT_MS) {
            close_client(&http->clients[idx]);
        }
    }
}
