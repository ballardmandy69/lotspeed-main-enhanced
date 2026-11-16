/*
 * Userspace skb/dst primitives, not a replacement for packet policy.
 * IP/TCP structs come from installed Linux UAPI headers.
 * @author lyt
 * @date 2026-11-17
 */
#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/if_ether.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

#define NFPROTO_IPV4 2
#define NFPROTO_IPV6 10
#define IP_MF 0x2000
#define IP_OFFSET 0x1fff
#define TCPOPT_EOL 0
#define TCPOPT_NOP 1
#define SKB_GSO_TCPV4 (1U << 0)
#define SKB_GSO_DODGY (1U << 1)
#define SKB_GSO_TCP_ECN (1U << 2)
#define SKB_GSO_TCPV6 (1U << 4)
#define SKB_GSO_UDP_L4 (1U << 17)
#define LL_MAX_HEADER 128U
#define GFP_ATOMIC 0
#define CHECKSUM_NONE 0
#define CHECKSUM_PARTIAL 3

struct dst_entry {
    u32 mtu;
    unsigned int refs;
};

struct skb_shared_info {
    u32 gso_size;
    u32 gso_type;
    u32 gso_segs;
};

struct sk_buff {
    unsigned char *head;
    unsigned char *data;
    unsigned char *fragment;
    u32 capacity;
    u32 len;
    u32 head_len;
    u32 network_header;
    u32 transport_header;
    struct dst_entry *dst;
    struct skb_shared_info shared;
    void *sk;
    void *destructor;
    u64 tstamp;
    u32 mark;
    u32 priority;
    __be16 protocol;
    unsigned int ip_summed;
};

static unsigned int live_skbs;
static bool fail_alloc;
static unsigned int copy_calls;
static unsigned int fail_copy_call;

static struct sk_buff *alloc_skb(u32 size, int flags)
{
    struct sk_buff *skb;

    (void)flags;
    if (fail_alloc)
        return NULL;
    skb = calloc(1, sizeof(*skb));
    assert(skb);
    skb->head = malloc(size);
    assert(skb->head);
    memset(skb->head, 0xa5, size);
    skb->data = skb->head;
    skb->capacity = size;
    live_skbs++;
    return skb;
}

static void kfree_skb(struct sk_buff *skb)
{
    if (skb->dst) {
        assert(skb->dst->refs);
        skb->dst->refs--;
    }
    free(skb->fragment);
    free(skb->head);
    free(skb);
    assert(live_skbs);
    live_skbs--;
}

static void skb_reserve(struct sk_buff *skb, u32 size)
{
    assert(!skb->len && size <= skb->capacity);
    skb->data += size;
}

static unsigned char *skb_put(struct sk_buff *skb, u32 size)
{
    unsigned char *tail = skb->data + skb->len;

    assert((u64)(skb->data - skb->head) + skb->len + size <= skb->capacity);
    skb->len += size;
    skb->head_len += size;
    return tail;
}

static u32 skb_network_offset(const struct sk_buff *skb)
{
    return skb->network_header;
}

static int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int length)
{
    u32 linear;

    copy_calls++;
    if (fail_copy_call && copy_calls == fail_copy_call)
        return -1;
    if (offset < 0 || length < 0 || (u64)(u32)offset + (u32)length > skb->len)
        return -1;
    linear = (u32)offset < skb->head_len ? skb->head_len - (u32)offset : 0;
    if (linear > (u32)length)
        linear = length;
    memcpy(to, skb->data + offset, linear);
    if ((u32)length > linear) {
        assert(skb->fragment);
        memcpy((unsigned char *)to + linear,
               skb->fragment + (u32)offset + linear - skb->head_len, (u32)length - linear);
    }
    return 0;
}

static void skb_reset_network_header(struct sk_buff *skb) { skb->network_header = 0; }
static void skb_set_transport_header(struct sk_buff *skb, u32 n) { skb->transport_header = n; }
static struct dst_entry *skb_dst(const struct sk_buff *skb) { return skb->dst; }
static u32 dst_mtu(const struct dst_entry *dst) { return dst->mtu; }
static struct dst_entry *dst_clone(struct dst_entry *dst) { dst->refs++; return dst; }
static void skb_dst_set(struct sk_buff *skb, struct dst_entry *dst) { skb->dst = dst; }
static bool skb_is_gso(const struct sk_buff *skb) { return skb->shared.gso_size != 0; }
#define skb_shinfo(skb) (&(skb)->shared)
#define ip_hdr(skb) ((struct iphdr *)((skb)->data + (skb)->network_header))
#define ipv6_hdr(skb) ((struct ipv6hdr *)((skb)->data + (skb)->network_header))
#define tcp_hdr(skb) ((struct tcphdr *)((skb)->data + (skb)->transport_header))

/* Semantic adapters for kernel checksum primitives; Python checks wire bytes
 * independently, with its own pseudo-header and one's-complement arithmetic.
 */
static u32 csum_partial(const void *data, unsigned int length, u32 sum)
{
    const unsigned char *bytes = data;
    unsigned int i;

    for (i = 0; i + 1 < length; i += 2)
        sum += ((u32)bytes[i] << 8) | bytes[i + 1];
    if (length & 1)
        sum += (u32)bytes[length - 1] << 8;
    return sum;
}

static __sum16 shim_fold(u32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static __sum16 tcp_v4_check(u32 length, __be32 source, __be32 dest, u32 sum)
{
    sum = csum_partial(&source, sizeof(source), sum);
    sum = csum_partial(&dest, sizeof(dest), sum);
    return shim_fold(sum + IPPROTO_TCP + length);
}

static void ip_send_check(struct iphdr *ip)
{
    ip->check = 0;
    ip->check = shim_fold(csum_partial(ip, ip->ihl * 4, 0));
}

static __sum16 csum_ipv6_magic(const struct in6_addr *source, const struct in6_addr *dest,
                             u32 length, u8 protocol, u32 sum)
{
    sum = csum_partial(source, sizeof(*source), sum);
    sum = csum_partial(dest, sizeof(*dest), sum);
    return shim_fold(sum + (length >> 16) + (length & 0xffffU) + protocol);
}
