#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "lanstat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#endif

struct lanstat_capture {
    int fd;
    bool use_ring;
    uint8_t *ring_base;
    size_t ring_bytes;
    size_t frame_size;
    size_t frame_nr;
    size_t ring_idx;
};

#ifdef __linux__

static bool set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void attach_ip_bpf(int fd) {
    static const struct sock_filter insns[] = {
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0800, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x86DD, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x8100, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x88A8, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 256),
        BPF_STMT(BPF_RET | BPF_K, 0),
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(insns) / sizeof(insns[0])),
        .filter = (struct sock_filter *)(uintptr_t)insns,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) != 0) {
        fprintf(stderr, "lanstat: SO_ATTACH_FILTER warning: %s\n", strerror(errno));
    }
}

static bool setup_rx_ring(lanstat_capture_t *cap, size_t ring_kb) {
    int ver = TPACKET_V2;
    if (setsockopt(cap->fd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver)) != 0) {
        return false;
    }

    size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
    if (page_sz < 4096) {
        page_sz = 4096;
    }
    size_t block_size = page_sz * 4;
    if (block_size < 16384) {
        block_size = 16384;
    }
    size_t frame_size = 256;
    size_t total_bytes = ring_kb * 1024;
    if (total_bytes < block_size * 4) {
        total_bytes = block_size * 4;
    }
    size_t block_nr = total_bytes / block_size;
    if (block_nr < 4) {
        block_nr = 4;
    }
    size_t frames_per_block = block_size / frame_size;
    size_t frame_nr = frames_per_block * block_nr;

    struct tpacket_req req;
    memset(&req, 0, sizeof(req));
    req.tp_block_size = (unsigned int)block_size;
    req.tp_block_nr = (unsigned int)block_nr;
    req.tp_frame_size = (unsigned int)frame_size;
    req.tp_frame_nr = (unsigned int)frame_nr;

    if (setsockopt(cap->fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) != 0) {
        return false;
    }

    size_t map_bytes = block_size * block_nr;
    void *mapped = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, cap->fd, 0);
    if (mapped == MAP_FAILED) {
        return false;
    }

    cap->use_ring = true;
    cap->ring_base = (uint8_t *)mapped;
    cap->ring_bytes = map_bytes;
    cap->frame_size = frame_size;
    cap->frame_nr = frame_nr;
    cap->ring_idx = 0;
    return true;
}

lanstat_capture_t *lanstat_capture_open(const char *iface, bool promisc, size_t ring_kb) {
    unsigned int ifindex = if_nametoindex(iface);
    if (ifindex == 0) {
        fprintf(stderr, "lanstat: interface '%s' not found: %s\n", iface, strerror(errno));
        return NULL;
    }

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        fprintf(stderr, "lanstat: socket(AF_PACKET) failed: %s\n", strerror(errno));
        return NULL;
    }

    lanstat_capture_t *cap = calloc(1, sizeof(*cap));
    if (!cap) {
        close(fd);
        return NULL;
    }
    cap->fd = fd;

    attach_ip_bpf(fd);

    if (!setup_rx_ring(cap, ring_kb)) {
        int rcvbuf = 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        cap->use_ring = false;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = (int)ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) != 0) {
        fprintf(stderr, "lanstat: bind(%s) failed: %s\n", iface, strerror(errno));
        lanstat_capture_close(cap);
        return NULL;
    }

    if (promisc) {
        struct packet_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.mr_ifindex = (int)ifindex;
        mreq.mr_type = PACKET_MR_PROMISC;
        if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            fprintf(stderr, "lanstat: promisc warning on %s: %s\n", iface, strerror(errno));
        }
    }

    set_nonblock(fd);
    return cap;
}

void lanstat_capture_close(lanstat_capture_t *cap) {
    if (!cap) {
        return;
    }
    if (cap->use_ring && cap->ring_base && cap->ring_bytes > 0) {
        munmap(cap->ring_base, cap->ring_bytes);
    }
    if (cap->fd >= 0) {
        close(cap->fd);
    }
    free(cap);
}

int lanstat_capture_fd(const lanstat_capture_t *cap) {
    return cap ? cap->fd : -1;
}

size_t lanstat_capture_drain(lanstat_capture_t *cap,
                             lanstat_state_t *state,
                             size_t budget,
                             uint64_t now_ms) {
    if (!cap || cap->fd < 0) {
        return 0;
    }
    size_t processed = 0;

    if (cap->use_ring) {
        while (processed < budget) {
            uint8_t *slot = cap->ring_base + cap->ring_idx * cap->frame_size;
            struct tpacket2_hdr *hdr = (struct tpacket2_hdr *)slot;
            uint32_t status = __atomic_load_n(&hdr->tp_status, __ATOMIC_ACQUIRE);
            if ((status & TP_STATUS_USER) == 0) {
                break;
            }
            uint16_t mac_off = hdr->tp_mac;
            uint32_t snap_len = hdr->tp_snaplen;
            uint32_t wire_len = hdr->tp_len;
            if (mac_off < cap->frame_size && snap_len <= cap->frame_size - mac_off) {
                lanstat_record_packet(state, slot + mac_off, snap_len, wire_len, now_ms);
            }
            __atomic_store_n(&hdr->tp_status, TP_STATUS_KERNEL, __ATOMIC_RELEASE);
            cap->ring_idx = (cap->ring_idx + 1) % cap->frame_nr;
            processed++;
        }
        return processed;
    }

    uint8_t buf[256];
    while (processed < budget) {
        ssize_t wire_ret = recv(cap->fd, buf, sizeof(buf), MSG_TRUNC);
        if (wire_ret <= 0) {
            break;
        }
        uint32_t wire_len = (uint32_t)wire_ret;
        uint32_t snap_len = (wire_len < sizeof(buf)) ? wire_len : (uint32_t)sizeof(buf);
        lanstat_record_packet(state, buf, snap_len, wire_len, now_ms);
        processed++;
    }
    return processed;
}

void lanstat_capture_poll_stats(lanstat_capture_t *cap, lanstat_state_t *state) {
    if (!cap || cap->fd < 0) {
        return;
    }
    struct tpacket_stats stats;
    socklen_t len = sizeof(stats);
    if (getsockopt(cap->fd, SOL_PACKET, PACKET_STATISTICS, &stats, &len) == 0) {
        state->kern_captured += stats.tp_packets;
        state->kern_dropped += stats.tp_drops;
    }
}

#else

lanstat_capture_t *lanstat_capture_open(const char *iface, bool promisc, size_t ring_kb) {
    (void)iface;
    (void)promisc;
    (void)ring_kb;
    return NULL;
}

void lanstat_capture_close(lanstat_capture_t *cap) {
    free(cap);
}

int lanstat_capture_fd(const lanstat_capture_t *cap) {
    (void)cap;
    return -1;
}

size_t lanstat_capture_drain(lanstat_capture_t *cap,
                             lanstat_state_t *state,
                             size_t budget,
                             uint64_t now_ms) {
    (void)cap;
    (void)state;
    (void)budget;
    (void)now_ms;
    return 0;
}

void lanstat_capture_poll_stats(lanstat_capture_t *cap, lanstat_state_t *state) {
    (void)cap;
    (void)state;
}

#endif
