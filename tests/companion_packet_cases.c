/*
 * Fixtures and assertions around extracted production parser/copy functions.
 * @author lyt
 * @date 2026-11-17
 */
#include "companion/policy.h"

static struct sk_buff *packet(u8 family, u32 payload, u32 options, u32 mss,
                              struct dst_entry *dst)
{
    u32 ip_length = family == NFPROTO_IPV4 ? 20U : 40U;
    u32 tcp_length = 20U + options;
    u32 total = ip_length + tcp_length + payload;
    struct sk_buff *skb = alloc_skb(total + 8U, GFP_ATOMIC);
    struct tcphdr *tcp;
    unsigned char *data;
    u32 i;

    assert(options <= 40 && !(options % 4));
    assert(total <= 65535);
    data = skb_put(skb, total + 8U);
    memset(data, 0, total + 8U);
    skb->network_header = 8;
    skb->transport_header = 8U + ip_length;
    skb->sk = (void *)(uintptr_t)1;
    skb->destructor = (void *)(uintptr_t)2;
    skb->tstamp = 123456789;
    skb->mark = 0x76543210;
    skb->priority = 7;
    skb->ip_summed = CHECKSUM_PARTIAL;
    skb->protocol = htons(family == NFPROTO_IPV4 ? ETH_P_IP : ETH_P_IPV6);
    skb->shared.gso_size = mss;
    skb->shared.gso_type = family == NFPROTO_IPV4 ? SKB_GSO_TCPV4 : SKB_GSO_TCPV6;
    skb->shared.gso_segs = mss ? (payload + mss - 1U) / mss : 0;
    skb_dst_set(skb, dst_clone(dst));
    if (family == NFPROTO_IPV4) {
        struct iphdr *ip = ip_hdr(skb);

        ip->version = 4;
        ip->ihl = 5;
        ip->tos = 0xab;
        ip->tot_len = htons(total);
        ip->id = htons(65534);
        ip->frag_off = htons(0x4000);
        ip->ttl = 53;
        ip->protocol = IPPROTO_TCP;
        ip->saddr = htonl(0xc0000201);
        ip->daddr = htonl(0xc6336402);
        ip->check = htons(0x1234);
    } else {
        struct ipv6hdr *ip = ipv6_hdr(skb);

        ip->version = 6;
        ip->priority = 9;
        ip->flow_lbl[0] = 0xa1;
        ip->flow_lbl[1] = 0x23;
        ip->flow_lbl[2] = 0x45;
        ip->payload_len = htons(tcp_length + payload);
        ip->nexthdr = IPPROTO_TCP;
        ip->hop_limit = 53;
        for (i = 0; i < 16; i++) {
            ip->saddr.s6_addr[i] = (unsigned char)(i + 17);
            ip->daddr.s6_addr[i] = (unsigned char)(i + 101);
        }
    }
    tcp = tcp_hdr(skb);
    tcp->source = htons(12345);
    tcp->dest = htons(443);
    tcp->seq = htonl(0xfffffc00U);
    tcp->ack_seq = htonl(0x12345678);
    tcp->doff = tcp_length / 4;
    tcp->ack = 1;
    tcp->psh = 1;
    tcp->cwr = 1;
    tcp->ece = 1;
    tcp->window = htons(54321);
    tcp->check = htons(0xabcd);
    data = (unsigned char *)(tcp + 1);
    memset(data, TCPOPT_NOP, options);
    if (options >= 12) {
        data[2] = 8;
        data[3] = 10;
        for (i = 4; i < 12; i++)
            data[i] = (unsigned char)(i * 13U);
    }
    for (i = 0; i < payload; i++)
        data[options + i] = (unsigned char)(i * 37U + 11U);
    return skb;
}

static void make_nonlinear(struct sk_buff *skb, u32 head_length)
{
    assert(head_length < skb->len);
    skb->fragment = malloc(skb->len - head_length);
    assert(skb->fragment);
    memcpy(skb->fragment, skb->data + head_length, skb->len - head_length);
    memset(skb->data + head_length, 0xcc, skb->len - head_length);
    skb->head_len = head_length;
}

static void print_hex(const unsigned char *bytes, u32 size)
{
    u32 i;

    for (i = 0; i < size; i++)
        printf("%02x", bytes[i]);
}

static void check_copy(struct sk_buff *original, u8 family, u32 offset, u32 length,
                       const char *label)
{
    struct sk_buff before = *original;
    struct dup_packet p, incoming;
    struct sk_buff *copy;
    unsigned char *snapshot = malloc(original->len);
    unsigned char *after = malloc(original->len);
    u32 refs = original->dst->refs;
    u32 count = live_skbs;
    u32 header;

    assert(snapshot && after);
    assert(!skb_copy_bits(original, 0, snapshot, original->len));
    assert(dup_parse(original, family, true, &p));
    assert(dup_parse(original, family, false, &incoming));
    assert(p.key.local_port == incoming.key.remote_port);
    assert(p.key.remote_port == incoming.key.local_port);
    assert(!memcmp(&p.key.local, &incoming.key.remote, sizeof(p.key.local)));
    assert(!memcmp(&p.key.remote, &incoming.key.local, sizeof(p.key.remote)));
    assert(p.seq == 0xfffffc00U && p.ack == 0x12345678U);
    assert(p.mss == (skb_is_gso(original) ? original->shared.gso_size : p.payload));
    header = p.ip_len + p.tcp_len;
    copy = dup_copy_range(original, &p, offset, length);
    assert(copy && copy != original && copy->head != original->head);
    assert(live_skbs == count + 1 && original->dst->refs == refs + 1);
    assert(copy->len == header + length);
    assert(copy->data - copy->head == LL_MAX_HEADER);
    assert(copy->network_header == 0 && copy->transport_header == p.ip_len);
    assert(copy->sk == NULL && copy->destructor == NULL && copy->tstamp == 0);
    assert(copy->ip_summed == CHECKSUM_NONE);
    assert(!skb_is_gso(copy) && !copy->shared.gso_type && !copy->shared.gso_segs);
    assert(copy->dst == original->dst && copy->fragment == NULL);
    assert(copy->mark == original->mark && copy->priority == original->priority);
    assert(copy->protocol == original->protocol);
    assert(ntohl(tcp_hdr(copy)->seq) == p.seq + offset);
    assert(!memcmp(copy->data + header, snapshot + 8U + header + offset, length));
    assert(!memcmp(copy->data + p.ip_len + 20, p.options, p.tcp_len - 20));
    assert(tcp_hdr(copy)->cwr == (offset == 0));
    assert(tcp_hdr(copy)->psh == (offset + length == p.payload));
    assert(tcp_hdr(copy)->ack && tcp_hdr(copy)->ece);
    assert(!memcmp(&before, original, sizeof(before)));
    assert(!skb_copy_bits(original, 0, after, original->len));
    assert(!memcmp(snapshot, after, original->len));
    printf("{\"family\":%u,\"offset\":%u,\"length\":%u,\"mss\":%u,\"label\":\"%s\",",
           family == NFPROTO_IPV4 ? 4 : 6, offset, length, p.mss, label);
    printf("\"original\":\"");
    print_hex(snapshot + 8, original->len - 8);
    printf("\",\"copy\":\"");
    print_hex(copy->data, copy->len);
    puts("\"}");
    kfree_skb(copy);
    assert(live_skbs == count && original->dst->refs == refs);
    free(snapshot);
    free(after);
}

static void test_construction(void)
{
    const u8 families[] = { NFPROTO_IPV4, NFPROTO_IPV6 };
    const u32 option_sizes[] = { 0, 12, 40 };
    struct dst_entry dst = { 65535, 1 };
    struct sk_buff *skb;
    struct dup_packet p;
    u32 prefix_end, allowed;
    unsigned int f, o;

    assert(sizeof(struct iphdr) == 20 && sizeof(struct ipv6hdr) == 40);
    assert(sizeof(struct tcphdr) == 20);
    for (f = 0; f < 2; f++) {
        for (o = 0; o < 3; o++) {
            skb = packet(families[f], 6001, option_sizes[o], 1460, &dst);
            make_nonlinear(skb, 8U + 21U);
            check_copy(skb, families[f], 0, 1460, "first");
            check_copy(skb, families[f], 1460, 1460, "middle");
            check_copy(skb, families[f], 5840, 161, "last-odd");
            check_copy(skb, families[f], 6000, 1, "last-byte");
            check_copy(skb, families[f], 5840, 160, "before-last-byte");
            assert(dup_parse(skb, families[f], true, &p));
            prefix_end = (p.seq - (128U * 1024U - 1501U)) + 128U * 1024U;
            allowed = lsdup_prefix_bytes(p.seq, p.payload, prefix_end, false);
            assert(allowed == 1501);
            check_copy(skb, families[f], 0, p.mss, "prefix-first");
            check_copy(skb, families[f], p.mss, allowed - p.mss, "prefix-partial");
            kfree_skb(skb);
        }
        skb = packet(families[f], 1460, 12, 0, &dst);
        check_copy(skb, families[f], 0, 1460, "single-even");
        kfree_skb(skb);
        skb = packet(families[f], 1, 0, 0, &dst);
        check_copy(skb, families[f], 0, 1, "single-byte");
        kfree_skb(skb);
    }
    assert(dst.refs == 1 && !live_skbs);
}

static void expect_parse(struct sk_buff *skb, u8 family, bool expected)
{
    struct sk_buff before = *skb;
    struct dup_packet p;
    unsigned char *snapshot = malloc(skb->capacity);

    assert(snapshot);
    memcpy(snapshot, skb->head, skb->capacity);
    assert(dup_parse(skb, family, true, &p) == expected);
    assert(dup_parse(skb, family, false, &p) == expected);
    assert(!memcmp(&before, skb, sizeof(before)));
    assert(!memcmp(snapshot, skb->head, skb->capacity));
    free(snapshot);
}

static void test_parser(void)
{
    const u8 families[] = { NFPROTO_IPV4, NFPROTO_IPV6 };
    const u8 extensions[] = { 0, 43, 44, 50, 51, 60, 59 };
    const u8 auth[] = { 19, 29, 30 };
    struct dst_entry dst = { 1500, 1 };
    struct sk_buff *skb;
    struct dup_packet p;
    unsigned char *options;
    u32 length, n;
    unsigned int f, i;

    for (f = 0; f < 2; f++) {
        skb = packet(families[f], 20, 12, 0, &dst);
        expect_parse(skb, families[f], true);
        length = skb->len;
        for (n = 0; n < length; n++) {
            skb->len = n;
            expect_parse(skb, families[f], false);
        }
        skb->len = length;
        tcp_hdr(skb)->doff = 4;
        expect_parse(skb, families[f], false);
        tcp_hdr(skb)->doff = 15;
        expect_parse(skb, families[f], false);
        tcp_hdr(skb)->doff = 8;
        options = (unsigned char *)(tcp_hdr(skb) + 1);
        for (i = 0; i < 3; i++) {
            memset(options, TCPOPT_NOP, 12);
            options[0] = auth[i];
            options[1] = 4;
            expect_parse(skb, families[f], false);
        }
        for (i = 0; i < 3; i++) {
            options[0] = 8;
            options[1] = i == 2 ? 13 : i;
            expect_parse(skb, families[f], false);
        }
        memset(options, TCPOPT_NOP, 12);
        options[11] = 8;
        expect_parse(skb, families[f], false);
        options[11] = TCPOPT_EOL;
        expect_parse(skb, families[f], true);
        options[0] = TCPOPT_EOL;
        options[1] = 19;
        expect_parse(skb, families[f], true);
        options[0] = 254;
        options[1] = 12;
        expect_parse(skb, families[f], true);
        copy_calls = 0;
        fail_copy_call = 3;
        assert(!dup_parse(skb, families[f], true, &p));
        fail_copy_call = 0;
        if (families[f] == NFPROTO_IPV4) {
            struct iphdr saved = *ip_hdr(skb);

            ip_hdr(skb)->version = 6;
            expect_parse(skb, families[f], false);
            *ip_hdr(skb) = saved;
            ip_hdr(skb)->ihl = 6;
            expect_parse(skb, families[f], false);
            *ip_hdr(skb) = saved;
            ip_hdr(skb)->protocol = IPPROTO_UDP;
            expect_parse(skb, families[f], false);
            *ip_hdr(skb) = saved;
            ip_hdr(skb)->frag_off = htons(IP_MF);
            expect_parse(skb, families[f], false);
            ip_hdr(skb)->frag_off = htons(1);
            expect_parse(skb, families[f], false);
            *ip_hdr(skb) = saved;
            ip_hdr(skb)->tot_len = htons(39);
            expect_parse(skb, families[f], false);
            ip_hdr(skb)->tot_len = htons(40);
            expect_parse(skb, families[f], false);
        } else {
            struct ipv6hdr saved = *ipv6_hdr(skb);

            ipv6_hdr(skb)->version = 4;
            expect_parse(skb, families[f], false);
            *ipv6_hdr(skb) = saved;
            for (i = 0; i < sizeof(extensions); i++) {
                ipv6_hdr(skb)->nexthdr = extensions[i];
                expect_parse(skb, families[f], false);
            }
            *ipv6_hdr(skb) = saved;
            ipv6_hdr(skb)->payload_len = 0;
            expect_parse(skb, families[f], false);
            ipv6_hdr(skb)->payload_len = htons(20);
            expect_parse(skb, families[f], false);
        }
        kfree_skb(skb);
        skb = packet(families[f], 0, 0, 0, &dst);
        expect_parse(skb, families[f], true);
        assert(dup_parse(skb, families[f], true, &p) && !p.payload && !p.mss);
        kfree_skb(skb);
    }
    assert(dst.refs == 1 && !live_skbs);
}

static void test_gso(bool missing_base)
{
    const u8 families[] = { NFPROTO_IPV4, NFPROTO_IPV6 };
    struct dst_entry dst = { 1500, 1 };
    struct sk_buff *skb;
    struct dup_packet p;
    u32 base, other, flags;
    unsigned int f, accepted = 0;

    for (f = 0; f < 2; f++) {
        skb = packet(families[f], 4000, 0, 1000, &dst);
        base = families[f] == NFPROTO_IPV4 ? SKB_GSO_TCPV4 : SKB_GSO_TCPV6;
        other = families[f] == NFPROTO_IPV4 ? SKB_GSO_TCPV6 : SKB_GSO_TCPV4;
        for (flags = 0; flags <= (SKB_GSO_DODGY | SKB_GSO_TCP_ECN); flags += 2) {
            skb->shared.gso_type = missing_base ? flags : base | flags;
            if (missing_base) {
                accepted += dup_parse(skb, families[f], true, &p);
                continue;
            }
            assert(dup_parse(skb, families[f], true, &p));
            assert(p.mss == 1000);
        }
        skb->shared.gso_type = other;
        assert(!dup_parse(skb, families[f], true, &p));
        skb->shared.gso_type = base | other;
        assert(!dup_parse(skb, families[f], true, &p));
        skb->shared.gso_type = base | SKB_GSO_UDP_L4;
        assert(!dup_parse(skb, families[f], true, &p));
        skb->shared.gso_type = base | (1U << 31);
        assert(!dup_parse(skb, families[f], true, &p));
        assert(dup_parse(skb, families[f], false, &p) && p.mss == 4000);
        skb->shared.gso_size = 0;
        assert(dup_parse(skb, families[f], true, &p) && p.mss == 4000);
        kfree_skb(skb);
    }
    assert(dst.refs == 1 && !live_skbs);
    if (missing_base)
        printf("invalid_gso_accepted=%u\n", accepted);
}

static void test_large_gso_wire_accounting(void)
{
    struct dst_entry dst = { 1500, 1 };
    struct sk_buff *skb = packet(NFPROTO_IPV6, 65435, 40, 1, &dst);
    struct dup_packet p;
    struct lsdup_budget budget;
    u32 wire, pct;
    u64 scaled, expected;

    assert(dup_parse(skb, NFPROTO_IPV6, true, &p));
    assert(p.mss == 1);
    wire = p.payload + p.payload * (p.ip_len + p.tcp_len);
    assert(wire == 6608935);
    for (pct = 1; pct <= 100; pct++) {
        lsdup_budget_init(&budget, 0);
        budget.remainder = 99;
        scaled = (u64)wire * pct + 99;
        expected = scaled / 100;
        if (expected > LS_DUP_CREDIT_CAP)
            expected = LS_DUP_CREDIT_CAP;
        lsdup_budget_earn(&budget, wire, pct);
        assert(budget.credit == expected && budget.remainder == scaled % 100);
    }
    kfree_skb(skb);
    assert(dst.refs == 1 && !live_skbs);
}

static void test_failures(void)
{
    const u8 families[] = { NFPROTO_IPV4, NFPROTO_IPV6 };
    struct dst_entry dst = { 1500, 1 };
    struct sk_buff *skb, *copy;
    struct sk_buff before;
    struct dup_packet p;
    unsigned int f, i;
    u32 header;

    for (f = 0; f < 2; f++) {
        skb = packet(families[f], 2000, 12, 1400, &dst);
        assert(dup_parse(skb, families[f], true, &p));
        header = p.ip_len + p.tcp_len;
        before = *skb;
        dst.mtu = header + 999;
        assert(!dup_copy_range(skb, &p, 0, 1000));
        assert(live_skbs == 1 && dst.refs == 2);
        dst.mtu++;
        copy = dup_copy_range(skb, &p, 0, 1000);
        assert(copy);
        kfree_skb(copy);
        skb->dst = NULL;
        assert(!dup_copy_range(skb, &p, 0, 1000));
        skb->dst = &dst;
        fail_alloc = true;
        assert(!dup_copy_range(skb, &p, 0, 1000));
        fail_alloc = false;
        for (i = 1; i <= 2; i++) {
            copy_calls = 0;
            fail_copy_call = i;
            assert(!dup_copy_range(skb, &p, 0, 1000));
            assert(live_skbs == 1 && dst.refs == 2);
        }
        fail_copy_call = 0;
        assert(!dup_copy_range(skb, &p, p.payload, 1));
        assert(!memcmp(&before, skb, sizeof(before)));
        assert(live_skbs == 1 && dst.refs == 2);
        kfree_skb(skb);
    }
    assert(dst.refs == 1 && !live_skbs);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "construction"))
        test_construction();
    else if (!strcmp(argv[1], "parser"))
        test_parser();
    else if (!strcmp(argv[1], "gso"))
        test_gso(false);
    else if (!strcmp(argv[1], "gso-missing-base"))
        test_gso(true);
    else if (!strcmp(argv[1], "failures"))
        test_failures();
    else if (!strcmp(argv[1], "large-wire"))
        test_large_gso_wire_accounting();
    else
        return 2;
    return 0;
}
