#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "lanstat.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "lanstat %s - lightweight native packet & bandwidth monitor\n"
            "Usage: %s [options]\n"
            "  -i <iface>      Capture interface (default: br-lan)\n"
            "  -b <bind_ip>    HTTP bind IP (default: 0.0.0.0)\n"
            "  -p <port>       HTTP port (default: 6670)\n"
            "  -n              Disable promiscuous mode\n"
            "  -e              Also track external (non-LAN) IPs\n"
            "  -m <max_hosts>  Max host slots (default: 512, max: 4096)\n"
            "  -r <ring_kb>    TPACKET_V2 RX ring size in KiB (default: 1024)\n"
            "  -d              Daemonize in background\n"
            "  -v              Print version and exit\n"
            "  -h              Show this help\n",
            LANSTAT_VERSION, prog);
}

static int daemonize_process(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }
    if (setsid() < 0) {
        return -1;
    }
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (!getenv("TZ")) {
        FILE *tzf = fopen("/etc/TZ", "r");
        if (tzf) {
            char tzbuf[64];
            if (fgets(tzbuf, sizeof(tzbuf), tzf)) {
                size_t len = strcspn(tzbuf, "\r\n");
                tzbuf[len] = '\0';
                if (len > 0) {
                    setenv("TZ", tzbuf, 1);
                }
            }
            fclose(tzf);
        }
    }
    tzset();

    lanstat_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.iface, sizeof(cfg.iface), "br-lan");
    snprintf(cfg.bind_ip, sizeof(cfg.bind_ip), "0.0.0.0");
    cfg.port = 6670;
    cfg.promisc = true;
    cfg.track_external = false;
    cfg.daemonize = false;
    cfg.max_hosts = DEFAULT_MAX_HOSTS;
    cfg.ring_kb = 1024;
    cfg.leases_path = "/tmp/dhcp.leases";
    cfg.hosts_path = "/etc/hosts";
    cfg.ethers_path = "/etc/ethers";

    int opt;
    while ((opt = getopt(argc, argv, "i:b:p:nem:r:dvh")) != -1) {
        switch (opt) {
        case 'i':
            snprintf(cfg.iface, sizeof(cfg.iface), "%s", optarg);
            break;
        case 'b':
            snprintf(cfg.bind_ip, sizeof(cfg.bind_ip), "%s", optarg);
            break;
        case 'p': {
            long port_val = strtol(optarg, NULL, 10);
            if (port_val <= 0 || port_val > 65535) {
                fprintf(stderr, "lanstat: invalid port '%s'\n", optarg);
                return 1;
            }
            cfg.port = (uint16_t)port_val;
            break;
        }
        case 'n':
            cfg.promisc = false;
            break;
        case 'e':
            cfg.track_external = true;
            break;
        case 'm':
            cfg.max_hosts = (size_t)strtoul(optarg, NULL, 10);
            break;
        case 'r':
            cfg.ring_kb = (size_t)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            cfg.daemonize = true;
            break;
        case 'v':
            printf("lanstat %s\n", LANSTAT_VERSION);
            return 0;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint64_t now_ms = monotonic_ms();
    lanstat_state_t state;
    if (lanstat_state_init(&state, &cfg, now_ms) != 0) {
        fprintf(stderr, "lanstat: failed to initialize state\n");
        return 1;
    }
    lanstat_refresh_iface(&state);
    lanstat_refresh_names(&state, cfg.leases_path, cfg.hosts_path, cfg.ethers_path, true);
    lanstat_refresh_ifaces(&state);

    lanstat_capture_t *cap = lanstat_capture_open(cfg.iface, cfg.promisc, cfg.ring_kb);
    if (!cap && state.iface_list[0]) {
        const char *fallbacks[] = {"lan", "eth0", "eth0.1", "eth1", NULL};
        /* First try any br-* bridge found in iface_list */
        const char *p = state.iface_list;
        while (*p && !cap) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            if (len > 3 && len < 32 && strncmp(p, "br-", 3) == 0) {
                char cand[32];
                memcpy(cand, p, len);
                cand[len] = '\0';
                if (strcmp(cand, cfg.iface) != 0) {
                    cap = lanstat_capture_open(cand, cfg.promisc, cfg.ring_kb);
                    if (cap) {
                        snprintf(state.iface.iface, sizeof(state.iface.iface), "%s", cand);
                        lanstat_refresh_iface(&state);
                    }
                }
            }
            if (!comma) break;
            p = comma + 1;
        }
        /* Then try common non-bridge LAN interfaces */
        for (size_t i = 0; fallbacks[i] && !cap; i++) {
            if (strcmp(fallbacks[i], cfg.iface) != 0) {
                cap = lanstat_capture_open(fallbacks[i], cfg.promisc, cfg.ring_kb);
                if (cap) {
                    snprintf(state.iface.iface, sizeof(state.iface.iface), "%s", fallbacks[i]);
                    lanstat_refresh_iface(&state);
                }
            }
        }
    }
    if (!cap) {
        state.capture_failed = true;
    }

    lanstat_http_t *http = lanstat_http_open(cfg.bind_ip, cfg.port);
    if (!http) {
        fprintf(stderr, "lanstat: failed to bind HTTP %s:%u: %s\n",
                cfg.bind_ip, cfg.port, strerror(errno));
        lanstat_capture_close(cap);
        lanstat_state_free(&state);
        return 1;
    }

    fprintf(stderr, "lanstat %s listening on http://%s:%u/ (iface=%s, max_hosts=%zu, ring=%zu KiB)\n",
            LANSTAT_VERSION, cfg.bind_ip, cfg.port, cfg.iface, state.max_hosts, cfg.ring_kb);

    if (cfg.daemonize && daemonize_process() != 0) {
        fprintf(stderr, "lanstat: daemonize failed\n");
        lanstat_http_close(http);
        lanstat_capture_close(cap);
        lanstat_state_free(&state);
        return 1;
    }

    const size_t drain_budget = 2048;
    bool more_packets = false;
    int cap_fd = cap ? lanstat_capture_fd(cap) : -1;

    while (g_running) {
        struct pollfd pfds[32];
        int nfds = 0;
        if (cap_fd >= 0) {
            pfds[nfds].fd = cap_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        int http_start = nfds;
        int http_nfds = lanstat_http_prepare_poll(http, pfds + nfds, 32 - nfds);
        nfds += http_nfds;

        now_ms = monotonic_ms();
        int timeout_ms = 250;
        if (more_packets) {
            timeout_ms = 0;
        } else if (now_ms >= state.last_tick_ms) {
            uint64_t elapsed = now_ms - state.last_tick_ms;
            if (elapsed >= 1000) {
                timeout_ms = 0;
            } else {
                uint64_t rem = 1000 - elapsed;
                if (rem < (uint64_t)timeout_ms) {
                    timeout_ms = (int)rem;
                }
            }
        }

        int ready = poll(pfds, (nfds_t)nfds, timeout_ms);
        now_ms = monotonic_ms();
        if (ready < 0 && errno != EINTR) {
            break;
        }

        size_t drained = cap ? lanstat_capture_drain(cap, &state, drain_budget, now_ms) : 0;
        more_packets = (drained >= drain_budget);

        if (state.iface_switch_requested) {
            char next_iface[32];
            snprintf(next_iface, sizeof(next_iface), "%s", state.pending_iface);
            if (cap) {
                lanstat_capture_close(cap);
                cap = NULL;
            }
            lanstat_reset_stats(&state, next_iface, now_ms);
            cap = lanstat_capture_open(next_iface, cfg.promisc, cfg.ring_kb);
            if (!cap) {
                state.capture_failed = true;
            }
            cap_fd = cap ? lanstat_capture_fd(cap) : -1;
            more_packets = false;
            if (access("/sbin/uci", X_OK) == 0) {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "uci set lanstat.main.iface=%s && uci commit lanstat >/dev/null 2>&1", next_iface);
                int rc = system(cmd);
                (void)rc;
            }
        }

        if (ready > 0 && http_nfds > 0) {
            lanstat_http_step(http, &state, pfds + http_start, http_nfds, now_ms);
        }

        if (now_ms - state.last_tick_ms >= 1000) {
            lanstat_capture_poll_stats(cap, &state);
            lanstat_tick(&state, now_ms);
        }
        if (now_ms - state.last_name_check_ms >= 10000) {
            state.last_name_check_ms = now_ms;
            lanstat_refresh_names(&state, cfg.leases_path, cfg.hosts_path, cfg.ethers_path, false);
        }
        if (now_ms - state.last_iface_check_ms >= 30000) {
            state.last_iface_check_ms = now_ms;
            lanstat_refresh_iface(&state);
        }
    }

    lanstat_http_close(http);
    if (cap) {
        lanstat_capture_close(cap);
    }
    lanstat_state_free(&state);
    return 0;
}
