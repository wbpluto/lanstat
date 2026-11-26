#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "lanstat.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t next_pow2(size_t val) {
    size_t pow2 = 16;
    while (pow2 < val) {
        pow2 <<= 1;
    }
    return pow2;
}

static uint64_t hash_host_key(uint8_t family, const uint8_t addr[16]) {
    size_t len = (family == AF_INET) ? 4 : 16;
    uint64_t hash = 14695981039346656037ULL ^ family;
    for (size_t idx = 0; idx < len; idx++) {
        hash ^= addr[idx];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool host_key_eq(const lanstat_host_t *host, uint8_t family, const uint8_t addr[16]) {
    if (host->family != family) {
        return false;
    }
    size_t len = (family == AF_INET) ? 4 : 16;
    return memcmp(host->addr, addr, len) == 0;
}

static void rehash_hosts(lanstat_state_t *state) {
    for (size_t slot = 0; slot < state->hash_cap; slot++) {
        state->hash_slots[slot] = -1;
    }
    size_t mask = state->hash_cap - 1;
    for (size_t idx = 0; idx < state->max_hosts; idx++) {
        if (!state->hosts[idx].used) {
            continue;
        }
        uint64_t hash = hash_host_key(state->hosts[idx].family, state->hosts[idx].addr);
        size_t pos = (size_t)hash & mask;
        while (state->hash_slots[pos] >= 0) {
            pos = (pos + 1) & mask;
        }
        state->hash_slots[pos] = (int32_t)idx;
    }
}

static void ring_init(lanstat_ring_t *ring, size_t capacity, uint32_t period_secs) {
    memset(ring, 0, sizeof(*ring));
    ring->capacity = capacity;
    ring->period_secs = period_secs;
}

static void ring_push(lanstat_ring_t *ring, uint64_t rx_bps, uint64_t tx_bps) {
    ring->samples[ring->head].rx_bps = rx_bps;
    ring->samples[ring->head].tx_bps = tx_bps;
    ring->head = (ring->head + 1) % ring->capacity;
    if (ring->count < ring->capacity) {
        ring->count++;
    }
}

static void ring_accumulate(lanstat_ring_t *ring, uint64_t rx_delta, uint64_t tx_delta, uint32_t elapsed_secs) {
    ring->accum_rx_bytes += rx_delta;
    ring->accum_tx_bytes += tx_delta;
    ring->accum_secs += elapsed_secs;
    if (ring->accum_secs >= ring->period_secs) {
        uint64_t rx_bps = (ring->accum_rx_bytes * 8ULL) / ring->accum_secs;
        uint64_t tx_bps = (ring->accum_tx_bytes * 8ULL) / ring->accum_secs;
        ring_push(ring, rx_bps, tx_bps);
        ring->accum_rx_bytes = 0;
        ring->accum_tx_bytes = 0;
        ring->accum_secs = 0;
    }
}

static void copy_clean_name(char *dst, size_t dst_len, const char *src) {
    if (!src || dst_len == 0) {
        return;
    }
    size_t out = 0;
    for (size_t idx = 0; src[idx] != '\0' && out + 1 < dst_len; idx++) {
        unsigned char ch = (unsigned char)src[idx];
        if (ch >= 0x20 && ch != 0x7f) {
            dst[out++] = (char)ch;
        }
    }
    dst[out] = '\0';
}

static bool parse_mac_str(const char *str, uint8_t out[6]) {
    unsigned int vals[6];
    if (!str) {
        return false;
    }
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
        return false;
    }
    for (size_t idx = 0; idx < 6; idx++) {
        out[idx] = (uint8_t)vals[idx];
    }
    return true;
}

static bool parse_ip_str(const char *str, uint8_t *family_out, uint8_t addr_out[16]) {
    if (!str) {
        return false;
    }
    memset(addr_out, 0, 16);
    if (inet_pton(AF_INET, str, addr_out) == 1) {
        *family_out = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, str, addr_out) == 1) {
        *family_out = AF_INET6;
        return true;
    }
    return false;
}

static void lookup_host_name(const lanstat_state_t *state, lanstat_host_t *host) {
    size_t ip_len = (host->family == AF_INET) ? 4 : 16;
    for (size_t idx = 0; idx < state->name_count; idx++) {
        const lanstat_name_entry_t *entry = &state->names[idx];
        if (entry->has_ip && entry->family == host->family &&
            memcmp(entry->addr, host->addr, ip_len) == 0) {
            copy_clean_name(host->name, sizeof(host->name), entry->name);
            return;
        }
    }
    if (host->has_mac) {
        for (size_t idx = 0; idx < state->name_count; idx++) {
            const lanstat_name_entry_t *entry = &state->names[idx];
            if (entry->has_mac && memcmp(entry->mac, host->mac, 6) == 0) {
                copy_clean_name(host->name, sizeof(host->name), entry->name);
                return;
            }
        }
    }
    if (host->is_router && state->iface.router_hostname[0] != '\0') {
        copy_clean_name(host->name, sizeof(host->name), state->iface.router_hostname);
    }
}

static void add_name_entry(lanstat_state_t *state,
                           bool has_ip,
                           uint8_t family,
                           const uint8_t addr[16],
                           bool has_mac,
                           const uint8_t mac[6],
                           const char *name) {
    if (!name || name[0] == '\0' || strcmp(name, "*") == 0 || state->name_count >= MAX_NAME_ENTRIES) {
        return;
    }
    lanstat_name_entry_t *entry = &state->names[state->name_count++];
    memset(entry, 0, sizeof(*entry));
    entry->has_ip = has_ip ? 1 : 0;
    if (has_ip) {
        entry->family = family;
        memcpy(entry->addr, addr, (family == AF_INET) ? 4 : 16);
    }
    entry->has_mac = has_mac ? 1 : 0;
    if (has_mac) {
        memcpy(entry->mac, mac, 6);
    }
    copy_clean_name(entry->name, sizeof(entry->name), name);
}

static void load_leases_file(lanstat_state_t *state, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *expiry = strtok(line, " \t\r\n");
        char *mac_str = strtok(NULL, " \t\r\n");
        char *ip_str = strtok(NULL, " \t\r\n");
        char *hostname = strtok(NULL, " \t\r\n");
        (void)expiry;
        if (!mac_str || !ip_str || !hostname || strcmp(hostname, "*") == 0) {
            continue;
        }
        uint8_t mac[6];
        uint8_t family = 0;
        uint8_t addr[16];
        bool has_mac = parse_mac_str(mac_str, mac);
        bool has_ip = parse_ip_str(ip_str, &family, addr);
        if (has_ip || has_mac) {
            add_name_entry(state, has_ip, family, addr, has_mac, mac, hostname);
        }
    }
    fclose(fp);
}

static void load_hosts_file(lanstat_state_t *state, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *ip_str = strtok(line, " \t\r\n");
        char *hostname = strtok(NULL, " \t\r\n");
        if (!ip_str || !hostname) {
            continue;
        }
        if (strcmp(hostname, "localhost") == 0 ||
            strcmp(ip_str, "127.0.0.1") == 0 ||
            strcmp(ip_str, "::1") == 0) {
            continue;
        }
        uint8_t family = 0;
        uint8_t addr[16];
        if (parse_ip_str(ip_str, &family, addr)) {
            add_name_entry(state, true, family, addr, false, NULL, hostname);
        }
    }
    fclose(fp);
}

static void load_ethers_file(lanstat_state_t *state, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *mac_str = strtok(line, " \t\r\n");
        char *name = strtok(NULL, " \t\r\n");
        if (!mac_str || !name) {
            continue;
        }
        uint8_t mac[6];
        if (parse_mac_str(mac_str, mac)) {
            add_name_entry(state, false, 0, NULL, true, mac, name);
        }
    }
    fclose(fp);
}

static long long file_mtime(const char *path) {
    if (!path) {
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long long)st.st_mtime;
}

void lanstat_refresh_names(lanstat_state_t *state,
                           const char *leases_path,
                           const char *hosts_path,
                           const char *ethers_path,
                           bool force) {
    long long m_leases = file_mtime(leases_path);
    long long m_hosts = file_mtime(hosts_path);
    long long m_ethers = file_mtime(ethers_path);

    if (!force &&
        m_leases == state->leases_mtime &&
        m_hosts == state->hosts_mtime &&
        m_ethers == state->ethers_mtime) {
        return;
    }

    state->leases_mtime = m_leases;
    state->hosts_mtime = m_hosts;
    state->ethers_mtime = m_ethers;
    state->name_count = 0;

    if (leases_path) {
        load_leases_file(state, leases_path);
    }
    if (hosts_path) {
        load_hosts_file(state, hosts_path);
    }
    if (ethers_path) {
        load_ethers_file(state, ethers_path);
    }

    for (size_t idx = 0; idx < state->max_hosts; idx++) {
        if (state->hosts[idx].used) {
            lookup_host_name(state, &state->hosts[idx]);
        }
    }
}

void lanstat_refresh_iface(lanstat_state_t *state) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", state->iface.iface);

        if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
            state->iface.if_ipv4 = sin->sin_addr.s_addr;
            state->iface.has_ipv4 = 1;
        }
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", state->iface.iface);
        if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
            state->iface.if_netmask = sin->sin_addr.s_addr;
        }
#ifdef SIOCGIFHWADDR
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", state->iface.iface);
        if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
            memcpy(state->iface.router_mac, ifr.ifr_hwaddr.sa_data, 6);
            state->iface.has_mac = 1;
        }
#endif
        close(sock);
    }

    if (gethostname(state->iface.router_hostname, sizeof(state->iface.router_hostname)) != 0) {
        state->iface.router_hostname[0] = '\0';
    }
    state->iface.router_hostname[HOST_NAME_MAX_LEN - 1] = '\0';

    state->iface.router_ip6_count = 0;
    FILE *fp = fopen("/proc/net/if_inet6", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp) && state->iface.router_ip6_count < MAX_ROUTER_IPV6) {
            char hex[40];
            char ifname[64];
            if (sscanf(line, "%32s %*x %*x %*x %*x %63s", hex, ifname) == 2) {
                if (strcmp(ifname, state->iface.iface) == 0 && strlen(hex) == 32) {
                    uint8_t ip6[16];
                    bool ok = true;
                    for (size_t idx = 0; idx < 16; idx++) {
                        unsigned int byte_val = 0;
                        if (sscanf(hex + idx * 2, "%2x", &byte_val) != 1) {
                            ok = false;
                            break;
                        }
                        ip6[idx] = (uint8_t)byte_val;
                    }
                    if (ok) {
                        memcpy(state->iface.router_ip6s[state->iface.router_ip6_count++], ip6, 16);
                    }
                }
            }
        }
        fclose(fp);
    }
}

int lanstat_state_init(lanstat_state_t *state, const lanstat_config_t *cfg, uint64_t now_ms) {
    memset(state, 0, sizeof(*state));
    snprintf(state->iface.iface, sizeof(state->iface.iface), "%s", cfg->iface);
    state->track_external = cfg->track_external;
    state->promisc = cfg->promisc;
    state->max_hosts = cfg->max_hosts;
    if (state->max_hosts < 16) {
        state->max_hosts = 16;
    }
    if (state->max_hosts > MAX_HOSTS_CAP) {
        state->max_hosts = MAX_HOSTS_CAP;
    }
    state->hash_cap = next_pow2(state->max_hosts * 4);

    state->hosts = calloc(state->max_hosts, sizeof(lanstat_host_t));
    state->hash_slots = malloc(state->hash_cap * sizeof(int32_t));
    if (!state->hosts || !state->hash_slots) {
        free(state->hosts);
        free(state->hash_slots);
        return -1;
    }
    for (size_t idx = 0; idx < state->hash_cap; idx++) {
        state->hash_slots[idx] = -1;
    }

    state->start_mono_ms = now_ms;
    state->last_tick_ms = now_ms;
    state->leases_mtime = -1;
    state->hosts_mtime = -1;
    state->ethers_mtime = -1;

    ring_init(&state->ring_sec, HIST_SEC_LEN, 1);
    ring_init(&state->ring_min, HIST_MIN_LEN, 60);
    ring_init(&state->ring_hour, HIST_HOUR_LEN, 3600);
    ring_init(&state->ring_day, HIST_DAY_LEN, 86400);

    return 0;
}

void lanstat_state_free(lanstat_state_t *state) {
    if (!state) {
        return;
    }
    free(state->hosts);
    free(state->hash_slots);
    state->hosts = NULL;
    state->hash_slots = NULL;
}

static int32_t find_host_index(const lanstat_state_t *state, uint8_t family, const uint8_t addr[16]) {
    size_t mask = state->hash_cap - 1;
    size_t pos = (size_t)hash_host_key(family, addr) & mask;
    for (size_t step = 0; step < state->hash_cap; step++) {
        int32_t idx = state->hash_slots[pos];
        if (idx == -1) {
            return -1;
        }
        if (idx >= 0 && state->hosts[idx].used && host_key_eq(&state->hosts[idx], family, addr)) {
            return idx;
        }
        pos = (pos + 1) & mask;
    }
    return -1;
}

static int32_t evict_and_alloc_slot(lanstat_state_t *state) {
    for (size_t idx = 0; idx < state->max_hosts; idx++) {
        if (!state->hosts[idx].used) {
            return (int32_t)idx;
        }
    }

    size_t victim = SIZE_MAX;
    uint64_t oldest_ms = UINT64_MAX;
    /* Prefer evicting non-LAN entries first so LAN client slots stay preserved. */
    for (size_t pass = 0; pass < 2 && victim == SIZE_MAX; pass++) {
        for (size_t idx = 0; idx < state->max_hosts; idx++) {
            const lanstat_host_t *cand = &state->hosts[idx];
            if (!cand->used || cand->is_router) {
                continue;
            }
            if (pass == 0 && cand->is_lan) {
                continue;
            }
            if (cand->last_seen_ms < oldest_ms) {
                oldest_ms = cand->last_seen_ms;
                victim = idx;
            }
        }
    }
    if (victim == SIZE_MAX) {
        return -1;
    }
    state->hosts[victim].used = 0;
    state->host_count--;
    rehash_hosts(state);
    return (int32_t)victim;
}

static lanstat_host_t *upsert_host(lanstat_state_t *state,
                                   uint8_t family,
                                   const uint8_t addr[16],
                                   const uint8_t mac[6],
                                   bool update_mac,
                                   bool is_lan,
                                   bool is_router,
                                   uint64_t now_ms) {
    int32_t idx = find_host_index(state, family, addr);
    if (idx < 0) {
        idx = evict_and_alloc_slot(state);
        if (idx < 0) {
            return NULL;
        }
        lanstat_host_t *host = &state->hosts[idx];
        memset(host, 0, sizeof(*host));
        host->used = 1;
        host->family = family;
        host->is_lan = is_lan ? 1 : 0;
        host->is_router = is_router ? 1 : 0;
        memcpy(host->addr, addr, (family == AF_INET) ? 4 : 16);
        if (update_mac) {
            memcpy(host->mac, mac, 6);
            host->has_mac = 1;
        }
        host->last_seen_ms = now_ms;
        lookup_host_name(state, host);
        state->host_count++;

        size_t mask = state->hash_cap - 1;
        size_t pos = (size_t)hash_host_key(family, addr) & mask;
        while (state->hash_slots[pos] >= 0) {
            pos = (pos + 1) & mask;
        }
        state->hash_slots[pos] = idx;
        return host;
    }

    lanstat_host_t *host = &state->hosts[idx];
    if (is_lan) {
        host->is_lan = 1;
    }
    if (is_router) {
        host->is_router = 1;
    }
    if (update_mac) {
        if (!host->has_mac || memcmp(host->mac, mac, 6) != 0) {
            memcpy(host->mac, mac, 6);
            host->has_mac = 1;
            if (host->name[0] == '\0') {
                lookup_host_name(state, host);
            }
        }
    }
    host->last_seen_ms = now_ms;
    return host;
}

static bool is_valid_unicast_ipv4(const lanstat_iface_info_t *iface, uint32_t ip_be) {
    uint32_t ip_host = ntohl(ip_be);
    if (ip_host == 0 || ip_host == 0xFFFFFFFFU) {
        return false;
    }
    if ((ip_host >> 28) == 0xEU || (ip_host >> 24) == 127U) {
        return false;
    }
    if (iface->has_ipv4 && iface->if_netmask != 0) {
        if ((ip_be & iface->if_netmask) == (iface->if_ipv4 & iface->if_netmask)) {
            uint32_t mask_host = ntohl(iface->if_netmask);
            if (mask_host < 0xFFFFFFFEU) {
                if ((ip_be | iface->if_netmask) == 0xFFFFFFFFU) {
                    return false;
                }
                if ((ip_be & ~iface->if_netmask) == 0U) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool is_valid_unicast_ipv6(const uint8_t ip6[16]) {
    if (ip6[0] == 0xff) {
        return false;
    }
    bool all_zero = true;
    for (size_t idx = 0; idx < 15; idx++) {
        if (ip6[idx] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero && (ip6[15] == 0 || ip6[15] == 1)) {
        return false;
    }
    return true;
}

static bool is_router_ipv6(const lanstat_iface_info_t *iface, const uint8_t ip6[16]) {
    for (size_t idx = 0; idx < iface->router_ip6_count; idx++) {
        if (memcmp(iface->router_ip6s[idx], ip6, 16) == 0) {
            return true;
        }
    }
    return false;
}

static void classify_ipv4(const lanstat_iface_info_t *iface,
                          uint32_t ip_be,
                          const uint8_t mac[6],
                          bool mac_is_router,
                          bool *is_lan,
                          bool *is_router) {
    *is_router = (iface->has_ipv4 && ip_be == iface->if_ipv4);
    if (*is_router) {
        *is_lan = true;
        return;
    }
    if (iface->has_ipv4 && iface->if_netmask != 0) {
        *is_lan = ((ip_be & iface->if_netmask) == (iface->if_ipv4 & iface->if_netmask));
    } else {
        *is_lan = (!mac_is_router && !(mac[0] & 1));
    }
}

static void classify_ipv6(const lanstat_iface_info_t *iface,
                          const uint8_t ip6[16],
                          const uint8_t mac[6],
                          bool mac_is_router,
                          bool *is_lan,
                          bool *is_router) {
    *is_router = is_router_ipv6(iface, ip6);
    if (*is_router) {
        *is_lan = true;
        return;
    }
    *is_lan = (!mac_is_router && !(mac[0] & 1));
}

static void account_endpoint(lanstat_state_t *state,
                             uint8_t family,
                             const uint8_t addr[16],
                             const uint8_t mac[6],
                             bool mac_is_router,
                             bool is_lan,
                             bool is_router,
                             bool is_lan_peer,
                             bool is_download,
                             uint32_t wire_len,
                             uint64_t now_ms) {
    if (!is_lan && !state->track_external) {
        return;
    }
    bool mac_is_mcast = (mac[0] & 1) != 0;
    bool update_mac = (!mac_is_mcast) && (is_router || !mac_is_router || !is_lan);
    lanstat_host_t *host = upsert_host(state, family, addr, mac, update_mac, is_lan, is_router, now_ms);
    if (!host) {
        return;
    }
    if (is_download) {
        host->rx_bytes += wire_len;
        host->rx_pkts++;
        if (is_lan_peer) {
            host->lan_rx_bytes += wire_len;
        }
        if (is_lan && !is_router) {
            state->total_rx_bytes += wire_len;
            if (is_lan_peer) {
                state->total_lan_rx_bytes += wire_len;
            }
        }
    } else {
        host->tx_bytes += wire_len;
        host->tx_pkts++;
        if (is_lan_peer) {
            host->lan_tx_bytes += wire_len;
        }
        if (is_lan && !is_router) {
            state->total_tx_bytes += wire_len;
            if (is_lan_peer) {
                state->total_lan_tx_bytes += wire_len;
            }
        }
    }
}

void lanstat_record_packet(lanstat_state_t *state,
                           const uint8_t *frame,
                           uint32_t snap_len,
                           uint32_t wire_len,
                           uint64_t now_ms) {
    if (!frame || snap_len < 14) {
        return;
    }
    state->raw_wire_bytes += wire_len;
    state->raw_packets++;

    const uint8_t *dst_mac = frame;
    const uint8_t *src_mac = frame + 6;
    uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
    uint32_t offset = 14;

    for (int vlan_depth = 0; vlan_depth < 2; vlan_depth++) {
        if ((ethertype == 0x8100 || ethertype == 0x88A8) && snap_len >= offset + 4) {
            ethertype = (uint16_t)((frame[offset + 2] << 8) | frame[offset + 3]);
            offset += 4;
        } else {
            break;
        }
    }

    bool src_mac_router = state->iface.has_mac && memcmp(src_mac, state->iface.router_mac, 6) == 0;
    bool dst_mac_router = state->iface.has_mac && memcmp(dst_mac, state->iface.router_mac, 6) == 0;
    bool dst_is_mcast = (dst_mac[0] & 1) != 0;

    if (ethertype == 0x0800) {
        if (snap_len < offset + 20) {
            return;
        }
        const uint8_t *ip_hdr = frame + offset;
        if ((ip_hdr[0] >> 4) != 4 || (ip_hdr[0] & 0x0f) < 5) {
            return;
        }
        uint8_t src_addr[16] = {0};
        uint8_t dst_addr[16] = {0};
        memcpy(src_addr, ip_hdr + 12, 4);
        memcpy(dst_addr, ip_hdr + 16, 4);
        uint32_t src_be;
        uint32_t dst_be;
        memcpy(&src_be, ip_hdr + 12, 4);
        memcpy(&dst_be, ip_hdr + 16, 4);

        bool src_valid = is_valid_unicast_ipv4(&state->iface, src_be);
        bool dst_valid = !dst_is_mcast && is_valid_unicast_ipv4(&state->iface, dst_be);
        bool src_is_lan = false;
        bool src_is_router = false;
        bool dst_is_lan = false;
        bool dst_is_router = false;
        if (src_valid) {
            classify_ipv4(&state->iface, src_be, src_mac, src_mac_router, &src_is_lan, &src_is_router);
        }
        if (dst_valid) {
            classify_ipv4(&state->iface, dst_be, dst_mac, dst_mac_router, &dst_is_lan, &dst_is_router);
        }
        if (src_valid) {
            account_endpoint(state, AF_INET, src_addr, src_mac, src_mac_router,
                             src_is_lan, src_is_router, dst_valid && dst_is_lan, false, wire_len, now_ms);
        }
        if (dst_valid) {
            account_endpoint(state, AF_INET, dst_addr, dst_mac, dst_mac_router,
                             dst_is_lan, dst_is_router, src_valid && src_is_lan, true, wire_len, now_ms);
        }
    } else if (ethertype == 0x86DD) {
        if (snap_len < offset + 40) {
            return;
        }
        const uint8_t *ip6_hdr = frame + offset;
        if ((ip6_hdr[0] >> 4) != 6) {
            return;
        }
        const uint8_t *src_addr = ip6_hdr + 8;
        const uint8_t *dst_addr = ip6_hdr + 24;

        bool src_valid = is_valid_unicast_ipv6(src_addr);
        bool dst_valid = !dst_is_mcast && is_valid_unicast_ipv6(dst_addr);
        bool src_is_lan = false;
        bool src_is_router = false;
        bool dst_is_lan = false;
        bool dst_is_router = false;
        if (src_valid) {
            classify_ipv6(&state->iface, src_addr, src_mac, src_mac_router, &src_is_lan, &src_is_router);
        }
        if (dst_valid) {
            classify_ipv6(&state->iface, dst_addr, dst_mac, dst_mac_router, &dst_is_lan, &dst_is_router);
        }
        if (src_valid) {
            account_endpoint(state, AF_INET6, src_addr, src_mac, src_mac_router,
                             src_is_lan, src_is_router, dst_valid && dst_is_lan, false, wire_len, now_ms);
        }
        if (dst_valid) {
            account_endpoint(state, AF_INET6, dst_addr, dst_mac, dst_mac_router,
                             dst_is_lan, dst_is_router, src_valid && src_is_lan, true, wire_len, now_ms);
        }
    }
}

static uint64_t read_self_rss_bytes(void) {
#ifdef __linux__
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp) {
        return 0;
    }
    unsigned long size_pages = 0;
    unsigned long rss_pages = 0;
    int scanned = fscanf(fp, "%lu %lu", &size_pages, &rss_pages);
    fclose(fp);
    if (scanned == 2) {
        long page_sz = sysconf(_SC_PAGESIZE);
        if (page_sz > 0) {
            return (uint64_t)rss_pages * (uint64_t)page_sz;
        }
    }
#endif
    return 0;
}

static uint64_t read_mem_total_bytes(void) {
#ifdef __linux__
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        return 0;
    }
    char line[128];
    uint64_t total_kb = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %llu kB", (unsigned long long *)&total_kb) == 1) {
            break;
        }
    }
    fclose(fp);
    return total_kb * 1024ULL;
#else
    return 0;
#endif
}

static uint64_t read_self_cpu_ticks(void) {
#ifdef __linux__
    FILE *fp = fopen("/proc/self/stat", "r");
    if (!fp) {
        return 0;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) {
        return 0;
    }
    buf[n] = '\0';
    char *rparen = strrchr(buf, ')');
    if (!rparen) {
        return 0;
    }
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    int matched = sscanf(
        rparen + 2,
        "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
        &utime,
        &stime
    );
    if (matched == 2) {
        return (uint64_t)(utime + stime);
    }
#endif
    return 0;
}

static uint64_t read_sys_cpu_ticks(void) {
#ifdef __linux__
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return 0;
    }
    char line[256];
    uint64_t sum = 0;
    if (fgets(line, sizeof(line), fp) && strncmp(line, "cpu ", 4) == 0) {
        char *p = line + 4;
        char *end = NULL;
        while (*p) {
            unsigned long long val = strtoull(p, &end, 10);
            if (end == p) {
                break;
            }
            sum += (uint64_t)val;
            p = end;
        }
    }
    fclose(fp);
    return sum;
#else
    return 0;
#endif
}

static uint32_t clamp_u32_Bps(uint64_t delta_bytes, uint64_t dt_ms) {
    uint64_t val = (delta_bytes * 1000ULL) / dt_ms;
    return (val > UINT32_MAX) ? UINT32_MAX : (uint32_t)val;
}

void lanstat_tick(lanstat_state_t *state, uint64_t now_ms) {
    if (now_ms <= state->last_tick_ms) {
        return;
    }
    uint64_t dt_ms = now_ms - state->last_tick_ms;
    if (dt_ms < 500) {
        return;
    }

    size_t hist_pos = state->host_hist_head;
    for (size_t idx = 0; idx < state->max_hosts; idx++) {
        lanstat_host_t *host = &state->hosts[idx];
        if (!host->used) {
            continue;
        }
        uint64_t rx_delta = host->rx_bytes - host->prev_rx_bytes;
        uint64_t tx_delta = host->tx_bytes - host->prev_tx_bytes;
        host->prev_rx_bytes = host->rx_bytes;
        host->prev_tx_bytes = host->tx_bytes;
        host->rx_bps = (rx_delta * 8000ULL) / dt_ms;
        host->tx_bps = (tx_delta * 8000ULL) / dt_ms;
        uint64_t lan_rx_delta = host->lan_rx_bytes - host->prev_lan_rx_bytes;
        uint64_t lan_tx_delta = host->lan_tx_bytes - host->prev_lan_tx_bytes;
        host->prev_lan_rx_bytes = host->lan_rx_bytes;
        host->prev_lan_tx_bytes = host->lan_tx_bytes;
        host->lan_rx_bps = (lan_rx_delta * 8000ULL) / dt_ms;
        host->lan_tx_bps = (lan_tx_delta * 8000ULL) / dt_ms;
        uint64_t rx_pkts_delta = host->rx_pkts - host->prev_rx_pkts;
        uint64_t tx_pkts_delta = host->tx_pkts - host->prev_tx_pkts;
        host->prev_rx_pkts = host->rx_pkts;
        host->prev_tx_pkts = host->tx_pkts;
        host->rx_pps = clamp_u32_Bps(rx_pkts_delta, dt_ms);
        host->tx_pps = clamp_u32_Bps(tx_pkts_delta, dt_ms);
        host->hist_rx_Bps[hist_pos] = clamp_u32_Bps(rx_delta, dt_ms);
        host->hist_tx_Bps[hist_pos] = clamp_u32_Bps(tx_delta, dt_ms);
    }
    state->host_hist_head = (hist_pos + 1) % HIST_SEC_LEN;
    if (state->host_hist_count < HIST_SEC_LEN) {
        state->host_hist_count++;
    }

    uint64_t tot_rx_delta = state->total_rx_bytes - state->prev_total_rx_bytes;
    uint64_t tot_tx_delta = state->total_tx_bytes - state->prev_total_tx_bytes;
    state->prev_total_rx_bytes = state->total_rx_bytes;
    state->prev_total_tx_bytes = state->total_tx_bytes;

    state->total_rx_bps = (tot_rx_delta * 8000ULL) / dt_ms;
    state->total_tx_bps = (tot_tx_delta * 8000ULL) / dt_ms;
    uint64_t tot_lan_rx_delta = state->total_lan_rx_bytes - state->prev_total_lan_rx_bytes;
    uint64_t tot_lan_tx_delta = state->total_lan_tx_bytes - state->prev_total_lan_tx_bytes;
    state->prev_total_lan_rx_bytes = state->total_lan_rx_bytes;
    state->prev_total_lan_tx_bytes = state->total_lan_tx_bytes;
    state->total_lan_rx_bps = (tot_lan_rx_delta * 8000ULL) / dt_ms;
    state->total_lan_tx_bps = (tot_lan_tx_delta * 8000ULL) / dt_ms;
    ring_push(&state->ring_sec, state->total_rx_bps, state->total_tx_bps);

    uint32_t elapsed_secs = (uint32_t)((dt_ms + 500ULL) / 1000ULL);
    if (elapsed_secs == 0) {
        elapsed_secs = 1;
    }
    ring_accumulate(&state->ring_min, tot_rx_delta, tot_tx_delta, elapsed_secs);
    ring_accumulate(&state->ring_hour, tot_rx_delta, tot_tx_delta, elapsed_secs);
    ring_accumulate(&state->ring_day, tot_rx_delta, tot_tx_delta, elapsed_secs);

    uint64_t rss = read_self_rss_bytes();
    if (rss > 0) {
        state->rss_bytes = rss;
    }
    if (state->mem_total_bytes == 0) {
        state->mem_total_bytes = read_mem_total_bytes();
    }
    uint64_t proc_ticks = read_self_cpu_ticks();
    uint64_t sys_ticks = read_sys_cpu_ticks();
    if (state->last_sys_cpu_ticks > 0 && sys_ticks > state->last_sys_cpu_ticks && proc_ticks >= state->last_proc_cpu_ticks) {
        uint64_t proc_delta = proc_ticks - state->last_proc_cpu_ticks;
        uint64_t sys_delta = sys_ticks - state->last_sys_cpu_ticks;
        uint64_t pct_x100 = (proc_delta * 10000ULL) / sys_delta;
        state->cpu_pct_x100 = pct_x100 > 10000ULL ? 10000U : (uint32_t)pct_x100;
    }
    state->last_proc_cpu_ticks = proc_ticks;
    state->last_sys_cpu_ticks = sys_ticks;
    state->last_tick_ms = now_ms;
}

void lanstat_refresh_ifaces(lanstat_state_t *state) {
    state->iface_list[0] = '\0';
#ifdef __linux__
    DIR *dir = opendir("/sys/class/net");
    if (!dir) {
        return;
    }
    size_t pos = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0) {
            continue;
        }
        size_t nlen = strlen(ent->d_name);
        if (nlen == 0 || nlen >= 32) {
            continue;
        }
        if (pos + nlen + 2 >= sizeof(state->iface_list)) {
            break;
        }
        if (pos > 0) {
            state->iface_list[pos++] = ',';
        }
        memcpy(state->iface_list + pos, ent->d_name, nlen);
        pos += nlen;
        state->iface_list[pos] = '\0';
    }
    closedir(dir);
#endif
}

void lanstat_reset_stats(lanstat_state_t *state, const char *new_iface, uint64_t now_ms) {
    if (new_iface && new_iface[0]) {
        snprintf(state->iface.iface, sizeof(state->iface.iface), "%s", new_iface);
        lanstat_refresh_iface(state);
    }
    memset(state->hosts, 0, state->max_hosts * sizeof(state->hosts[0]));
    state->total_rx_bytes = 0;
    state->total_tx_bytes = 0;
    state->total_lan_rx_bytes = 0;
    state->total_lan_tx_bytes = 0;
    state->total_rx_bps = 0;
    state->total_tx_bps = 0;
    state->total_lan_rx_bps = 0;
    state->total_lan_tx_bps = 0;
    state->raw_wire_bytes = 0;
    state->raw_packets = 0;
    state->kern_captured = 0;
    state->kern_dropped = 0;
    state->host_hist_head = 0;
    state->host_hist_count = 0;
    memset(&state->ring_sec, 0, sizeof(state->ring_sec));
    memset(&state->ring_min, 0, sizeof(state->ring_min));
    memset(&state->ring_hour, 0, sizeof(state->ring_hour));
    memset(&state->ring_day, 0, sizeof(state->ring_day));
    state->last_tick_ms = now_ms;
    state->capture_failed = false;
    state->iface_switch_requested = false;
}

const lanstat_host_t *lanstat_find_host_by_str(const lanstat_state_t *state, const char *ip_str) {
    uint8_t family = 0;
    uint8_t addr[16];
    if (!parse_ip_str(ip_str, &family, addr)) {
        return NULL;
    }
    int32_t idx = find_host_index(state, family, addr);
    return (idx >= 0) ? &state->hosts[idx] : NULL;
}

void lanstat_format_ip(const lanstat_host_t *host, char *buf, size_t buf_len) {
    if (!inet_ntop(host->family, host->addr, buf, (socklen_t)buf_len)) {
        snprintf(buf, buf_len, "?");
    }
}

void lanstat_format_mac(const uint8_t mac[6], char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
