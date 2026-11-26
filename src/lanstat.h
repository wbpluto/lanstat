#ifndef LANSTAT_H
#define LANSTAT_H

#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LANSTAT_VERSION "1.0.1"

#define HIST_SEC_LEN 60
#define HIST_MIN_LEN 60
#define HIST_HOUR_LEN 24
#define HIST_DAY_LEN 31

#define HOST_NAME_MAX_LEN 64
#define MAX_ROUTER_IPV6 16
#define MAX_NAME_ENTRIES 1024
#define DEFAULT_MAX_HOSTS 512
#define MAX_HOSTS_CAP 4096

#define IFACE_NAME_MAX 32

typedef struct {
    uint64_t rx_bps;
    uint64_t tx_bps;
} lanstat_rate_sample_t;

typedef struct {
    uint8_t used;
    uint8_t family;
    uint8_t is_lan;
    uint8_t is_router;
    uint8_t has_mac;
    uint8_t addr[16];
    uint8_t mac[6];
    char name[HOST_NAME_MAX_LEN];
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t prev_rx_bytes;
    uint64_t prev_tx_bytes;
    uint64_t lan_rx_bytes;
    uint64_t lan_tx_bytes;
    uint64_t prev_lan_rx_bytes;
    uint64_t prev_lan_tx_bytes;
    uint64_t rx_pkts;
    uint64_t tx_pkts;
    uint64_t prev_rx_pkts;
    uint64_t prev_tx_pkts;
    uint64_t rx_bps;
    uint64_t tx_bps;
    uint64_t lan_rx_bps;
    uint64_t lan_tx_bps;
    uint32_t rx_pps;
    uint32_t tx_pps;
    uint64_t last_seen_ms;
    uint32_t hist_rx_Bps[HIST_SEC_LEN];
    uint32_t hist_tx_Bps[HIST_SEC_LEN];
} lanstat_host_t;

typedef struct {
    uint8_t has_ip;
    uint8_t family;
    uint8_t addr[16];
    uint8_t has_mac;
    uint8_t mac[6];
    char name[HOST_NAME_MAX_LEN];
} lanstat_name_entry_t;

typedef struct {
    lanstat_rate_sample_t samples[64];
    size_t capacity;
    size_t head;
    size_t count;
    uint64_t accum_rx_bytes;
    uint64_t accum_tx_bytes;
    uint32_t accum_secs;
    uint32_t period_secs;
} lanstat_ring_t;

typedef struct {
    char iface[IFACE_NAME_MAX];
    uint32_t if_ipv4;
    uint32_t if_netmask;
    uint8_t has_ipv4;
    uint8_t has_mac;
    uint8_t router_mac[6];
    uint8_t router_ip6s[MAX_ROUTER_IPV6][16];
    size_t router_ip6_count;
    char router_hostname[HOST_NAME_MAX_LEN];
} lanstat_iface_info_t;

typedef struct {
    lanstat_iface_info_t iface;
    bool track_external;
    bool promisc;
    size_t max_hosts;
    size_t host_count;
    size_t hash_cap;
    lanstat_host_t *hosts;
    int32_t *hash_slots;

    uint64_t start_mono_ms;
    uint64_t last_tick_ms;
    uint64_t total_rx_bytes;
    uint64_t total_tx_bytes;
    uint64_t prev_total_rx_bytes;
    uint64_t prev_total_tx_bytes;
    uint64_t total_lan_rx_bytes;
    uint64_t total_lan_tx_bytes;
    uint64_t prev_total_lan_rx_bytes;
    uint64_t prev_total_lan_tx_bytes;
    uint64_t total_rx_bps;
    uint64_t total_tx_bps;
    uint64_t total_lan_rx_bps;
    uint64_t total_lan_tx_bps;
    uint64_t raw_wire_bytes;
    uint64_t raw_packets;
    uint64_t kern_captured;
    uint64_t kern_dropped;
    uint64_t rss_bytes;
    uint64_t mem_total_bytes;
    uint32_t cpu_pct_x100;
    uint64_t last_proc_cpu_ticks;
    uint64_t last_sys_cpu_ticks;
    bool capture_failed;
    char iface_list[512];
    char pending_iface[32];
    bool iface_switch_requested;

    size_t host_hist_head;
    size_t host_hist_count;

    lanstat_ring_t ring_sec;
    lanstat_ring_t ring_min;
    lanstat_ring_t ring_hour;
    lanstat_ring_t ring_day;

    lanstat_name_entry_t names[MAX_NAME_ENTRIES];
    size_t name_count;
    long long leases_mtime;
    long long hosts_mtime;
    long long ethers_mtime;
    uint64_t last_name_check_ms;
    uint64_t last_iface_check_ms;
} lanstat_state_t;

typedef struct {
    char iface[IFACE_NAME_MAX];
    char bind_ip[64];
    uint16_t port;
    bool promisc;
    bool track_external;
    bool daemonize;
    size_t max_hosts;
    size_t ring_kb;
    const char *leases_path;
    const char *hosts_path;
    const char *ethers_path;
} lanstat_config_t;

int lanstat_state_init(lanstat_state_t *state, const lanstat_config_t *cfg, uint64_t now_ms);
void lanstat_state_free(lanstat_state_t *state);

void lanstat_record_packet(lanstat_state_t *state,
                           const uint8_t *frame,
                           uint32_t snap_len,
                           uint32_t wire_len,
                           uint64_t now_ms);

void lanstat_tick(lanstat_state_t *state, uint64_t now_ms);
void lanstat_refresh_ifaces(lanstat_state_t *state);
void lanstat_reset_stats(lanstat_state_t *state, const char *new_iface, uint64_t now_ms);

void lanstat_refresh_names(lanstat_state_t *state,
                           const char *leases_path,
                           const char *hosts_path,
                           const char *ethers_path,
                           bool force);

void lanstat_refresh_iface(lanstat_state_t *state);

const lanstat_host_t *lanstat_find_host_by_str(const lanstat_state_t *state, const char *ip_str);
void lanstat_format_ip(const lanstat_host_t *host, char *buf, size_t buf_len);
void lanstat_format_mac(const uint8_t mac[6], char *buf, size_t buf_len);
char *lanstat_build_stats_json(const lanstat_state_t *state,
                               const char *detail_ip,
                               uint64_t now_ms,
                               size_t *out_len);

typedef struct lanstat_capture lanstat_capture_t;

lanstat_capture_t *lanstat_capture_open(const char *iface, bool promisc, size_t ring_kb);
void lanstat_capture_close(lanstat_capture_t *cap);
int lanstat_capture_fd(const lanstat_capture_t *cap);
size_t lanstat_capture_drain(lanstat_capture_t *cap,
                             lanstat_state_t *state,
                             size_t budget,
                             uint64_t now_ms);
void lanstat_capture_poll_stats(lanstat_capture_t *cap, lanstat_state_t *state);

typedef struct lanstat_http lanstat_http_t;

lanstat_http_t *lanstat_http_open(const char *bind_ip, uint16_t port);
void lanstat_http_close(lanstat_http_t *http);
int lanstat_http_prepare_poll(lanstat_http_t *http, struct pollfd *pfds, int max_pfds);
void lanstat_http_step(lanstat_http_t *http,
                       lanstat_state_t *state,
                       const struct pollfd *pfds,
                       int nfds,
                       uint64_t now_ms);

#endif
