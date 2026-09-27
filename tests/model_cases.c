static void round_sample(struct sock *sk, u32 delivered, u32 lost,
                         bool app_limited)
{
    struct rate_sample rs = {
        .prior_delivered = sk->tcp.delivered,
        .is_app_limited = app_limited,
    };
    sk->tcp.delivered += delivered;
    sk->tcp.lost += lost;
    assert(lotspeed_update_round_model(sk, &rs, 1440, 50000));
}

static void test_retained_samples(void)
{
    struct sock sk;
    u32 delivered = 0, lost = 0;
    init_test(&sk);
    sk.tcp.lost = 7;
    assert(!lotspeed_sample_loss(&sk, 0, &delivered, &lost));
    assert(sk.ca.loss_lost == 0);
    sk.tcp.delivered = 3;
    assert(!lotspeed_sample_loss(&sk, 0, &delivered, &lost));
    advance_ms(20);
    assert(lotspeed_sample_loss(&sk, tcp_jiffies32, &delivered, &lost));
    assert(delivered == 3 && lost == 7);
    advance_ms(100);
    assert(!lotspeed_sample_loss(&sk, tcp_jiffies32, &delivered, &lost));
    assert(sk.ca.loss_lost == 7);

    init_test(&sk);
    round_sample(&sk, 0, 20, true);
    assert(sk.ca.loss_lost == 0);
    lotspeed_cwnd_event(&sk, CA_EVENT_TX_START);
    assert(sk.ca.loss_lost == 0);
    advance_ms(100);
    round_sample(&sk, 100, 0, true);
    assert(sk.ca.loss_ewma > 0 && sk.ca.loss_lost == 20);
    advance_ms(100);
    u32 previous = sk.ca.loss_ewma;
    round_sample(&sk, 100, 0, true);
    assert(sk.ca.loss_ewma < previous);
}

static void test_single_burst_and_persistent_loss(void)
{
    struct sock sk;
    init_test(&sk);
    lotserver_loss_adapt_samples = 3;
    advance_ms(100);
    round_sample(&sk, 100, 100, false);
    assert(sk.ca.loss_adapt_count == 1);
    for (int i = 0; i < 30; ++i) {
        advance_ms(100);
        round_sample(&sk, 100, 0, false);
        assert(sk.ca.loss_adapt_count <= 1);
        assert(sk.ca.path_mode != PATH_CONGESTED);
    }
    assert(sk.ca.loss_adapt_count == 0);

    for (int app = 0; app < 2; ++app) {
        init_test(&sk);
        for (int i = 0; i < 50; ++i) {
            advance_ms(100);
            round_sample(&sk, 95, 5, app);
        }
        assert(sk.ca.loss_adapt_count == 5);
        assert(sk.ca.path_mode == PATH_CONGESTED);
        for (int i = 0; i < 50; ++i) {
            advance_ms(100);
            round_sample(&sk, 100, 0, app);
        }
        assert(sk.ca.loss_adapt_count == 0);
        assert(sk.ca.path_mode == PATH_STABLE);
    }

    init_test(&sk);
    for (int i = 0; i < 50; ++i) {
        advance_ms(100);
        round_sample(&sk, 999, 1, false);
    }
    assert(sk.ca.path_mode == PATH_STABLE);
    assert(sk.ca.loss_adapt_count == 0);
}

static void test_fresh_loss_entry(void)
{
    struct sock sk;
    init_test(&sk);
    lotserver_loss_adapt_samples = 2;
    /* 3 / (100 + 3) is below the moderate threshold, even after warm-up. */
    for (int i = 0; i < 50; ++i) {
        advance_ms(100);
        round_sample(&sk, 100, 3, false);
        assert(sk.ca.loss_adapt_count == 0);
        assert(sk.ca.path_mode == PATH_STABLE);
    }
    for (int app = 0; app < 2; ++app) {
        init_test(&sk);
        lotserver_loss_adapt_samples = 2;
        for (int i = 0; i < 2; ++i) {
            advance_ms(100);
            round_sample(&sk, 97, 3, app);
            assert(sk.ca.loss_adapt_count == i + 1);
            assert(sk.ca.loss_ewma <
                   lotserver_loss_adapt_pct * LOTSPEED_LOSS_SCALE / 100);
            assert((sk.ca.path_mode == PATH_CONGESTED) == (i == 1));
        }
        /* Keep the existing fast recovery after fresh moderate entry. */
        advance_ms(100);
        round_sample(&sk, 100, 0, app);
        assert(sk.ca.loss_adapt_count == 0);
        assert(sk.ca.path_mode == PATH_STABLE);
    }

    init_test(&sk);
    lotserver_loss_adapt_samples = 2;
    for (int i = 0; i < 2; ++i) {
        advance_ms(100);
        round_sample(&sk, 7, 1, false);
        assert(sk.ca.loss_adapt_count == 0);
        assert(sk.ca.path_mode == PATH_STABLE);
    }
    advance_ms(100);
    round_sample(&sk, 8, 1, false);
    assert(sk.ca.loss_adapt_count == 1);

    init_test(&sk);
    lotserver_loss_adapt_samples = 2;
    advance_ms(100);
    round_sample(&sk, 97, 3, false);
    struct rate_sample duplicate = { .prior_delivered = 0 };
    for (int i = 0; i < 10; ++i) {
        advance_ms(10);
        assert(!lotspeed_update_round_model(&sk, &duplicate, 1440, 50000));
        assert(sk.ca.loss_adapt_count == 1);
    }
    advance_ms(100);
    round_sample(&sk, 100, 0, false);
    advance_ms(100);
    round_sample(&sk, 97, 3, false);
    assert(sk.ca.loss_adapt_count == 1);
    assert(sk.ca.path_mode == PATH_STABLE);

    /* The runtime threshold still controls entry; this is not a flow quota. */
    init_test(&sk);
    lotserver_loss_adapt_pct = 4;
    lotserver_loss_adapt_samples = 1;
    advance_ms(100);
    round_sample(&sk, 97, 3, false);
    assert(sk.ca.path_mode == PATH_STABLE);
    advance_ms(100);
    round_sample(&sk, 96, 4, false);
    assert(sk.ca.path_mode == PATH_CONGESTED);
}

static void test_idle_vs_backlog(void)
{
    struct sock sk;
    init_test(&sk);
    sk.ca.state = AVOIDING;
    sk.ca.path_mode = PATH_CONGESTED;
    sk.ca.loss_ewma = 100;
    sk.ca.loss_adapt_count = 5;
    sk.ca.actual_rate = 1000000;
    sk.ca.target_rate = lotserver_rate / 2;
    sk.tcp.write_seq = 20000;
    sk.tcp.packets_out = 10;
    for (int i = 0; i < 40; ++i) {
        advance_ms(1000);
        lotspeed_update_mux_activity(&sk, tcp_jiffies32);
    }
    assert(sk.ca.loss_adapt_count == 5);
    assert(sk.ca.actual_rate == 1000000);
    assert(sk.ca.state == AVOIDING);
    assert(!sk.ca.mux_drained);

    // Recovery TX_START is not proof of idle, even with no estimated flight.
    advance_ms(11000);
    lotspeed_cwnd_event(&sk, CA_EVENT_TX_START);
    assert(sk.ca.state == AVOIDING && sk.ca.loss_adapt_count == 5);

    // Unsent data alone also prevents reset.
    sk.tcp.packets_out = 0;
    advance_ms(11000);
    lotspeed_update_mux_activity(&sk, tcp_jiffies32);
    assert(sk.ca.state == AVOIDING);
    // Outstanding data prevents reset even if sequence counters coincide.
    sk.tcp.snd_una = sk.tcp.write_seq;
    sk.tcp.packets_out = 1;
    advance_ms(11000);
    lotspeed_update_mux_activity(&sk, tcp_jiffies32);
    assert(sk.ca.state == AVOIDING);

    sk.tcp.packets_out = 0;
    lotspeed_update_mux_activity(&sk, tcp_jiffies32);
    assert(sk.ca.mux_drained);
    advance_ms(5000);
    lotspeed_update_mux_activity(&sk, tcp_jiffies32);
    assert(sk.ca.state == AVOIDING);
    advance_ms(5100);
    // A new AnyTLS write may already be queued when TX_START is called.
    sk.tcp.write_seq += 1000;
    lotspeed_cwnd_event(&sk, CA_EVENT_TX_START);
    assert(sk.ca.state == CRUISING);
    assert(sk.ca.path_mode == PATH_STABLE);
    assert(sk.ca.target_rate == lotserver_rate);
    assert(sk.ca.actual_rate == 0 && sk.ca.loss_ewma == 0);
    assert(!sk.ca.mux_drained);
}

static void test_expiry_wrap_and_confirmation_settings(void)
{
    struct sock sk;
    u32 delivered = 0, lost = 0;
    init_test(&sk);
    sk.ca.loss_ewma = 100;
    sk.ca.loss_adapt_count = 4;
    sk.ca.path_mode = PATH_CONGESTED;
    sk.tcp.lost = 100;
    advance_ms(2100);
    assert(!lotspeed_sample_loss(&sk, tcp_jiffies32, &delivered, &lost));
    assert(sk.ca.loss_ewma == 0 && sk.ca.loss_adapt_count == 0);
    assert(sk.ca.path_mode == PATH_CONGESTED); // no invented healthy sample
    advance_ms(100);
    round_sample(&sk, 100, 0, true);
    assert(sk.ca.path_mode == PATH_STABLE);

    init_test(&sk);
    sk.ca.loss_delivered = UINT32_MAX - 2;
    sk.ca.loss_lost = UINT32_MAX - 1;
    sk.ca.loss_stamp = UINT32_MAX - 2;
    sk.tcp.delivered = 2;
    sk.tcp.lost = 1;
    assert(lotspeed_sample_loss(&sk, 2, &delivered, &lost));
    assert(delivered == 5 && lost == 3);

    for (unsigned int samples = 1; samples <= 255; ++samples) {
        init_test(&sk);
        lotserver_loss_adapt_samples = samples;
        sk.ca.loss_ewma = 60;
        for (unsigned int i = 1; i <= samples; ++i) {
            advance_ms(100);
            round_sample(&sk, 94, 6, true);
            assert(sk.ca.loss_adapt_count == i);
            assert((sk.ca.path_mode == PATH_CONGESTED) == (i == samples));
        }
    }
}

static void init_app_loss_test(struct sock *sk)
{
    init_test(sk);
    sk->ca.actual_rate = lotserver_rate;
    sk->ca.state = AVOIDING;
    sk->ca.path_mode = PATH_CONGESTED;
    sk->ca.loss_ewma = LOTSPEED_LOSS_SCALE / 2;
    sk->tcp.write_seq = 1024 * 1024;
}

static void app_loss_round(struct sock *sk, u32 ms, u32 delivered,
                            u32 lost, u32 retrans)
{
    advance_ms(ms);
    sk->tcp.total_retrans += retrans;
    round_sample(sk, delivered, lost, true);
}

static void test_app_limited_ordinary_and_moderate(void)
{
    struct sock sk;
    init_test(&sk);
    sk.ca.actual_rate = lotserver_rate;
    for (int i = 0; i < 150; ++i) {
        app_loss_round(&sk, 100, 50, 0, 0);
        assert(sk.ca.actual_rate == lotserver_rate);
        assert(sk.ca.app_loss_ms == 0);
    }
    // Higher app-limited samples and lower non-app-limited samples still work.
    app_loss_round(&sk, 100, 5000, 0, 0);
    assert(sk.ca.actual_rate > lotserver_rate);
    u64 prior = sk.ca.actual_rate;
    advance_ms(100);
    round_sample(&sk, 50, 0, false);
    assert(sk.ca.actual_rate < prior);

    init_app_loss_test(&sk);
    sk.ca.loss_ewma = 0;
    lotserver_loss_adapt_pct = 1;
    lotserver_loss_adapt_samples = 1;
    for (int i = 0; i < 150; ++i) {
        app_loss_round(&sk, 100, 99, 1, 1);
        assert(sk.ca.actual_rate == lotserver_rate);
        assert(sk.ca.app_loss_ms == 0);
    }
    assert(sk.ca.path_mode == PATH_CONGESTED);
}

static void test_app_limited_severe_learning_and_recovery(void)
{
    struct sock sk;
    init_app_loss_test(&sk);
    for (int i = 0; i < 100; ++i) {
        app_loss_round(&sk, 100, 50, 50, 50);
        assert(sk.ca.actual_rate == lotserver_rate);
    }
    sk.ca.extra_acked = 80;
    app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.app_loss_ms == LOTSPEED_APP_LOSS_HOLD_MS + 1);
    assert(sk.ca.actual_rate == (lotserver_rate * 7ULL + 720000) / 8);
    assert(sk.ca.extra_acked == 70); // no bypass of ACK-aggregation protection
    for (int i = 0; i < 40; ++i)
        app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.actual_rate < lotserver_rate / 10);

    u64 prior = sk.ca.actual_rate;
    app_loss_round(&sk, 100, 50, 0, 0);
    assert(sk.ca.app_loss_ms == LOTSPEED_APP_LOSS_HOLD_MS + 1);
    assert(sk.ca.app_loss_gap_ms == 100);
    assert(sk.ca.actual_rate == prior);
    // One quiet RTT pauses learning without discarding sustained evidence.
    app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.actual_rate < prior);
    assert(sk.ca.app_loss_gap_ms == 0);
    app_loss_round(&sk, 100, 5000, 0, 0);
    assert(sk.ca.actual_rate > prior);
    for (int i = 0; i < 20; ++i)
        app_loss_round(&sk, 100, 50, 0, 0);
    assert(sk.ca.app_loss_ms == 0);
    assert(sk.ca.app_loss_gap_ms == 0);

    sk.ca.app_loss_ms = LOTSPEED_APP_LOSS_HOLD_MS + 1;
    sk.ca.app_loss_gap_ms = 100;
    lotspeed_reset_mux_history(&sk, tcp_jiffies32);
    assert(sk.ca.app_loss_ms == 0 && sk.ca.actual_rate == 0);
    assert(sk.ca.app_loss_gap_ms == 0);
    assert(sk.ca.app_loss_retrans == (u16)sk.tcp.total_retrans);

    // Production starts without a prewarmed EWMA; severe traffic must still qualify.
    init_app_loss_test(&sk);
    sk.ca.loss_ewma = 0;
    for (int i = 0; i < 160; ++i)
        app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.actual_rate < lotserver_rate / 10);
}

static void test_app_limited_rejects_weak_or_stale_evidence(void)
{
    struct sock sk;
    for (int scenario = 0; scenario < 12; ++scenario) {
        init_app_loss_test(&sk);
        sk.ca.app_loss_ms = LOTSPEED_APP_LOSS_HOLD_MS + 1;
        u32 ms = 100, delivered = 50, lost = 50, retrans = 50;
        switch (scenario) {
        case 0: lotserver_adaptive = false; break;
        case 1: lotserver_turbo = true; break;
        case 2: sk.tcp.snd_nxt = sk.tcp.write_seq; break;
        case 3: sk.tcp.snd_nxt = sk.tcp.write_seq - 1439; break;
        case 4: retrans = 0; break;
        case 5: sk.ca.path_mode = PATH_STABLE; sk.ca.loss_ewma = 0; lost = 0; break;
        case 6: delivered = 7; lost = 7; break;
        case 7: ms = 2100; break;
        case 8: delivered = 0; break;
        case 9: ms = 0; break;
        case 10: lotserver_loss_congest_pct = 60; break;
        case 11:
            lotserver_loss_congest_pct = 1;
            lotserver_loss_recover_pct = 0;
            sk.ca.loss_ewma = LOTSPEED_LOSS_SCALE / 10;
            delivered = 90;
            lost = 10;
            break;
        }
        app_loss_round(&sk, ms, delivered, lost, retrans);
        assert(sk.ca.actual_rate == lotserver_rate);
        if (scenario <= 3 || scenario == 5 || scenario == 7) {
            assert(sk.ca.app_loss_ms == 0 && sk.ca.app_loss_gap_ms == 0);
        } else {
            assert(sk.ca.app_loss_ms == LOTSPEED_APP_LOSS_HOLD_MS + 1);
            assert(sk.ca.app_loss_gap_ms == ms);
        }
    }

    // Backlog disappearing between packet-timed rounds must invalidate the hold.
    init_app_loss_test(&sk);
    sk.ca.app_loss_ms = LOTSPEED_APP_LOSS_HOLD_MS + 1;
    sk.tcp.snd_nxt = sk.tcp.write_seq;
    assert(!lotspeed_update_round_model(&sk, NULL, 1440, 50000));
    assert(sk.ca.app_loss_ms == 0);
}

static void test_app_limited_counter_wrap(void)
{
    struct sock sk;
    for (int scenario = 0; scenario < 4; ++scenario) {
        init_app_loss_test(&sk);
        sk.ca.app_loss_ms = LOTSPEED_APP_LOSS_HOLD_MS + 1;
        sk.tcp.total_retrans = scenario ? UINT32_MAX - 10 : 65530;
        sk.ca.app_loss_retrans = (u16)sk.tcp.total_retrans;
        if (scenario == 2) {
            tcp_jiffies32 = UINT32_MAX - msecs_to_jiffies(50);
            sk.ca.round_stamp = tcp_jiffies32;
            sk.ca.loss_stamp = tcp_jiffies32;
            sk.tcp.snd_nxt = UINT32_MAX - 2000;
            sk.tcp.write_seq = 1000;
        }
        app_loss_round(&sk, 100, 50, 50, scenario == 3 ? 65536 : 50);
        if (scenario == 3) {
            // A low-bit collision must fail closed, never invent fresh evidence.
            assert(sk.ca.actual_rate == lotserver_rate);
            assert(sk.ca.app_loss_gap_ms == 100);
        } else {
            assert(sk.ca.actual_rate < lotserver_rate);
            assert(sk.ca.app_loss_ms == LOTSPEED_APP_LOSS_HOLD_MS + 1);
        }
    }
}

static void test_app_limited_outstanding_retransmissions(void)
{
    struct sock sk;
    init_app_loss_test(&sk);
    sk.tcp.snd_nxt = sk.tcp.write_seq;
    sk.tcp.packets_out = 1000;
    for (int i = 0; i < 100; ++i) {
        app_loss_round(&sk, 100, 50, 50, 50);
        assert(sk.ca.actual_rate == lotserver_rate);
    }
    for (int i = 0; i < 60; ++i)
        app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.actual_rate < lotserver_rate / 10);

    // Pending old data between callbacks is not a drained application.
    assert(!lotspeed_update_round_model(&sk, NULL, 1440, 50000));
    assert(sk.ca.app_loss_ms == LOTSPEED_APP_LOSS_HOLD_MS + 1);
    sk.tcp.snd_una = sk.tcp.snd_nxt;
    sk.tcp.packets_out = 0;
    assert(!lotspeed_update_round_model(&sk, NULL, 1440, 50000));
    assert(sk.ca.app_loss_ms == 0 && sk.ca.app_loss_gap_ms == 0);
}

static void test_app_loss_backlog_boundaries(void)
{
    struct sock sk;
    init_test(&sk);
    assert(!lotspeed_has_app_loss_backlog(&sk.tcp, 1440));
    sk.tcp.write_seq = 1440;
    assert(lotspeed_has_app_loss_backlog(&sk.tcp, 1440));
    sk.tcp.write_seq--;
    assert(!lotspeed_has_app_loss_backlog(&sk.tcp, 1440));

    sk.tcp.snd_nxt = sk.tcp.write_seq = 8 * 1440;
    sk.tcp.packets_out = 8;
    assert(lotspeed_has_app_loss_backlog(&sk.tcp, 1440));
    sk.tcp.packets_out = 7;
    assert(!lotspeed_has_app_loss_backlog(&sk.tcp, 1440));
    sk.tcp.packets_out = 8;
    sk.tcp.snd_una = 1;
    assert(!lotspeed_has_app_loss_backlog(&sk.tcp, 1440));
    sk.tcp.snd_una = sk.tcp.snd_nxt;
    assert(!lotspeed_has_app_loss_backlog(&sk.tcp, 1440));

    sk.tcp.snd_una = UINT32_MAX - 1000;
    sk.tcp.snd_nxt = sk.tcp.write_seq = sk.tcp.snd_una + 8U * 1440;
    assert(lotspeed_has_app_loss_backlog(&sk.tcp, 1440));
    assert(!lotspeed_has_app_loss_backlog(&sk.tcp, 0));
    assert(!lotspeed_has_app_loss_backlog(&sk.tcp, UINT32_MAX));
}

static void test_app_loss_short_gaps_and_expiry(void)
{
    struct sock sk;
    init_app_loss_test(&sk);
    for (int i = 0; i < 40; ++i)
        app_loss_round(&sk, 100, 50, 50, 50);
    u16 confirmed = sk.ca.app_loss_ms;
    app_loss_round(&sk, 300, 50, 0, 0);
    assert(sk.ca.app_loss_ms == confirmed && sk.ca.app_loss_gap_ms == 300);
    assert(sk.ca.actual_rate == lotserver_rate);
    app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.app_loss_ms == confirmed + 100 && sk.ca.app_loss_gap_ms == 0);

    // Paused time never counts toward the ten seconds of qualified evidence.
    init_app_loss_test(&sk);
    sk.tcp.snd_nxt = sk.tcp.write_seq;
    sk.tcp.packets_out = 1000;
    unsigned int qualified = 0;
    for (int i = 0; i < 200; ++i) {
        bool quiet = (i % 5 == 4);
        app_loss_round(&sk, 100, 50, quiet ? 0 : 50, quiet ? 0 : 50);
        if (!quiet)
            qualified++;
        if (qualified <= 100)
            assert(sk.ca.actual_rate == lotserver_rate);
    }
    assert(sk.ca.actual_rate < lotserver_rate / 10);

    // New retransmissions can concern packets already marked lost earlier.
    sk.ca.loss_ewma = LOTSPEED_LOSS_SCALE;
    u64 prior = sk.ca.actual_rate;
    app_loss_round(&sk, 100, 50, 0, 50);
    assert(sk.ca.actual_rate < prior);

    // Historical severe loss without actual retransmissions cannot learn down.
    prior = sk.ca.actual_rate;
    for (int i = 0; i < 20; ++i) {
        app_loss_round(&sk, 100, 50, 50, 0);
        assert(sk.ca.actual_rate == prior);
    }
    assert(sk.ca.app_loss_ms == 0 && sk.ca.app_loss_gap_ms == 0);
    app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.app_loss_ms == 1 && sk.ca.actual_rate == prior);

    // A stale loss window and a single long callback gap both discard history.
    sk.ca.app_loss_ms = 5000;
    sk.ca.app_loss_gap_ms = 100;
    u32 delivered = 0, lost = 0;
    advance_ms(2100);
    assert(!lotspeed_sample_loss(&sk, tcp_jiffies32, &delivered, &lost));
    assert(sk.ca.app_loss_ms == 0 && sk.ca.app_loss_gap_ms == 0);
    init_app_loss_test(&sk);
    sk.ca.app_loss_ms = 5000;
    sk.ca.app_loss_gap_ms = 100;
    app_loss_round(&sk, 2100, 50, 50, 50);
    assert(sk.ca.app_loss_ms == 0 && sk.ca.app_loss_gap_ms == 0);
}

static void test_app_loss_small_or_healthy_outstanding(void)
{
    struct sock sk;
    for (int scenario = 0; scenario < 4; ++scenario) {
        init_app_loss_test(&sk);
        sk.tcp.snd_nxt = sk.tcp.write_seq = 8 * 1440;
        sk.tcp.packets_out = scenario == 0 ? 7 : 8;
        if (scenario == 1)
            sk.tcp.snd_una = 1;
        if (scenario >= 2) {
            sk.ca.loss_ewma = 0;
            sk.ca.path_mode = PATH_STABLE;
        }
        for (int i = 0; i < 160; ++i) {
            u32 loss = scenario < 2 ? 50 : scenario == 2 ? 0 : 1;
            app_loss_round(&sk, 100, 99, loss, loss);
            assert(sk.ca.actual_rate == lotserver_rate);
            assert(sk.ca.app_loss_ms == 0);
        }
    }
}

static void test_app_loss_high_rate_outstanding_pattern(void)
{
    struct sock sk;
    unsigned long saved_rate = lotserver_rate;
    unsigned int saved_floor = lotserver_min_rate_pct;
    unsigned int qualified_ms = 0;
    u64 elapsed_us = jiffies_to_usecs(msecs_to_jiffies(50));
    u64 round_rate = 360ULL * 1440 * USEC_PER_SEC / elapsed_us;
    lotserver_rate = 180000000;
    lotserver_min_rate_pct = 8;
    init_app_loss_test(&sk);
    sk.ca.actual_rate = 112500000; // stale 900 Mbps estimate
    sk.ca.loss_ewma = 0;
    sk.tcp.snd_nxt = sk.tcp.write_seq = 14 * 1024 * 1024;
    sk.tcp.packets_out = 10000;

    // Synthetic analogue of the capture, not a replay of unobserved callbacks.
    for (int i = 0; i < 400; ++i) {
        bool quiet = (i % 5 == 4);
        u64 prior = sk.ca.actual_rate;
        app_loss_round(&sk, 50, 360, quiet ? 0 : 2040, quiet ? 0 : 2040);
        if (!quiet)
            qualified_ms += elapsed_us / 1000;
        if (qualified_ms <= LOTSPEED_APP_LOSS_HOLD_MS)
            assert(sk.ca.actual_rate == 112500000);
        if (quiet)
            assert(sk.ca.actual_rate == prior);
        assert(sk.ca.loss_ewma <= LOTSPEED_LOSS_SCALE);
    }
    assert(sk.ca.path_mode == PATH_CONGESTED);
    assert(sk.ca.actual_rate >= round_rate && sk.ca.actual_rate < round_rate * 106 / 100);
    assert(lotspeed_adaptive_floor() == 14400000); // existing 115.2 Mbps floor
    assert(lotspeed_scale_percent(sk.ca.actual_rate, 105) < lotspeed_adaptive_floor());
    lotserver_rate = saved_rate;
    lotserver_min_rate_pct = saved_floor;
}

static void control_sample(struct sock *sk, u32 delivered, u32 lost)
{
    struct rate_sample rs = {
        .prior_delivered = sk->tcp.delivered,
        .is_app_limited = true,
        .rtt_us = 50000,
        .acked_sacked = delivered,
        .losses = lost,
    };
    sk->tcp.delivered += delivered;
    sk->tcp.lost += lost;
    lotspeed_adapt_and_control(sk, &rs, 0);
}

static void test_severe_stall_scope_and_boundaries(void)
{
    struct sock sk;
    for (int scenario = 0; scenario < 15; ++scenario) {
        u32 delivered = 0, lost = 0;
        bool retain = true;
        init_app_loss_test(&sk);
        sk.ca.loss_adapt_count = 1;
        sk.ca.app_loss_ms = LOTSPEED_APP_LOSS_HOLD_MS + 1;
        sk.ca.app_loss_gap_ms = 100;
        sk.tcp.delivered = 50;
        sk.tcp.lost = 100;
        switch (scenario) {
        case 1:
            sk.tcp.snd_nxt = sk.tcp.write_seq;
            sk.tcp.packets_out = 1000;
            break;
        case 2:
            sk.tcp.snd_una = sk.tcp.write_seq;
            sk.tcp.packets_out = 1;
            break;
        case 3: sk.tcp.write_seq = 1; break;
        case 4: sk.tcp.write_seq = 0; retain = false; break;
        case 5: sk.ca.loss_ewma = 100; retain = false; break;
        case 6: sk.ca.path_mode = PATH_STABLE; retain = false; break;
        case 7: sk.ca.path_mode = PATH_JITTERY; retain = false; break;
        case 8: lotserver_adaptive = false; retain = false; break;
        case 9: lotserver_turbo = true; retain = false; break;
        case 10:
            lotserver_loss_congest_pct = 60;
            sk.ca.loss_ewma = 59 * LOTSPEED_LOSS_SCALE / 100;
            retain = false;
            break;
        case 11:
            lotserver_loss_congest_pct = 1;
            sk.ca.loss_ewma = 20 * LOTSPEED_LOSS_SCALE / 100;
            retain = false;
            break;
        case 12:
            sk.ca.loss_ewma = 30 * LOTSPEED_LOSS_SCALE / 100;
            break;
        case 13:
            lotserver_loss_congest_pct = 60;
            sk.ca.loss_ewma = 60 * LOTSPEED_LOSS_SCALE / 100;
            break;
        case 14:
            tcp_jiffies32 = UINT32_MAX - msecs_to_jiffies(1000);
            sk.ca.loss_stamp = tcp_jiffies32;
            break;
        }
        u16 previous_ewma = sk.ca.loss_ewma;
        advance_ms(2100);
        assert(!lotspeed_sample_loss(&sk, tcp_jiffies32, &delivered, &lost));
        assert(sk.ca.loss_ewma == (retain ? previous_ewma : 0));
        assert(sk.ca.loss_adapt_count == (retain ? 1 : 0));
        assert(sk.ca.app_loss_ms == 0 && sk.ca.app_loss_gap_ms == 0);
        assert(sk.ca.loss_delivered == sk.tcp.delivered);
        assert(sk.ca.loss_lost == sk.tcp.lost);
        assert(sk.ca.loss_stamp == tcp_jiffies32);
    }

    /* The existing two-second validity boundary has not moved. */
    init_app_loss_test(&sk);
    advance_ms(2000);
    sk.tcp.delivered = 8;
    u32 delivered = 0, lost = 0;
    assert(lotspeed_sample_loss(&sk, tcp_jiffies32, &delivered, &lost));
    assert(delivered == 8 && lost == 0);
}

static void test_severe_stall_pacing_and_recovery(void)
{
    struct sock sk;
    unsigned long saved_rate = lotserver_rate;
    unsigned int saved_floor = lotserver_min_rate_pct;
    lotserver_rate = 100000000; /* 800 Mbps, with a 64 Mbps adaptive floor. */
    lotserver_min_rate_pct = 8;
    for (int restart = 0; restart < 2; ++restart) {
        init_app_loss_test(&sk);
        lotserver_loss_adapt_samples = 1;
        sk.ca.actual_rate = 1000000;
        sk.ca.loss_ewma = LOTSPEED_LOSS_SCALE * 80 / 100;
        sk.ca.loss_adapt_count = 1;
        sk.tcp.mss_cache = 1380;
        sk.tcp.snd_nxt = sk.tcp.write_seq = 4 * 1024 * 1024;
        sk.tcp.packets_out = 3000;
        advance_ms(100);
        control_sample(&sk, 80, 320);
        assert(sk.sk_pacing_rate == 8000000);

        for (int gap = 0; gap < 3; ++gap) {
            advance_ms(2100);
            if (restart)
                lotspeed_cwnd_event(&sk, CA_EVENT_TX_START);
            else
                control_sample(&sk, 0, 0);
            advance_ms(40);
            control_sample(&sk, 8, 0);
            if (sk.sk_pacing_rate != 8000000)
                fprintf(stderr, "severe stalled pacing: expected 64000000, got %llu bps\n",
                        (unsigned long long)sk.sk_pacing_rate * 8);
            assert(sk.sk_pacing_rate == 8000000);
            assert(sk.ca.path_mode == PATH_CONGESTED);
            assert(sk.ca.state == AVOIDING);
            assert(sk.ca.app_loss_ms == 0); /* No stale authorization to learn down. */
        }

        /* Qualified healthy feedback can still recover without draining the queue. */
        for (int i = 0; i < 60; ++i) {
            advance_ms(100);
            control_sample(&sk, 100, 0);
        }
        assert(sk.ca.path_mode == PATH_STABLE);
        assert(sk.ca.state == CRUISING);
        assert(sk.sk_pacing_rate == 120000000);

        /* A truly drained AnyTLS connection retains the existing idle reset. */
        sk.ca.path_mode = PATH_CONGESTED;
        sk.ca.state = AVOIDING;
        sk.ca.loss_ewma = LOTSPEED_LOSS_SCALE;
        sk.tcp.snd_una = sk.tcp.write_seq;
        sk.tcp.packets_out = 0;
        lotspeed_update_mux_activity(&sk, tcp_jiffies32);
        advance_ms(LOTSPEED_MUX_IDLE_RESET_MS + 100);
        control_sample(&sk, 0, 0);
        assert(sk.ca.path_mode == PATH_STABLE);
        assert(sk.ca.actual_rate == 0);
        assert(sk.sk_pacing_rate == 120000000);
    }
    lotserver_rate = saved_rate;
    lotserver_min_rate_pct = saved_floor;
}

int main(void)
{
    assert(sizeof(struct lotspeed) <= 88); // oldest advertised private area
    test_retained_samples();
    test_single_burst_and_persistent_loss();
    test_fresh_loss_entry();
    test_idle_vs_backlog();
    test_expiry_wrap_and_confirmation_settings();
    test_app_limited_ordinary_and_moderate();
    test_app_limited_severe_learning_and_recovery();
    test_app_limited_rejects_weak_or_stale_evidence();
    test_app_limited_counter_wrap();
    test_app_limited_outstanding_retransmissions();
    test_app_loss_backlog_boundaries();
    test_app_loss_short_gaps_and_expiry();
    test_app_loss_small_or_healthy_outstanding();
    test_app_loss_high_rate_outstanding_pattern();
    test_severe_stall_scope_and_boundaries();
    test_severe_stall_pacing_and_recovery();
    printf("PASS: controller regression cases, HZ=%d, state=%zu bytes\n",
           HZ, sizeof(struct lotspeed));
    return 0;
}
