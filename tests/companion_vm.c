// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-only TCP sender and synthetic TUN peer for lotspeed_dup.
 * @author lyt
 * @date 2026-11-17
 * This is a wire observer, not a TCP receiver implementation or ACK validator.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TOTAL (256U * 1024U)
#define PREFIX (128U * 1024U)
#define VNET 10U
#define PARAM "/sys/module/lotspeed_dup/parameters/"
#define FLOW_PROBE_ARG "lotspeed_dup_synthetic_flows="
#define CHECK(test, message) do { if (!(test)) fail(message); } while (0)

struct stats {
    unsigned long long flows, pending, queue, original, copies, copy_bytes;
    unsigned long long cancelled, budget, limit, packet, errors, late;
};

struct report {
    uint32_t bytes, retrans, unacked;
    uint32_t rtt, rttvar, rto, lost;
};

struct peer {
    int tun, command, result;
    pid_t child;
    uint32_t base, received, acknowledged;
    unsigned char seen[TOTAL];
    unsigned int originals, gso, copies, prefix_copies, tail_copies;
    unsigned int ack_delay;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_addr, peer_isn;
    uint64_t ack_due;
    uint64_t started, last_ack;
    int established, passive;
};

static pid_t active_child = -1;
static unsigned int handshake_ms = 25;
static unsigned int budget_pct, max_mbps;
static int all_ports, budgeted;
static unsigned int synthetic_flows;
static void diagnose_repeat(struct peer *p, const unsigned char *packet, uint32_t offset, uint32_t length);

static void fail(const char *message)
{
    fprintf(stderr, "COMPANION_VM_FAIL: %s (errno=%d: %s)\n", message, errno, strerror(errno));
    if (active_child > 0) {
        kill(active_child, SIGKILL);
        waitpid(active_child, NULL, 0);
    }
    exit(1);
}

static uint64_t milliseconds(void)
{
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "clock_gettime");
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const unsigned char *p)
{
    return ((uint32_t)get16(p) << 16) | get16(p + 2);
}

static void put16(unsigned char *p, uint16_t n)
{
    p[0] = (unsigned char)(n >> 8);
    p[1] = (unsigned char)n;
}

static void put32(unsigned char *p, uint32_t n)
{
    put16(p, (uint16_t)(n >> 16));
    put16(p + 2, (uint16_t)n);
}

static uint32_t sum_bytes(const unsigned char *p, size_t length, uint32_t sum)
{
    while (length >= 2) {
        sum += get16(p);
        p += 2;
        length -= 2;
    }
    if (length)
        sum += (uint32_t)*p << 8;
    return sum;
}

static uint16_t checksum(const unsigned char *p, size_t length, uint32_t sum)
{
    sum = sum_bytes(p, length, sum);
    while (sum >> 16)
        sum = (sum & 65535U) + (sum >> 16);
    return (uint16_t)~sum;
}

static unsigned char pattern(uint32_t offset)
{
    return (unsigned char)((offset * 31U) ^ (offset >> 8) ^ (offset >> 16) ^ 0xa5U);
}

static void read_text(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    CHECK(fd >= 0, path);
    n = read(fd, buffer, size - 1);
    CHECK(n > 0 && (size_t)n < size - 1, "read bounded text");
    buffer[n] = '\0';
    CHECK(close(fd) == 0, "close text");
}

static void guest_guard(void)
{
    char cmdline[8192], *token, *save;
    int allowed = 0;

    read_text("/proc/cmdline", cmdline, sizeof(cmdline));
    for (token = strtok_r(cmdline, " \t\r\n", &save); token;
         token = strtok_r(NULL, " \t\r\n", &save)) {
        if (!strcmp(token, "lotspeed_dup_test=1"))
            allowed = 1;
        if (!strcmp(token, "lotspeed_dup_all_ports=1"))
            all_ports = 1;
        if (!strncmp(token, FLOW_PROBE_ARG, sizeof(FLOW_PROBE_ARG) - 1)) {
            char *start = token + sizeof(FLOW_PROBE_ARG) - 1, *end;
            unsigned long count = strtoul(start, &end, 10);

            CHECK(end != start && !*end && count <= 100000, "invalid synthetic flow target");
            synthetic_flows = (unsigned int)count;
        }
        if (!strncmp(token, "lotspeed_dup_handshake_ms=", 26)) {
            char *end;
            unsigned long delay = strtoul(token + 26, &end, 10);

            CHECK(end != token + 26 && !*end && delay <= 50, "invalid guest handshake delay");
            handshake_ms = (unsigned int)delay;
        }
    }
    CHECK(allowed, "refusing runtime outside lotspeed_dup_test=1 guest");
    CHECK(geteuid() == 0, "guest root required (host root is not required)");
}

static void enable(int on)
{
    int fd = open(PARAM "enabled", O_WRONLY);

    CHECK(fd >= 0, "open guest enabled parameter");
    CHECK(write(fd, on ? "1\n" : "0\n", 2) == 2, "set guest enabled parameter");
    CHECK(close(fd) == 0, "close enabled parameter");
}

static struct stats snapshot(void)
{
    char buffer[1024];
    struct stats s = {0};

    read_text(PARAM "stats", buffer, sizeof(buffer));
    CHECK(sscanf(buffer, "flows=%llu pending=%llu queue_bytes=%llu original_ip_bytes=%llu "
                 "submitted_copies=%llu submitted_ip_bytes=%llu cancelled=%llu "
                 "skipped_budget=%llu skipped_limit=%llu skipped_packet=%llu output_errors=%llu skipped_late=%llu",
                 &s.flows, &s.pending, &s.queue, &s.original, &s.copies, &s.copy_bytes,
                 &s.cancelled, &s.budget, &s.limit, &s.packet, &s.errors, &s.late) == 12,
          "parse module stats");
    return s;
}

static void unchanged(struct stats a, struct stats b)
{
    CHECK(a.flows == b.flows && a.pending == b.pending && a.queue == b.queue &&
          a.original == b.original && a.copies == b.copies && a.copy_bytes == b.copy_bytes &&
          a.cancelled == b.cancelled && a.budget == b.budget && a.limit == b.limit &&
          a.packet == b.packet && a.errors == b.errors && a.late == b.late, "expected unchanged module stats");
}

static int make_tun(void)
{
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    int ctl, header_size = VNET;
    struct ifreq req = {0};
    struct sockaddr_in *addr = (struct sockaddr_in *)&req.ifr_addr;

    CHECK(fd >= 0, "open TUN");
    strcpy(req.ifr_name, "lsdup0");
    req.ifr_flags = IFF_TUN | IFF_NO_PI | IFF_VNET_HDR;
    CHECK(ioctl(fd, TUNSETIFF, &req) == 0, "TUNSETIFF");
    CHECK(ioctl(fd, TUNSETVNETHDRSZ, &header_size) == 0, "10-byte virtio header");
    CHECK(ioctl(fd, TUNSETOFFLOAD, TUN_F_CSUM | TUN_F_TSO4 | TUN_F_TSO6) == 0,
          "TUNSETOFFLOAD CSUM/TSO4/TSO6");
    ctl = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(ctl >= 0, "interface control socket");
    addr->sin_family = AF_INET;
    CHECK(inet_pton(AF_INET, "10.77.0.1", &addr->sin_addr) == 1, "local address");
    CHECK(ioctl(ctl, SIOCSIFADDR, &req) == 0, "TUN local address");
    CHECK(inet_pton(AF_INET, "255.255.255.0", &addr->sin_addr) == 1, "netmask");
    CHECK(ioctl(ctl, SIOCSIFNETMASK, &req) == 0, "TUN /24 netmask");
    req.ifr_mtu = 1500;
    CHECK(ioctl(ctl, SIOCSIFMTU, &req) == 0, "TUN MTU");
    req.ifr_flags = IFF_UP | IFF_RUNNING;
    CHECK(ioctl(ctl, SIOCSIFFLAGS, &req) == 0, "TUN up");
    close(ctl);
    return fd;
}

static void send_peer_packet(struct peer *p, unsigned char flags)
{
    unsigned char packet[VNET + 60] = {0};
    unsigned char *ip = packet + VNET, *tcp = ip + 20;
    int syn = flags & 0x02;
    unsigned int tcp_len = syn ? 28 : 20;
    uint32_t pseudo;

    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + tcp_len));
    ip[8] = 64;
    ip[9] = IPPROTO_TCP;
    put32(ip + 12, p->remote_addr);
    CHECK(inet_pton(AF_INET, "10.77.0.1", ip + 16) == 1, "sender address");
    put16(ip + 10, checksum(ip, 20, 0));
    put16(tcp, p->remote_port);
    put16(tcp + 2, p->local_port);
    put32(tcp + 4, p->peer_isn + (syn ? 0 : 1));
    if (flags & 0x10)
        put32(tcp + 8, p->base + p->received);
    tcp[12] = (unsigned char)((tcp_len / 4) << 4);
    tcp[13] = flags;
    put16(tcp + 14, 65535);
    if (syn) {
        const unsigned char options[8] = {2, 4, 4, 176, 4, 2, 1, 1};
        memcpy(tcp + 20, options, sizeof(options)); /* MSS=1200, SACK permitted. */
    }
    pseudo = sum_bytes(ip + 12, 8, IPPROTO_TCP + tcp_len);
    put16(tcp + 16, checksum(tcp, tcp_len, pseudo));
    CHECK(write(p->tun, packet, VNET + 20 + tcp_len) == (ssize_t)(VNET + 20 + tcp_len),
          "inject checksummed ACK");
    p->acknowledged = p->received;
    p->ack_due = 0;
    p->last_ack = milliseconds();
}

static void send_ack(struct peer *p)
{
    send_peer_packet(p, 0x10);
}

/* Returns one for new original data, zero for control/copies/unrelated IPv6. */
static int receive_packet(struct peer *p)
{
    unsigned char packet[65536 + VNET];
    unsigned char *ip = packet + VNET, *tcp;
    ssize_t n = read(p->tun, packet, sizeof(packet));
    uint32_t offset, length, i;
    unsigned int ip_len, tcp_len, total, repeated = 0;
    int partial, gso;

    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    CHECK(n >= (ssize_t)(VNET + 20), "short TUN packet");
    if ((ip[0] >> 4) == 6)
        return 0; /* IPv6 address configuration can emit unrelated control traffic. */
    CHECK(ip[0] == 0x45 && ip[9] == IPPROTO_TCP, "unexpected IPv4 packet");
    ip_len = (ip[0] & 15U) * 4;
    total = get16(ip + 2);
    CHECK(total == (unsigned int)n - VNET && total >= ip_len + 20, "IPv4 length");
    CHECK(checksum(ip, ip_len, 0) == 0, "IPv4 checksum");
    CHECK(get32(ip + 12) == 0x0a4d0001U && get32(ip + 16) == 0x0a4d0002U,
          "unexpected IPv4 tuple");
    tcp = ip + ip_len;
    CHECK(get16(tcp) == p->local_port && get16(tcp + 2) == 9000, "local-port selector tuple");
    tcp_len = (tcp[12] >> 4) * 4U;
    CHECK(tcp_len >= 20 && ip_len + tcp_len <= total, "TCP header length");
    length = total - ip_len - tcp_len;
    partial = packet[0] & 1;
    gso = packet[1] & 0x7f;
    CHECK(gso == 0 || gso == 1, "unexpected GSO type");
    if (gso) {
        unsigned int mss = packet[4] | ((unsigned int)packet[5] << 8);
        CHECK(partial && mss > 0 && mss <= 1200 && length > mss, "TCPv4 GSO metadata");
    }
    if (!partial)
        CHECK(checksum(tcp, total - ip_len,
                       sum_bytes(ip + 12, 8, IPPROTO_TCP + total - ip_len)) == 0,
              "software TCP checksum");
    if (tcp[13] & 0x02) {
        CHECK(!p->established && !length, "unexpected retransmitted SYN");
        CHECK(!!(tcp[13] & 0x10) == p->passive, "SYN/SYNACK direction");
        if (p->passive)
            CHECK(get32(tcp + 8) == 0x12345679U, "SYNACK acknowledgment");
        p->base = get32(tcp + 4) + 1;
        p->established = 1;
        /* Seed a realistic RTT before delayed data ACKs. A near-zero handshake RTT
         * followed by 25ms data ACKs can spuriously trigger TCP tail-loss probes.
         */
        usleep(handshake_ms * 1000);
        send_peer_packet(p, p->passive ? 0x10 : 0x12);
        return 0;
    }
    CHECK(!(tcp[13] & 0x05), "unexpected FIN/RST during transfer");
    if (!length)
        return 0;
    CHECK(p->established && (tcp[13] & 0x10), "data before handshake");
    offset = get32(tcp + 4) - p->base;
    CHECK(offset < TOTAL && length <= TOTAL - offset, "TCP stream bounds");
    for (i = 0; i < length; i++) {
        CHECK(tcp[tcp_len + i] == pattern(offset + i), "deterministic payload mismatch");
        repeated += p->seen[offset + i] != 0;
        p->seen[offset + i] = 1;
    }
    if (repeated) {
        CHECK(repeated == length, "partially repeated range");
        if (partial || gso || length > 1200)
            diagnose_repeat(p, packet, offset, length);
        CHECK(!partial && !gso && length <= 1200, "copy is not linear/software checksummed");
        p->copies++;
        if (offset < PREFIX)
            p->prefix_copies++;
        else
            p->tail_copies++;
        /* Never send duplicate ACKs for copies: they would induce fast retransmit. */
        return 0;
    }
    p->originals++;
    p->gso += gso != 0;
    CHECK(offset == p->received, "original stream gap or reordering");
    while (p->received < TOTAL && p->seen[p->received])
        p->received++;
    if (!p->ack_due)
        p->ack_due = milliseconds() + p->ack_delay;
    return 1;
}

static void pipe_write(int fd, const void *data, size_t size)
{
    CHECK(write(fd, data, size) == (ssize_t)size, "pipe write");
}

static void pipe_read(int fd, void *data, size_t size)
{
    struct pollfd pollfd = {.fd = fd, .events = POLLIN};

    CHECK(poll(&pollfd, 1, 5000) == 1, "pipe timeout");
    CHECK(read(fd, data, size) == (ssize_t)size, "pipe read");
}

static struct report socket_report(int fd, uint32_t bytes)
{
    struct tcp_info info = {0};
    struct report report = {.bytes = bytes};
    socklen_t size = sizeof(info);

    CHECK(getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &size) == 0, "TCP_INFO");
    report.retrans = info.tcpi_total_retrans;
    report.unacked = info.tcpi_unacked;
    report.rtt = info.tcpi_rtt;
    report.rttvar = info.tcpi_rttvar;
    report.rto = info.tcpi_rto;
    report.lost = info.tcpi_lost;
    return report;
}

static void diagnose_repeat(struct peer *p, const unsigned char *packet, uint32_t offset, uint32_t length)
{
    uint32_t request = UINT32_MAX;
    struct report report;
    uint64_t now = milliseconds();
    const unsigned char *tcp = packet + VNET + 20;

    fprintf(stderr, "REPEATED_RANGE elapsed_ms=%llu ack_age_ms=%llu ack_due_ms=%lld "
            "virtio_flags=0x%02x gso_type=0x%02x seq=%u ack=%u offset=%u length=%u acked=%u\n",
            (unsigned long long)(now - p->started), (unsigned long long)(now - p->last_ack),
            p->ack_due ? (long long)p->ack_due - (long long)now : -1LL,
            packet[0], packet[1], get32(tcp + 4), get32(tcp + 8), offset, length, p->acknowledged);
    /* Request an immediate TCP_INFO snapshot before preserving the strict failure. */
    pipe_write(p->command, &request, sizeof(request));
    pipe_read(p->result, &report, sizeof(report));
    fprintf(stderr, "REPEATED_TCP_INFO total_retrans=%u unacked=%u lost=%u rtt_us=%u "
            "rttvar_us=%u rto_us=%u handshake_ms=%u\n", report.retrans, report.unacked,
            report.lost, report.rtt, report.rttvar, report.rto, handshake_ms);
}

static void sender(int command, int result, int tun, int passive, uint16_t local_port)
{
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(local_port)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(9000)};
    struct linger linger = {.l_onoff = 1, .l_linger = 0};
    struct report report = {0};
    unsigned char buffer[32768];
    int fd, one = 1, send_buffer = 1024 * 1024;
    uint32_t amount, i;
    uint64_t deadline;

    active_child = -1;
    CHECK(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() != 1, "sender parent lifetime");
    alarm(25);
    close(tun);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "sender socket");
    CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "SO_REUSEADDR");
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0, "TCP_NODELAY");
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0, "SO_SNDBUF");
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) == 0, "SO_LINGER");
    CHECK(inet_pton(AF_INET, "10.77.0.1", &local.sin_addr) == 1, "bind address");
    CHECK(inet_pton(AF_INET, "10.77.0.2", &remote.sin_addr) == 1, "connect address");
    CHECK(bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0, "bind selected local port");
    if (passive) {
        int accepted;
        uint32_t ready = 1;

        CHECK(listen(fd, 1) == 0, "kernel TCP listen selected local port");
        pipe_write(result, &ready, sizeof(ready));
        accepted = accept(fd, NULL, NULL);
        CHECK(accepted >= 0, "kernel TCP accept");
        close(fd);
        fd = accepted;
    } else {
        CHECK(connect(fd, (struct sockaddr *)&remote, sizeof(remote)) == 0, "kernel TCP connect");
    }
    for (;;) {
        pipe_read(command, &amount, sizeof(amount));
        if (amount == UINT32_MAX) {
            struct report current = socket_report(fd, report.bytes);

            pipe_write(result, &current, sizeof(current));
            continue;
        }
        if (!amount)
            break;
        CHECK(amount <= TOTAL - report.bytes, "sender requested length");
        while (amount) {
            size_t chunk = amount < sizeof(buffer) ? amount : sizeof(buffer);
            ssize_t n;

            for (i = 0; i < chunk; i++)
                buffer[i] = pattern(report.bytes + i);
            n = send(fd, buffer, chunk, MSG_NOSIGNAL);
            CHECK(n > 0, "kernel TCP send");
            report.bytes += (uint32_t)n;
            amount -= (uint32_t)n;
        }
    }
    deadline = milliseconds() + 1000;
    do {
        report = socket_report(fd, report.bytes);
        if (!report.unacked)
            break;
        usleep(1000);
    } while (milliseconds() < deadline);
    pipe_write(result, &report, sizeof(report));
    pipe_read(command, &amount, sizeof(amount));
    close(fd); /* Abort only after assertions; no TIME_WAIT between identical tuples. */
    _exit(0);
}

static struct peer *start_peer(int tun, unsigned int delay, int passive, uint16_t local_port)
{
    struct peer *p = calloc(1, sizeof(*p));
    int command[2], result[2];

    CHECK(p && pipe(command) == 0 && pipe(result) == 0, "peer allocation/pipes");
    p->tun = tun;
    p->ack_delay = delay;
    p->local_port = local_port;
    p->remote_port = 9000;
    p->remote_addr = 0x0a4d0002U;
    p->peer_isn = 0x12345678U;
    p->started = milliseconds();
    p->passive = passive;
    p->child = fork();
    CHECK(p->child >= 0, "fork TCP sender");
    if (!p->child) {
        close(command[1]);
        close(result[0]);
        sender(command[0], result[1], tun, passive, local_port);
    }
    active_child = p->child;
    close(command[0]);
    close(result[1]);
    p->command = command[1];
    p->result = result[0];
    if (passive) {
        uint32_t ready = 0;

        pipe_read(p->result, &ready, sizeof(ready));
        CHECK(ready == 1, "listener ready");
        send_peer_packet(p, 0x02);
    }
    return p;
}

/* A bounded pump; an optional hook runs before ACK injection for new data only. */
static void transfer(struct peer *p, uint32_t amount, void (*hook)(struct peer *))
{
    uint32_t target = p->received + amount;
    uint64_t deadline = milliseconds() + 15000;

    pipe_write(p->command, &amount, sizeof(amount));
    while (p->received < target || p->acknowledged < target) {
        struct pollfd pollfd = {.fd = p->tun, .events = POLLIN};
        int status;

        CHECK(milliseconds() < deadline, "transfer deadline");
        status = poll(&pollfd, 1, 1);
        CHECK(status >= 0 || errno == EINTR, "TUN poll");
        if (status > 0 && receive_packet(p) && hook)
            hook(p);
        if (p->ack_due && milliseconds() >= p->ack_due)
            send_ack(p);
    }
}

static void settle(struct peer *p)
{
    uint64_t until = milliseconds() + 60;

    while (milliseconds() < until) {
        struct pollfd pollfd = {.fd = p->tun, .events = POLLIN};
        int status = poll(&pollfd, 1, 2);

        CHECK(status >= 0 || errno == EINTR, "settle poll");
        if (status > 0)
            CHECK(!receive_packet(p), "unexpected new data while settling");
    }
}

static void finish(struct peer *p, const char *name)
{
    struct report report;
    uint32_t stop = 0;
    int status;
    unsigned char drain[65546];

    pipe_write(p->command, &stop, sizeof(stop));
    pipe_read(p->result, &report, sizeof(report));
    printf("CASE %s local_port=%u bytes=%u originals=%u gso=%u copies=%u prefix=%u tail=%u "
           "tcpi_total_retrans=%u unacked=%u\n", name, p->local_port, report.bytes, p->originals, p->gso,
           p->copies, p->prefix_copies, p->tail_copies, report.retrans, report.unacked);
    CHECK(report.bytes == p->received && report.unacked == 0, "sender/peer byte or ACK mismatch");
    CHECK(report.retrans == 0, "normal TCP retransmissions must be zero");
    pipe_write(p->command, &stop, sizeof(stop));
    CHECK(waitpid(p->child, &status, 0) == p->child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "sender exit");
    active_child = -1;
    close(p->command);
    close(p->result);
    /* Sender reset is queued synchronously by close; discard only between cases. */
    while (read(p->tun, drain, sizeof(drain)) > 0)
        ;
    free(p);
}

static int fast_cancelled, purged;
static struct stats frozen;

static void check_budget(struct stats before, struct stats after, const char *phase, int require_skips)
{
    unsigned long long original, copies, allowance, skips;

    CHECK(after.original >= before.original && after.copy_bytes >= before.copy_bytes &&
          after.budget >= before.budget, "budget counters regressed");
    original = after.original - before.original;
    copies = after.copy_bytes - before.copy_bytes;
    allowance = original * budget_pct / 100;
    skips = after.budget - before.budget;
    printf("BUDGET %s budgeted=%d pct=%u max_mbps=%u original_ip_bytes=%llu submitted_ip_bytes=%llu "
           "allowance=%llu skipped_budget=%llu\n", phase, budgeted, budget_pct, max_mbps,
           original, copies, allowance, skips);
    /* Each caller starts immediately after enable resets credit, without prior traffic. */
    if (budgeted) {
        CHECK(copies <= allowance, "submitted IP bytes exceeded earned percentage budget");
        if (require_skips)
            CHECK(skips > 0, "budget-limited transfer did not exercise skipped_budget");
    }
}

static void enabled_case(int tun, int passive, uint16_t local_port)
{
    struct peer *p;
    struct stats before, after;
    int covered;
    int included = all_ports || local_port == 5201;
    unsigned int copies;

    enable(1);
    before = snapshot();
    CHECK(!before.flows && !before.pending && !before.queue, "scope case requires an empty flow table");
    p = start_peer(tun, 25, passive, local_port);
    transfer(p, TOTAL, NULL);
    settle(p);
    after = snapshot();
    covered = p->gso && p->copies;
    if (!budgeted || (budget_pct == 100 && max_mbps == 1000))
        covered = covered && p->prefix_copies && p->tail_copies;
    copies = p->copies;
    printf("ENROLLMENT %s local_port=%u expected=%s flows=%llu original_ip_bytes=%llu submitted_copies=%llu\n",
           passive ? "passive" : "active", local_port, included ? "tracked" : "excluded",
           after.flows, after.original - before.original,
           after.copies - before.copies);
    if (!included) {
        CHECK(p->gso && !p->copies, "excluded port must have GSO originals and no copies");
        /* Inspect while the socket is live, so close/reset cannot hide enrollment. */
        unchanged(before, after);
        finish(p, passive ? "excluded-passive" : "excluded-active");
        unchanged(before, snapshot());
        printf("SCOPE_EXCLUDED local_port=%u direction=%s stats_unchanged=1\n",
               local_port, passive ? "passive" : "active");
        enable(0);
        return;
    }
    finish(p, passive ? "passive-server-delayed-ack" : "enabled-delayed-ack");
    CHECK(after.flows == before.flows + 1, "SYN/SYNACK flow enrollment failed");
    CHECK(covered, "missing GSO/prefix/tail copy coverage");
    CHECK(after.copies - before.copies == copies, "submitted versus observed copy count");
    CHECK(after.original > before.original && after.errors == before.errors, "enabled stats/output errors");
    check_budget(before, after, passive ? "passive" : "active", budget_pct < 100);
    if (!budgeted) {
        /* Prove bypass behavior, not just parameter readback. This workload's prefix
         * must exceed the old default 10% cap; exact counts can vary with ACK cancellation.
         */
        CHECK(after.copy_bytes - before.copy_bytes > (after.original - before.original) / 10,
              "unbudgeted prefix/tail workload did not exceed the old 10 percent cap");
    }
    printf("SCOPE_TRACKED local_port=%u direction=%s copies=%u\n",
           local_port, passive ? "passive" : "active", copies);
    enable(0);
}

static void fast_ack(struct peer *p)
{
    struct stats before = snapshot(), after;

    send_ack(p);
    after = snapshot();
    /* With one 1200-byte write and no next write, no tail replacement can cancel it. */
    if (before.pending && !after.pending && after.cancelled > before.cancelled &&
        after.copies == before.copies)
        fast_cancelled++;
}

static void disable_pending(struct peer *p)
{
    struct stats before;

    (void)p;
    if (purged)
        return;
    before = snapshot();
    if (!before.pending)
        return;
    enable(0);
    frozen = snapshot();
    CHECK(!frozen.flows && !frozen.pending && !frozen.queue, "disable did not purge flows/queue");
    CHECK(frozen.cancelled > before.cancelled, "disable did not cancel a pending copy");
    purged = 1;
}

static void cookie_mode(const char *value)
{
    int fd = open("/proc/sys/net/ipv4/tcp_syncookies", O_WRONLY);
    size_t length = strlen(value);

    CHECK(fd >= 0, "guest-only SYN cookie setting");
    CHECK(write(fd, value, length) == (ssize_t)length, "set guest SYN cookie mode");
    CHECK(close(fd) == 0, "close SYN cookie setting");
}

static int synthetic_reply(int tun, unsigned char *seen, unsigned int sent)
{
    unsigned char packet[256], *ip = packet + VNET, *tcp = ip + 20;
    ssize_t n = read(tun, packet, sizeof(packet));
    unsigned int total, tcp_len, id;
    uint32_t ack;

    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    CHECK(n >= (ssize_t)(VNET + 20), "short synthetic response");
    if ((ip[0] >> 4) == 6)
        return 0;
    CHECK(ip[0] == 0x45 && ip[9] == IPPROTO_TCP, "synthetic response IPv4/TCP");
    total = get16(ip + 2);
    CHECK(total >= 40 && total == (unsigned int)n - VNET, "synthetic response length");
    tcp_len = (tcp[12] >> 4) * 4U;
    CHECK(tcp_len >= 20 && total == 20 + tcp_len && (tcp[13] & 0x17) == 0x12,
          "synthetic response must be an empty SYNACK");
    CHECK(packet[1] == 0 && checksum(ip, 20, 0) == 0, "synthetic response GSO/IP checksum");
    if (!(packet[0] & 1))
        CHECK(checksum(tcp, tcp_len, sum_bytes(ip + 12, 8, IPPROTO_TCP + tcp_len)) == 0,
              "synthetic response software TCP checksum");
    ack = get32(tcp + 8) - 0x40000001U;
    CHECK(ack % 16 == 0, "synthetic SYNACK sequence acknowledgment");
    id = ack / 16;
    CHECK(id < sent && !seen[id], "unexpected/duplicate synthetic SYNACK");
    CHECK(get32(ip + 12) == 0x0a4d0001U && get32(ip + 16) == 0x0a4d0002U + id / 50000 &&
          get16(tcp) == 5201 && get16(tcp + 2) == 10000 + id % 50000, "synthetic SYNACK tuple");
    seen[id] = 1;
    return 1; /* Deliberately do not ACK: this is not an established TCP connection. */
}

static void synthetic_state_case(int tun)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(5201)};
    struct peer *probe = calloc(1, sizeof(*probe));
    unsigned char *seen = calloc(synthetic_flows, 1);
    struct stats before, after, stopped;
    char saved_cookies[32], limit[32];
    unsigned int sent = 0, received = 0;
    uint64_t started, built, disabled;
    int listener, one = 1;

    CHECK(all_ports && !budgeted && probe && seen, "synthetic state test requires explicit all-ports mode");
    read_text(PARAM "max_flows", limit, sizeof(limit));
    CHECK(strtoul(limit, NULL, 10) >= synthetic_flows, "module flow ceiling below synthetic target");
    read_text("/proc/sys/net/ipv4/tcp_syncookies", saved_cookies, sizeof(saved_cookies));
    cookie_mode("2\n"); /* Guest only: no request_sock backlog growth or SYNACK retransmission timers. */
    listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0, "synthetic listener socket");
    CHECK(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "synthetic reuseaddr");
    CHECK(inet_pton(AF_INET, "10.77.0.1", &address.sin_addr) == 1, "synthetic local address");
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(listener, 1) == 0,
          "synthetic listener bind/listen");
    probe->tun = tun;
    probe->local_port = 5201;
    enable(1);
    before = snapshot();
    CHECK(!before.flows && !before.pending && !before.queue, "synthetic test requires empty state");
    started = milliseconds();
    while (received < synthetic_flows) {
        CHECK(milliseconds() - started < 60000, "synthetic state generation exceeded 60 seconds");
        /* Keep at most 64 replies outstanding, below the guest TUN queue limit. */
        while (sent < synthetic_flows && sent - received < 64) {
            probe->remote_addr = 0x0a4d0002U + sent / 50000;
            probe->remote_port = (uint16_t)(10000 + sent % 50000);
            probe->peer_isn = 0x40000000U + sent * 16;
            send_peer_packet(probe, 0x02);
            sent++;
        }
        if (synthetic_reply(tun, seen, sent)) {
            received++;
            if (received % 10000 == 0)
                printf("SYNTHETIC_PROGRESS synacks=%u target=%u\n", received, synthetic_flows);
        } else {
            struct pollfd pollfd = {.fd = tun, .events = POLLIN};
            int status = poll(&pollfd, 1, 10);

            CHECK(status >= 0 || errno == EINTR, "synthetic response poll");
        }
    }
    built = milliseconds();
    after = snapshot();
    CHECK(after.flows == synthetic_flows && !after.pending && !after.queue,
          "synthetic flow tracking count/queue mismatch");
    CHECK(after.original == before.original && after.copies == before.copies &&
          after.copy_bytes == before.copy_bytes && after.limit == before.limit,
          "synthetic SYN-only workload unexpectedly copied data or hit a resource limit");
    enable(0);
    disabled = milliseconds();
    stopped = snapshot();
    CHECK(!stopped.flows && !stopped.pending && !stopped.queue, "synthetic disable did not purge all state");
    CHECK(close(listener) == 0, "close synthetic listener");
    cookie_mode(saved_cookies);
    usleep(20000);
    unchanged(stopped, snapshot());
    printf("SYNTHETIC_STATE_PASS target=%u observed_synacks=%u tracked=%llu generation_ms=%llu "
           "disable_ms=%llu established_connections=0\n", synthetic_flows, received, after.flows,
           (unsigned long long)(built - started), (unsigned long long)(disabled - built));
    free(seen);
    free(probe);
}

int main(void)
{
    struct peer *p;
    struct stats before;
    char value[128];
    unsigned int i, probes;
    int tun;

    setvbuf(stdout, NULL, _IONBF, 0);
    guest_guard(); /* Must precede all sysfs writes, sockets, TUN ioctls and forks. */
    alarm(90);
    signal(SIGPIPE, SIG_IGN);
    read_text(PARAM "enabled", value, sizeof(value));
    CHECK(value[0] == 'N' || value[0] == '0', "module must start disabled");
    read_text(PARAM "port", value, sizeof(value));
    CHECK(strtoul(value, NULL, 10) == (all_ports ? 0U : 5201U), "module port does not match requested scope");
    read_text(PARAM "all_ports", value, sizeof(value));
    CHECK(value[0] == (all_ports ? 'Y' : 'N'), "module all_ports does not match requested scope");
    budgeted = !all_ports;
    read_text(PARAM "budgeted", value, sizeof(value));
    CHECK(value[0] == (budgeted ? 'Y' : 'N'), "module budgeted does not match requested mode");
    read_text(PARAM "delay_ms", value, sizeof(value));
    CHECK(strtoul(value, NULL, 10) == 5, "test requires delay_ms=5");
    read_text(PARAM "budget_pct", value, sizeof(value));
    budget_pct = (unsigned int)strtoul(value, NULL, 10);
    read_text(PARAM "max_mbps", value, sizeof(value));
    max_mbps = (unsigned int)strtoul(value, NULL, 10);
    CHECK(budget_pct >= 1 && budget_pct <= 100 && max_mbps >= 1 && max_mbps <= 1000,
          "invalid module budget/rate parameters");
    /* Small isolated writes must first earn enough credit for a single 1240-byte candidate. */
    probes = (100 + budget_pct - 1) / budget_pct + 24;
    printf("TIMING handshake_ms=%u data_ack_ms=25 copy_delay_ms=5\n", handshake_ms);
    printf("CONFIG budget_pct=%u max_mbps=%u all_ports=%d scope_port=%u budgeted=%d\n",
           budget_pct, max_mbps, all_ports, all_ports ? 0U : 5201U, budgeted);
    tun = make_tun();

    before = snapshot();
    p = start_peer(tun, 25, 0, 5201);
    transfer(p, TOTAL, NULL);
    settle(p);
    unchanged(before, snapshot());
    CHECK(!p->copies && p->gso, "disabled transfer must have GSO originals and no copies");
    finish(p, "disabled");

    enabled_case(tun, 0, 5201);
    enabled_case(tun, 1, 5201);
    enabled_case(tun, 0, 5202);
    enabled_case(tun, 1, 5202);

    enable(1);
    before = snapshot();
    p = start_peer(tun, 0, 0, 5201);
    for (i = 0; i < probes; i++)
        transfer(p, 1200, fast_ack);
    settle(p);
    CHECK(fast_cancelled > 0, "no observed fast ACK cancellation (scheduling-sensitive)");
    printf("ACK_OBSERVATION cancelled_probes=%d; NOT a claim of TCP-validated ACKs\n", fast_cancelled);
    check_budget(before, snapshot(), "fast-ack", 0);
    finish(p, "fast-ack");
    enable(0);

    enable(1);
    before = snapshot();
    p = start_peer(tun, 25, 0, 5201);
    /* Disable while a flow is live; subsequent writes occur only after disable returns. */
    for (i = 0; i < probes && !purged; i++)
        transfer(p, 1200, disable_pending);
    CHECK(purged, "no pending candidate available for disable test");
    check_budget(before, frozen, "disable-live-flow", 0);
    settle(p);
    before = snapshot();
    unchanged(frozen, before);
    i = p->copies;
    transfer(p, TOTAL - p->received, NULL);
    settle(p);
    unchanged(before, snapshot());
    CHECK(p->copies == i, "new copies after synchronous disable/drain");
    finish(p, "disable-live-flow");
    if (synthetic_flows)
        synthetic_state_case(tun);
    close(tun);
    puts("COVERAGE: IPv4 active/passive open on local ports 5201/5202, scope and explicit budget mode; "
         "syncookies/IPv6/SACK blocks/loss/reordering untested; "
         "aggregate totals do NOT prove a sliding-window rate/burst bound");
    puts("LOAD_COVERAGE: functional cases and optional SYN-only state; NOT established-connection load validation");
    puts("COMPANION_VM_C_PASS");
    return 0;
}
