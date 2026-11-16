// SPDX-License-Identifier: GPL-2.0
/*
 * Bounded, opt-in TCP redundancy experiment. No congestion-control hooks.
 * Author: lyt
 * Date: 2026-11-17
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/timerqueue.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/tcp.h>
#include <net/net_namespace.h>
#include "policy.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0)
#error "lotspeed_dup requires Linux 4.9 or newer"
#endif
#if !IS_ENABLED(CONFIG_NETFILTER)
#error "lotspeed_dup requires CONFIG_NETFILTER"
#endif

#define DUP_PREFIX (128U * 1024)
#define DUP_PENDING 131072U
#define DUP_FLOW_PENDING 128U
#define DUP_HASH_BITS 15
#define DUP_GC_BUCKETS 256U
#define DUP_IDLE_NS (120ULL * NSEC_PER_SEC)
#define DUP_LATE_NS (100ULL * NSEC_PER_MSEC)
#define DUP_MAX_SEGMENTS 64U

static bool enabled;
static unsigned int port;
static bool all_ports;
static bool budgeted = true;
static unsigned int max_flows = 131072;
static unsigned int max_queue_mb = 64;
static unsigned int delay_ms = 5;
static unsigned int max_mbps = 10;
static unsigned int budget_pct = 10;
module_param(port, uint, 0444);
MODULE_PARM_DESC(port, "Local TCP port, 1..65535; must be 0 with explicit all_ports=1");
module_param(all_ports, bool, 0444);
MODULE_PARM_DESC(all_ports, "Explicit ALL local TCP ports in init_net; requires port=0 (default off)");
module_param(budgeted, bool, 0444);
MODULE_PARM_DESC(budgeted, "Apply ratio/rate limits; off sends every eligible candidate within resource limits");
module_param(max_flows, uint, 0444);
MODULE_PARM_DESC(max_flows, "Global tracked-flow ceiling, 1..262144 (default 131072)");
module_param(max_queue_mb, uint, 0444);
MODULE_PARM_DESC(max_queue_mb, "Global queued skb/item charge limit, 1..512 MiB (default 64)");
module_param(delay_ms, uint, 0444);
MODULE_PARM_DESC(delay_ms, "Minimum delay from observed send/EDT, 1..50 ms (default 5)");
module_param(max_mbps, uint, 0444);
MODULE_PARM_DESC(max_mbps, "Global optional-copy IP-byte rate cap, 1..1000 Mbps (default 10)");
module_param(budget_pct, uint, 0444);
MODULE_PARM_DESC(budget_pct, "Copy credit percent of observed new data IP bytes, 1..100 (default 10)");

struct dup_key {
    struct in6_addr local;
    struct in6_addr remote;
    __be16 local_port;
    __be16 remote_port;
    u32 family;
};

struct dup_flow {
    struct hlist_node hash;
    struct list_head pending;
    struct dup_item *tail;
    struct dup_key key;
    u64 last_seen;
    u32 initial_seq;
    u32 high_seq;
    u32 ack_seq;
    u32 pending_count;
    bool prefix_done;
};

struct dup_item {
    struct timerqueue_node timer;
    struct list_head flow_list;
    struct dup_flow *flow;
    struct sk_buff *skb;
    u32 seq;
    u32 end_seq;
    u32 charge;
};

struct dup_packet {
    struct dup_key key;
    struct tcphdr tcp;
    unsigned char options[40];
    u32 seq;
    u32 ack;
    u32 payload;
    u32 ip_len;
    u32 tcp_len;
    u32 mss;
};

static DEFINE_HASHTABLE(flows, DUP_HASH_BITS);
static DEFINE_SPINLOCK(dup_lock);
static DEFINE_MUTEX(control_lock);
static struct timerqueue_head queue;
static struct lsdup_budget budget;
static unsigned int flow_count;
static unsigned int pending_count;
static unsigned int queue_bytes;
static unsigned int reserved_credit;
static u32 hash_seed;
static unsigned int gc_bucket;
static bool ready;
static void dup_send_work(struct work_struct *work);
static void dup_gc_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(send_work, dup_send_work);
static DECLARE_DELAYED_WORK(gc_work, dup_gc_work);

#define DUP_COUNTER(name) \
    static atomic64_t name = ATOMIC64_INIT(0)
DUP_COUNTER(original_bytes);
DUP_COUNTER(copy_packets);
DUP_COUNTER(copy_bytes);
DUP_COUNTER(cancelled);
DUP_COUNTER(skipped_budget);
DUP_COUNTER(skipped_limit);
DUP_COUNTER(skipped_packet);
DUP_COUNTER(output_errors);
DUP_COUNTER(skipped_late);

/* A single bounded sysfs snapshot avoids per-packet logging. */
static int dup_stats_get(char *buffer, const struct kernel_param *kp)
{
    unsigned int count, pending, bytes;

    spin_lock_bh(&dup_lock);
    count = flow_count;
    pending = pending_count;
    bytes = queue_bytes;
    spin_unlock_bh(&dup_lock);
    return scnprintf(buffer, PAGE_SIZE,
        "flows=%u pending=%u queue_bytes=%u original_ip_bytes=%lld "
        "submitted_copies=%lld submitted_ip_bytes=%lld cancelled=%lld "
        "skipped_budget=%lld skipped_limit=%lld skipped_packet=%lld output_errors=%lld skipped_late=%lld\n",
        count, pending, bytes, atomic64_read(&original_bytes), atomic64_read(&copy_packets),
        atomic64_read(&copy_bytes), atomic64_read(&cancelled), atomic64_read(&skipped_budget),
        atomic64_read(&skipped_limit), atomic64_read(&skipped_packet), atomic64_read(&output_errors),
        atomic64_read(&skipped_late));
}

static const struct kernel_param_ops stats_ops = { .get = dup_stats_get };
module_param_cb(stats, &stats_ops, NULL, 0444);

static u32 dup_hash(const struct dup_key *key)
{
    return jhash(key, sizeof(*key), hash_seed);
}

static struct dup_flow *dup_find(const struct dup_key *key)
{
    struct dup_flow *flow;

    hash_for_each_possible(flows, flow, hash, dup_hash(key)) {
        if (!memcmp(&flow->key, key, sizeof(*key)))
            return flow;
    }
    return NULL;
}

static void dup_unlink(struct dup_item *item)
{
    timerqueue_del(&queue, &item->timer);
    list_del(&item->flow_list);
    if (item->flow->tail == item)
        item->flow->tail = NULL;
    item->flow->pending_count--;
    pending_count--;
    queue_bytes -= item->charge;
    reserved_credit -= item->skb->len;
}

static void dup_discard(struct dup_item *item)
{
    dup_unlink(item);
    kfree_skb(item->skb);
    kfree(item);
    atomic64_inc(&cancelled);
}

static void dup_forget(struct dup_flow *flow)
{
    struct dup_item *item, *next;

    list_for_each_entry_safe(item, next, &flow->pending, flow_list)
        dup_discard(item);
    hash_del(&flow->hash);
    flow_count--;
    kfree(flow);
}

static void dup_purge(void)
{
    struct dup_flow *flow;
    unsigned int bucket, count;

    /* Hooks no longer enroll and workers are stopped. Release the spinlock
     * between small batches so unloading a large table does not stall softirqs.
     */
    for (bucket = 0; bucket < HASH_SIZE(flows); bucket++) {
        bool empty;

        do {
            spin_lock_bh(&dup_lock);
            for (count = 0; count < 8 && !hlist_empty(&flows[bucket]); count++) {
                flow = hlist_entry(flows[bucket].first, struct dup_flow, hash);
                dup_forget(flow);
            }
            empty = hlist_empty(&flows[bucket]);
            spin_unlock_bh(&dup_lock);
            cond_resched();
        } while (!empty);
    }
}

static int dup_enabled_set(const char *value, const struct kernel_param *kp)
{
    bool on;
    int err = kstrtobool(value, &on);

    if (err)
        return err;
    mutex_lock(&control_lock);
    /* Loading is always inert: explicitly enable after inspecting parameters. */
    if (on && (!ready || !lsdup_scope_valid(port, all_ports))) {
        err = -EINVAL;
        goto out;
    }
    if (on == READ_ONCE(enabled))
        goto out;
    if (!on) {
        spin_lock_bh(&dup_lock);
        WRITE_ONCE(enabled, false);
        spin_unlock_bh(&dup_lock);
        cancel_delayed_work_sync(&send_work);
        cancel_delayed_work_sync(&gc_work);
        dup_purge();
    } else {
        spin_lock_bh(&dup_lock);
        lsdup_budget_init(&budget, ktime_get_ns());
        gc_bucket = 0;
        WRITE_ONCE(enabled, true);
        spin_unlock_bh(&dup_lock);
        schedule_delayed_work(&gc_work, HZ);
    }
    pr_info("lotspeed_dup: %s scope=%s port=%u budgeted=%u (init_net only); originals unchanged\n",
            on ? "enabled" : "disabled", all_ports ? "ALL_PORTS" : "SINGLE_PORT", port, budgeted);
out:
    mutex_unlock(&control_lock);
    return err;
}

static const struct kernel_param_ops enabled_ops = {
    .set = dup_enabled_set,
    .get = param_get_bool,
};
module_param_cb(enabled, &enabled_ops, &enabled, 0644);

static bool dup_parse(struct sk_buff *skb, u8 family, bool outgoing, struct dup_packet *p)
{
    struct iphdr ip;
    struct ipv6hdr ip6;
    u32 offset = skb_network_offset(skb), total;
    unsigned int i = 0, opt_len;

    memset(p, 0, sizeof(*p));
    p->key.family = family;
    if (family == NFPROTO_IPV4) {
        if (skb_copy_bits(skb, offset, &ip, sizeof(ip)) || ip.version != 4 ||
            ip.ihl != 5 || ip.protocol != IPPROTO_TCP ||
            (ip.frag_off & htons(IP_MF | IP_OFFSET)))
            return false;
        p->ip_len = sizeof(ip);
        total = ntohs(ip.tot_len);
        p->key.local.s6_addr32[0] = outgoing ? ip.saddr : ip.daddr;
        p->key.remote.s6_addr32[0] = outgoing ? ip.daddr : ip.saddr;
    } else {
        if (skb_copy_bits(skb, offset, &ip6, sizeof(ip6)) || ip6.version != 6 ||
            ip6.nexthdr != IPPROTO_TCP)
            return false;
        p->ip_len = sizeof(ip6);
        total = sizeof(ip6) + ntohs(ip6.payload_len);
        p->key.local = outgoing ? ip6.saddr : ip6.daddr;
        p->key.remote = outgoing ? ip6.daddr : ip6.saddr;
    }
    if (total < p->ip_len + sizeof(p->tcp) || total > skb->len - offset ||
        skb_copy_bits(skb, offset + p->ip_len, &p->tcp, sizeof(p->tcp)))
        return false;
    p->tcp_len = p->tcp.doff * 4;
    if (p->tcp_len < sizeof(p->tcp) || p->tcp_len > sizeof(p->tcp) + sizeof(p->options) ||
        total < p->ip_len + p->tcp_len)
        return false;
    opt_len = p->tcp_len - sizeof(p->tcp);
    if (opt_len && skb_copy_bits(skb, offset + p->ip_len + sizeof(p->tcp), p->options, opt_len))
        return false;
    while (i < opt_len) {
        u8 kind = p->options[i];
        u8 len;

        if (kind == TCPOPT_EOL)
            break;
        if (kind == TCPOPT_NOP) {
            i++;
            continue;
        }
        if (i + 1 >= opt_len || (len = p->options[i + 1]) < 2 || i + len > opt_len)
            return false;
        /* Authentication and MPTCP options cannot be blindly resegmented. */
        if (kind == 19 || kind == 29 || kind == 30)
            return false;
        i += len;
    }
    p->payload = total - p->ip_len - p->tcp_len;
    p->seq = ntohl(p->tcp.seq);
    p->ack = ntohl(p->tcp.ack_seq);
    p->key.local_port = outgoing ? p->tcp.source : p->tcp.dest;
    p->key.remote_port = outgoing ? p->tcp.dest : p->tcp.source;
    p->mss = p->payload;
    if (outgoing && skb_is_gso(skb)) {
        u32 base = family == NFPROTO_IPV4 ? SKB_GSO_TCPV4 : SKB_GSO_TCPV6;
        u32 allowed = base | SKB_GSO_TCP_ECN | SKB_GSO_DODGY;

        if (!skb_shinfo(skb)->gso_size || !(skb_shinfo(skb)->gso_type & base) ||
            (skb_shinfo(skb)->gso_type & ~allowed))
            return false;
        p->mss = skb_shinfo(skb)->gso_size;
    }
    return true;
}

/* Create only the chosen TCP byte range; never segment or mutate the original. */
static struct sk_buff *dup_copy_range(struct sk_buff *original, const struct dup_packet *p,
                                      u32 offset, u32 length)
{
    u32 header = p->ip_len + p->tcp_len;
    u32 total = header + length;
    struct sk_buff *copy;
    struct tcphdr *tcp;
    unsigned char *data;
    struct dst_entry *dst = skb_dst(original);

    if (!dst || total > dst_mtu(dst))
        return NULL;
    copy = alloc_skb(LL_MAX_HEADER + total, GFP_ATOMIC);
    if (!copy)
        return NULL;
    skb_reserve(copy, LL_MAX_HEADER);
    data = skb_put(copy, total);
    if (skb_copy_bits(original, skb_network_offset(original), data, header) ||
        skb_copy_bits(original, skb_network_offset(original) + header + offset, data + header, length)) {
        kfree_skb(copy);
        return NULL;
    }
    skb_reset_network_header(copy);
    skb_set_transport_header(copy, p->ip_len);
    skb_dst_set(copy, dst_clone(dst));
    copy->protocol = p->key.family == NFPROTO_IPV4 ? htons(ETH_P_IP) : htons(ETH_P_IPV6);
    copy->mark = original->mark;
    copy->priority = original->priority;
    copy->ip_summed = CHECKSUM_NONE;
    tcp = tcp_hdr(copy);
    tcp->seq = htonl(p->seq + offset);
    tcp->check = 0;
    if (offset)
        tcp->cwr = 0;
    if (offset + length < p->payload)
        tcp->psh = 0;
    if (p->key.family == NFPROTO_IPV4) {
        struct iphdr *ip = ip_hdr(copy);

        ip->tot_len = htons(total);
        ip->id = htons(ntohs(ip->id) + offset / p->mss);
        tcp->check = tcp_v4_check(p->tcp_len + length, ip->saddr, ip->daddr,
                                csum_partial(tcp, p->tcp_len + length, 0));
        ip_send_check(ip);
    } else {
        struct ipv6hdr *ip = ipv6_hdr(copy);

        ip->payload_len = htons(p->tcp_len + length);
        tcp->check = csum_ipv6_magic(&ip->saddr, &ip->daddr, p->tcp_len + length,
                                   IPPROTO_TCP, csum_partial(tcp, p->tcp_len + length, 0));
    }
    return copy;
}

static void dup_enqueue(struct dup_flow *flow, struct sk_buff *original,
                        const struct dup_packet *p, u32 offset, u32 length, bool tail, u64 now)
{
    struct dup_item *item;
    struct sk_buff *copy;
    u64 edt = ktime_to_ns(original->tstamp);
    u64 due = now + (u64)delay_ms * NSEC_PER_MSEC;

    /* Reserve earned credit before allocating; bulk traffic must not create
     * a large copy queue merely to have the worker reject it later.
     */
    if (budgeted && budget.credit < (u64)reserved_credit + p->ip_len + p->tcp_len + length) {
        atomic64_inc(&skipped_budget);
        return;
    }
    if (pending_count >= DUP_PENDING || flow->pending_count >= DUP_FLOW_PENDING ||
        queue_bytes + SKB_TRUESIZE(LL_MAX_HEADER + p->ip_len + p->tcp_len + length) +
        sizeof(*item) > (max_queue_mb << 20)) {
        atomic64_inc(&skipped_limit);
        return;
    }
    /* Ignore wall-clock timestamps; honor a plausible monotonic future EDT. */
    if (edt > now && edt - now < NSEC_PER_SEC)
        due = edt + (u64)delay_ms * NSEC_PER_MSEC;
    item = kzalloc(sizeof(*item), GFP_ATOMIC);
    if (!item)
        goto failed;
    copy = dup_copy_range(original, p, offset, length);
    if (!copy) {
        kfree(item);
        goto failed;
    }
    item->charge = copy->truesize + sizeof(*item);
    if (queue_bytes + item->charge > (max_queue_mb << 20)) {
        kfree_skb(copy);
        kfree(item);
        goto failed;
    }
    item->flow = flow;
    item->skb = copy;
    item->seq = p->seq + offset;
    item->end_seq = item->seq + length;
    timerqueue_init(&item->timer);
    item->timer.expires = ns_to_ktime(due);
    timerqueue_add(&queue, &item->timer);
    list_add_tail(&item->flow_list, &flow->pending);
    flow->pending_count++;
    pending_count++;
    queue_bytes += item->charge;
    reserved_credit += copy->len;
    if (tail)
        flow->tail = item;
    schedule_delayed_work(&send_work, 1);
    return;
failed:
    atomic64_inc(&skipped_limit);
}

static void dup_cancel_range(struct dup_flow *flow, u32 start, u32 end)
{
    struct dup_item *item, *next;

    list_for_each_entry_safe(item, next, &flow->pending, flow_list) {
        if (lsdup_overlap(item->seq, item->end_seq, start, end))
            dup_discard(item);
    }
}

static void dup_observe_ack(struct dup_flow *flow, const struct dup_packet *p)
{
    unsigned int i = 0, length = p->tcp_len - sizeof(p->tcp);

    /* Observed ACKs are not TCP-validated here; use only bounded forward progress. */
    if (!p->tcp.ack || lsdup_before(flow->high_seq, p->ack) || lsdup_before(p->ack, flow->ack_seq))
        return;
    if (flow->ack_seq != p->ack) {
        dup_cancel_range(flow, flow->ack_seq, p->ack);
        flow->ack_seq = p->ack;
    }
    while (i < length) {
        u8 kind = p->options[i], len;
        unsigned int j;

        if (kind == TCPOPT_EOL)
            break;
        if (kind == TCPOPT_NOP) {
            i++;
            continue;
        }
        len = p->options[i + 1]; /* dup_parse already checked option bounds. */
        if (kind == TCPOPT_SACK && len >= 10 && (len - 2) % 8 == 0) {
            for (j = i + 2; j < i + len; j += 8) {
                __be32 left, right;
                u32 start, end;

                memcpy(&left, &p->options[j], sizeof(left));
                memcpy(&right, &p->options[j + 4], sizeof(right));
                start = ntohl(left);
                end = ntohl(right);
                if (lsdup_before(start, end) && !lsdup_before(start, flow->ack_seq) &&
                    !lsdup_before(flow->high_seq, end))
                    dup_cancel_range(flow, start, end);
            }
        }
        i += len;
    }
}

static unsigned int dup_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    bool outgoing = state->hook == NF_INET_LOCAL_OUT;
    struct dup_packet p;
    struct dup_flow *flow;
    u64 now;
    u32 end, offset, length, count, wire;

    if (!READ_ONCE(enabled))
        return NF_ACCEPT;
    if (outgoing && state->sk && sk_fullsock(state->sk) &&
        state->sk->sk_protocol != IPPROTO_TCP)
        return NF_ACCEPT;
    if (!dup_parse(skb, state->pf, outgoing, &p)) {
        atomic64_inc(&skipped_packet);
        return NF_ACCEPT;
    }
    if (!lsdup_scope_matches(port, all_ports, ntohs(p.key.local_port)))
        return NF_ACCEPT;
    /* Normal SYN-ACKs may own a request_sock, and syncookies have no socket.
     * Permit only these empty handshakes without a full socket. Our data
     * copies have sk=NULL and cannot enroll or recursively create copies.
     */
    if (outgoing && (!state->sk || !sk_fullsock(state->sk)) &&
        !(p.tcp.syn && p.tcp.ack && !p.tcp.rst && !p.tcp.fin && !p.payload))
        return NF_ACCEPT;
    now = ktime_get_ns();
    spin_lock_bh(&dup_lock);
    if (!READ_ONCE(enabled))
        goto out;
    flow = dup_find(&p.key);
    if (outgoing && p.tcp.syn && !p.tcp.rst && !p.tcp.fin && !p.payload) {
        if (flow && flow->initial_seq != p.seq + 1) {
            dup_forget(flow);
            flow = NULL;
        }
        if (!flow && flow_count < max_flows) {
            flow = kzalloc(sizeof(*flow), GFP_ATOMIC);
            if (flow) {
                flow->key = p.key;
                flow->initial_seq = p.seq + 1;
                flow->high_seq = p.seq + 1;
                flow->ack_seq = p.seq + 1;
                INIT_LIST_HEAD(&flow->pending);
                hash_add(flows, &flow->hash, dup_hash(&flow->key));
                flow_count++;
            }
        }
        if (!flow)
            atomic64_inc(&skipped_limit);
    }
    if (!flow)
        goto out;
    flow->last_seen = now;
    if (p.tcp.fin || p.tcp.rst) {
        dup_forget(flow);
        goto out;
    }
    if (!outgoing) {
        dup_observe_ack(flow, &p);
        goto out;
    }
    if (!p.payload || !p.tcp.ack || p.tcp.syn || p.tcp.urg || !p.mss)
        goto out;
    end = p.seq + p.payload;
    if (flow->tail)
        dup_discard(flow->tail);
    if (lsdup_before(p.seq, flow->high_seq)) {
        dup_cancel_range(flow, p.seq, end);
        /* Normal retransmissions never earn credit or become new candidates. */
        if (lsdup_before(flow->high_seq, end))
            flow->high_seq = end;
        goto out;
    }
    flow->high_seq = end;
    count = DIV_ROUND_UP(p.payload, p.mss);
    wire = p.payload + count * (p.ip_len + p.tcp_len);
    atomic64_add(wire, &original_bytes);
    if (budgeted)
        lsdup_budget_earn(&budget, wire, budget_pct);
    length = lsdup_prefix_bytes(p.seq, p.payload, flow->initial_seq + DUP_PREFIX, flow->prefix_done);
    for (offset = 0, count = 0; offset < length && count < DUP_MAX_SEGMENTS; count++) {
        u32 part = min(p.mss, length - offset);

        dup_enqueue(flow, skb, &p, offset, part, false, now);
        offset += part;
    }
    if (offset < length)
        atomic64_add(DIV_ROUND_UP(length - offset, p.mss), &skipped_limit);
    offset = ((p.payload - 1) / p.mss) * p.mss;
    /* Do not overlap prefix candidates, even on the boundary segment. */
    if (!length || offset >= length)
        dup_enqueue(flow, skb, &p, offset, p.payload - offset, true, now);
    if (!lsdup_before(end, flow->initial_seq + DUP_PREFIX))
        flow->prefix_done = true;
out:
    spin_unlock_bh(&dup_lock);
    return NF_ACCEPT;
}

static void dup_send_work(struct work_struct *work)
{
    unsigned int sent;

    for (sent = 0; sent < DUP_MAX_SEGMENTS; sent++) {
        struct timerqueue_node *node;
        struct dup_item *item;
        struct sk_buff *skb;
        u64 now, due;
        u32 family, length;
        int err;

        spin_lock_bh(&dup_lock);
        now = ktime_get_ns();
        node = timerqueue_getnext(&queue);
        if (!READ_ONCE(enabled) || !node || ktime_to_ns(node->expires) > now) {
            if (READ_ONCE(enabled) && node)
                schedule_delayed_work(&send_work, 1);
            spin_unlock_bh(&dup_lock);
            return;
        }
        item = container_of(node, struct dup_item, timer);
        due = ktime_to_ns(node->expires);
        if (now - due > DUP_LATE_NS ||
            (budgeted && !lsdup_budget_take(&budget, item->skb->len, max_mbps, now))) {
            if (now - due > DUP_LATE_NS)
                atomic64_inc(&skipped_late);
            else
                atomic64_inc(&skipped_budget);
            dup_discard(item);
            spin_unlock_bh(&dup_lock);
            continue;
        }
        family = item->flow->key.family;
        skb = item->skb;
        length = skb->len;
        dup_unlink(item);
        kfree(item);
        spin_unlock_bh(&dup_lock);
        /* From this commit boundary, ACKs/disable cannot retract the copy.
         * cancel_delayed_work_sync waits for this submission before disable returns.
         */
        if (family == NFPROTO_IPV4)
            err = ip_local_out(&init_net, NULL, skb);
        else {
#if IS_ENABLED(CONFIG_IPV6)
            err = ip6_local_out(&init_net, NULL, skb);
#else
            kfree_skb(skb);
            err = -EAFNOSUPPORT;
#endif
        }
        atomic64_inc(&copy_packets);
        atomic64_add(length, &copy_bytes);
        if (net_xmit_eval(err))
            atomic64_inc(&output_errors);
    }
    spin_lock_bh(&dup_lock);
    if (READ_ONCE(enabled) && timerqueue_getnext(&queue)) {
        struct timerqueue_node *node = timerqueue_getnext(&queue);
        unsigned long wait = !budgeted && ktime_to_ns(node->expires) <= ktime_get_ns() ? 0 : 1;

        /* Enqueue may already have scheduled a later tick while we ran. */
        if (!wait)
            mod_delayed_work(system_wq, &send_work, 0);
        else
            schedule_delayed_work(&send_work, wait);
    }
    spin_unlock_bh(&dup_lock);
}

static void dup_gc_work(struct work_struct *work)
{
    struct dup_flow *flow;
    struct hlist_node *next;
    unsigned int bucket, count;
    u64 now;

    /* Incremental sweep: never hold the lock across the entire 100k-flow table. */
    for (count = 0; count < DUP_GC_BUCKETS; count++) {
        spin_lock_bh(&dup_lock);
        if (!READ_ONCE(enabled)) {
            spin_unlock_bh(&dup_lock);
            return;
        }
        bucket = gc_bucket++ & (HASH_SIZE(flows) - 1);
        now = ktime_get_ns();
        hlist_for_each_entry_safe(flow, next, &flows[bucket], hash) {
            if (!flow->pending_count && now - flow->last_seen > DUP_IDLE_NS)
                dup_forget(flow);
        }
        spin_unlock_bh(&dup_lock);
        cond_resched();
    }
    spin_lock_bh(&dup_lock);
    if (READ_ONCE(enabled))
        schedule_delayed_work(&gc_work, max_t(unsigned long, 1, msecs_to_jiffies(20)));
    spin_unlock_bh(&dup_lock);
}

static struct nf_hook_ops hooks[] = {
    { .hook = dup_hook, .pf = NFPROTO_IPV4, .hooknum = NF_INET_LOCAL_OUT, .priority = NF_IP_PRI_RAW - 10 },
    { .hook = dup_hook, .pf = NFPROTO_IPV4, .hooknum = NF_INET_LOCAL_IN, .priority = NF_IP_PRI_LAST },
#if IS_ENABLED(CONFIG_IPV6)
    { .hook = dup_hook, .pf = NFPROTO_IPV6, .hooknum = NF_INET_LOCAL_OUT, .priority = NF_IP6_PRI_RAW - 10 },
    { .hook = dup_hook, .pf = NFPROTO_IPV6, .hooknum = NF_INET_LOCAL_IN, .priority = NF_IP6_PRI_LAST },
#endif
};

static int __init dup_init(void)
{
    int err;

    if (!lsdup_scope_valid(port, all_ports) || !delay_ms || delay_ms > 50 ||
        !max_mbps || max_mbps > 1000 || !budget_pct || budget_pct > 100 ||
        !max_flows || max_flows > 262144 || !max_queue_mb || max_queue_mb > 512)
        return -EINVAL;
    timerqueue_init_head(&queue);
    get_random_bytes(&hash_seed, sizeof(hash_seed));
    err = nf_register_net_hooks(&init_net, hooks, ARRAY_SIZE(hooks));
    if (err)
        return err;
    WRITE_ONCE(ready, true);
    pr_info("lotspeed_dup: 3.10.17-experimental loaded DISABLED; scope=%s port=%u, init_net only\n",
            all_ports ? "ALL_PORTS" : "SINGLE_PORT", port);
    pr_info("lotspeed_dup: max_flows=%u flow_bytes=%zu queue_limit=%u MiB budgeted=%u\n",
            max_flows, sizeof(struct dup_flow), max_queue_mb, budgeted);
    return 0;
}

static void __exit dup_exit(void)
{
    mutex_lock(&control_lock);
    WRITE_ONCE(ready, false);
    spin_lock_bh(&dup_lock);
    WRITE_ONCE(enabled, false);
    spin_unlock_bh(&dup_lock);
    nf_unregister_net_hooks(&init_net, hooks, ARRAY_SIZE(hooks));
    synchronize_net();
    cancel_delayed_work_sync(&send_work);
    cancel_delayed_work_sync(&gc_work);
    dup_purge();
    mutex_unlock(&control_lock);
    pr_info("lotspeed_dup: unloaded; no congestion-control or qdisc changes\n");
}

module_init(dup_init);
module_exit(dup_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("lyt");
MODULE_VERSION("3.10.17-experimental");
MODULE_DESCRIPTION("Bounded opt-in TCP prefix/tail copies, independent of LotSpeed CC");
