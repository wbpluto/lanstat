#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "../src/lanstat.h"

#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const uint8_t ROUTER_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t CLIENT1_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x10};
static const uint8_t CLIENT2_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x20};
static const uint8_t MCAST_MAC[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};

static void init_test_state(lanstat_state_t *state, size_t max_hosts, bool track_external) {
    lanstat_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.iface, sizeof(cfg.iface), "br-lan");
    cfg.max_hosts = max_hosts;
    cfg.track_external = track_external;
    cfg.promisc = true;
    assert(lanstat_state_init(state, &cfg, 10000) == 0);

    state->iface.has_ipv4 = 1;
    inet_pton(AF_INET, "192.168.1.1", &state->iface.if_ipv4);
    inet_pton(AF_INET, "255.255.255.0", &state->iface.if_netmask);
    state->iface.has_mac = 1;
    memcpy(state->iface.router_mac, ROUTER_MAC, 6);
    snprintf(state->iface.router_hostname, sizeof(state->iface.router_hostname), "OpenWrt");
    state->iface.router_ip6_count = 1;
    inet_pton(AF_INET6, "fe80::1", state->iface.router_ip6s[0]);
}

static size_t build_ipv4_frame(uint8_t *buf,
                               const uint8_t dst_mac[6],
                               const uint8_t src_mac[6],
                               uint16_t vlan_tpid,
                               const char *src_ip,
                               const char *dst_ip) {
    size_t pos = 0;
    memcpy(buf + pos, dst_mac, 6);
    pos += 6;
    memcpy(buf + pos, src_mac, 6);
    pos += 6;
    if (vlan_tpid != 0) {
        buf[pos++] = (uint8_t)(vlan_tpid >> 8);
        buf[pos++] = (uint8_t)(vlan_tpid & 0xff);
        buf[pos++] = 0x00;
        buf[pos++] = 0x0a;
    }
    buf[pos++] = 0x08;
    buf[pos++] = 0x00;
    memset(buf + pos, 0, 20);
    buf[pos] = 0x45;
    assert(inet_pton(AF_INET, src_ip, buf + pos + 12) == 1);
    assert(inet_pton(AF_INET, dst_ip, buf + pos + 16) == 1);
    pos += 20;
    return pos;
}

static size_t build_ipv6_frame(uint8_t *buf,
                               const uint8_t dst_mac[6],
                               const uint8_t src_mac[6],
                               const char *src_ip6,
                               const char *dst_ip6) {
    size_t pos = 0;
    memcpy(buf + pos, dst_mac, 6);
    pos += 6;
    memcpy(buf + pos, src_mac, 6);
    pos += 6;
    buf[pos++] = 0x86;
    buf[pos++] = 0xdd;
    memset(buf + pos, 0, 40);
    buf[pos] = 0x60;
    assert(inet_pton(AF_INET6, src_ip6, buf + pos + 8) == 1);
    assert(inet_pton(AF_INET6, dst_ip6, buf + pos + 24) == 1);
    pos += 40;
    return pos;
}

static void test_ipv4_ipv6_and_rates(void) {
    lanstat_state_t state;
    init_test_state(&state, 64, false);

    uint8_t frame[128];
    size_t len = build_ipv4_frame(frame, CLIENT1_MAC, ROUTER_MAC, 0, "198.51.100.10", "192.168.1.20");
    lanstat_record_packet(&state, frame, (uint32_t)len, 1500, 10200);

    len = build_ipv4_frame(frame, ROUTER_MAC, CLIENT1_MAC, 0x8100, "192.168.1.20", "198.51.100.10");
    lanstat_record_packet(&state, frame, (uint32_t)len, 500, 10300);

    /* LAN-to-LAN transfer between .20 and .30 must count into lan_*_bytes without inflating WAN. */
    len = build_ipv4_frame(frame, CLIENT2_MAC, CLIENT1_MAC, 0, "192.168.1.20", "192.168.1.30");
    lanstat_record_packet(&state, frame, (uint32_t)len, 400, 10350);

    /* Router traffic should be isolated as router=true and excluded from global client totals. */
    len = build_ipv4_frame(frame, ROUTER_MAC, CLIENT2_MAC, 0, "192.168.1.224", "192.168.1.1");
    lanstat_record_packet(&state, frame, (uint32_t)len, 250, 10400);

    /* Multicast and subnet broadcast must not create fake host rows. */
    len = build_ipv4_frame(frame, MCAST_MAC, CLIENT1_MAC, 0, "192.168.1.20", "224.0.0.251");
    lanstat_record_packet(&state, frame, (uint32_t)len, 100, 10450);
    len = build_ipv4_frame(frame, MCAST_MAC, CLIENT1_MAC, 0, "192.168.1.20", "192.168.1.255");
    lanstat_record_packet(&state, frame, (uint32_t)len, 100, 10460);

    /* IPv6 client download & upload. */
    len = build_ipv6_frame(frame, CLIENT1_MAC, ROUTER_MAC, "2001:db8:1::10", "2001:db8::20");
    lanstat_record_packet(&state, frame, (uint32_t)len, 1000, 10500);

    /* Truncated / malformed frames should be safely ignored without crashing. */
    lanstat_record_packet(&state, frame, 5, 5, 10550);
    lanstat_record_packet(&state, NULL, 0, 0, 10560);

    const lanstat_host_t *client1 = lanstat_find_host_by_str(&state, "192.168.1.20");
    assert(client1 != NULL);
    assert(client1->rx_bytes == 1500);
    assert(client1->tx_bytes == 1100);
    assert(client1->lan_rx_bytes == 0);
    assert(client1->lan_tx_bytes == 400);
    assert(client1->rx_pkts == 1);
    assert(client1->tx_pkts == 4);
    assert(client1->is_lan == 1 && client1->is_router == 0);

    const lanstat_host_t *lan_peer = lanstat_find_host_by_str(&state, "192.168.1.30");
    assert(lan_peer != NULL);
    assert(lan_peer->rx_bytes == 400);
    assert(lan_peer->lan_rx_bytes == 400);
    assert(state.total_lan_rx_bytes == 400);
    assert(state.total_lan_tx_bytes == 650);

    const lanstat_host_t *router = lanstat_find_host_by_str(&state, "192.168.1.1");
    assert(router != NULL);
    assert(router->is_router == 1);
    assert(router->rx_bytes == 250);

    assert(lanstat_find_host_by_str(&state, "198.51.100.10") == NULL);
    assert(lanstat_find_host_by_str(&state, "224.0.0.251") == NULL);
    assert(lanstat_find_host_by_str(&state, "192.168.1.255") == NULL);

    const lanstat_host_t *client1_v6 = lanstat_find_host_by_str(&state, "2001:db8::20");
    assert(client1_v6 != NULL);
    assert(client1_v6->rx_bytes == 1000);
    assert(client1_v6->is_lan == 1 && client1_v6->is_router == 0);

    lanstat_tick(&state, 11000);
    /* 1500 + 400 + 1000 = 2900 bytes in 1.0s => 23200 bps */
    assert(state.total_rx_bps == 23200);
    /* 1100 + 250 = 1350 bytes in 1.0s => 10800 bps */
    assert(state.total_tx_bps == 10800);
    assert(state.total_lan_rx_bps == 3200);
    assert(state.total_lan_tx_bps == 5200);
    assert(state.ring_sec.count == 1);
    assert(client1->rx_bps == 12000);
    assert(client1->tx_bps == 8800);
    assert(client1->lan_tx_bps == 3200);
    assert(client1->rx_pps == 1);
    assert(client1->tx_pps == 4);

    /* Advance 59 more seconds to verify minute ring rollup. */
    for (uint64_t sec = 2; sec <= 60; sec++) {
        lanstat_tick(&state, 10000 + sec * 1000);
    }
    assert(state.ring_sec.count == 60);
    assert(state.ring_min.count == 1);

    lanstat_state_free(&state);
    printf("PASS test_ipv4_ipv6_and_rates\n");
}

static void test_names_eviction_and_json(void) {
    char leases_path[] = "/tmp/lanstat_leases_XXXXXX";
    char hosts_path[] = "/tmp/lanstat_hosts_XXXXXX";
    int lfd = mkstemp(leases_path);
    int hfd = mkstemp(hosts_path);
    assert(lfd >= 0 && hfd >= 0);

    const char *leases_txt =
        "1795500000 02:00:00:00:00:10 192.168.1.20 Media\"Box\\Lab *\n"
        "1795500000 02:00:00:00:00:20 192.168.1.224 * *\n";
    const char *hosts_txt =
        "127.0.0.1 localhost\n"
        "192.168.1.1 router.lan\n";
    assert(write(lfd, leases_txt, strlen(leases_txt)) == (ssize_t)strlen(leases_txt));
    assert(write(hfd, hosts_txt, strlen(hosts_txt)) == (ssize_t)strlen(hosts_txt));
    close(lfd);
    close(hfd);

    lanstat_state_t state;
    init_test_state(&state, 16, false);
    lanstat_refresh_names(&state, leases_path, hosts_path, NULL, true);

    uint8_t frame[128];
    /* Record router + 18 distinct LAN IPs to trigger eviction while keeping max_hosts=16. */
    size_t len = build_ipv4_frame(frame, ROUTER_MAC, CLIENT2_MAC, 0, "192.168.1.224", "192.168.1.1");
    lanstat_record_packet(&state, frame, (uint32_t)len, 128, 10050);

    for (int idx = 10; idx < 28; idx++) {
        char ipbuf[32];
        snprintf(ipbuf, sizeof(ipbuf), "192.168.1.%d", idx);
        len = build_ipv4_frame(frame, ROUTER_MAC, CLIENT1_MAC, 0, ipbuf, "198.51.100.10");
        lanstat_record_packet(&state, frame, (uint32_t)len, 256, 10100 + (uint64_t)idx * 10);
    }
    assert(state.host_count == 16);
    /* Router must never be evicted. */
    assert(lanstat_find_host_by_str(&state, "192.168.1.1") != NULL);
    /* Host .20 should survive and have escaped name from leases. */
    const lanstat_host_t *client1 = lanstat_find_host_by_str(&state, "192.168.1.20");
    assert(client1 != NULL);
    assert(strcmp(client1->name, "Media\"Box\\Lab") == 0);

    /* IPv6 from same MAC inherits hostname from MAC entry. */
    len = build_ipv6_frame(frame, ROUTER_MAC, CLIENT1_MAC, "fe80::1234", "2001:db8:1::1");
    lanstat_record_packet(&state, frame, (uint32_t)len, 300, 10500);
    const lanstat_host_t *client1_v6 = lanstat_find_host_by_str(&state, "fe80::1234");
    assert(client1_v6 != NULL);
    assert(strcmp(client1_v6->name, "Media\"Box\\Lab") == 0);

    lanstat_tick(&state, 11000);
    size_t json_len = 0;
    char *json = lanstat_build_stats_json(&state, "192.168.1.20", 11000, &json_len);
    assert(json != NULL && json_len > 100);
    assert(strstr(json, "Media\\\"Box\\\\Lab") != NULL);
    assert(strstr(json, "\"detail\":{\"ip\":\"192.168.1.20\",\"sec\":[[") != NULL);
    assert(strstr(json, "\"subnet\":\"192.168.1.0/24\"") != NULL);
    free(json);

    lanstat_state_free(&state);
    unlink(leases_path);
    unlink(hosts_path);
    printf("PASS test_names_eviction_and_json\n");
}

static void http_roundtrip(uint16_t port,
                           const char *req,
                           lanstat_http_t *http,
                           lanstat_state_t *state,
                           char *resp_out,
                           size_t resp_cap) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(send(sock, req, strlen(req), 0) == (ssize_t)strlen(req));

    size_t total = 0;
    for (int iter = 0; iter < 50; iter++) {
        struct pollfd pfds[32];
        int nfds = lanstat_http_prepare_poll(http, pfds, 32);
        int ready = poll(pfds, (nfds_t)nfds, 10);
        if (ready > 0) {
            lanstat_http_step(http, state, pfds, nfds, 12000);
        }
        struct pollfd cfd = { .fd = sock, .events = POLLIN };
        if (poll(&cfd, 1, 5) > 0) {
            ssize_t got = recv(sock, resp_out + total, resp_cap - 1 - total, 0);
            if (got > 0) {
                total += (size_t)got;
            } else if (got == 0) {
                break;
            }
        }
    }
    resp_out[total] = '\0';
    close(sock);
}

static void test_http_server(void) {
    lanstat_state_t state;
    init_test_state(&state, 32, false);
    uint8_t frame[128];
    size_t len = build_ipv4_frame(frame, CLIENT1_MAC, ROUTER_MAC, 0, "198.51.100.10", "192.168.1.20");
    lanstat_record_packet(&state, frame, (uint32_t)len, 1024, 10500);
    lanstat_tick(&state, 11000);

    lanstat_http_t *http = lanstat_http_open("127.0.0.1", 18667);
    assert(http != NULL);

    char resp[65536];
    http_roundtrip(18667,
                   "GET / HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n",
                   http, &state, resp, sizeof(resp));
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "Content-Encoding: gzip") != NULL);

    http_roundtrip(18667,
                   "GET /api/stats?detail=192.168.1.20 HTTP/1.1\r\nHost: localhost\r\n\r\n",
                   http, &state, resp, sizeof(resp));
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "\"total_rx_bps\":8192") != NULL);
    assert(strstr(resp, "\"total_lan_rx_bps\":0") != NULL);
    assert(strstr(resp, "\"rx_pps\":1") != NULL);
    assert(strstr(resp, "\"ring_totals\":{\"sec\":[") != NULL);
    assert(strstr(resp, "\"detail\":{\"ip\":\"192.168.1.20\"") != NULL);

    http_roundtrip(18667,
                   "GET /graphs.xml HTTP/1.1\r\nHost: localhost\r\n\r\n",
                   http, &state, resp, sizeof(resp));
    assert(strstr(resp, "<graphs ") != NULL);

    http_roundtrip(18667,
                   "POST /api/stats HTTP/1.1\r\nHost: localhost\r\n\r\n",
                   http, &state, resp, sizeof(resp));
    assert(strstr(resp, "405 Method Not Allowed") != NULL);

    lanstat_http_close(http);
    lanstat_state_free(&state);
    printf("PASS test_http_server\n");
}

int main(void) {
    test_ipv4_ipv6_and_rates();
    test_names_eviction_and_json();
    test_http_server();
    printf("ALL TESTS PASSED\n");
    return 0;
}
