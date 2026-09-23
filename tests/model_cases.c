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
    assert(sk.ca.app_loss_ms == 0);
    assert(sk.ca.actual_rate == prior);
    // A new severe burst must qualify again, despite the historical EWMA.
    app_loss_round(&sk, 100, 50, 50, 50);
    assert(sk.ca.actual_rate == prior);
    app_loss_round(&sk, 100, 5000, 0, 0);
    assert(sk.ca.actual_rate > prior);
    assert(sk.ca.app_loss_ms == 0);

    sk.ca.app_loss_ms = LOTSPEED_APP_LOSS_HOLD_MS + 1;
    lotspeed_reset_mux_history(&sk, tcp_jiffies32);
    assert(sk.ca.app_loss_ms == 0 && sk.ca.actual_rate == 0);
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
        case 2: sk.tcp.snd_nxt = sk.tcp.write_seq; sk.tcp.packets_out = 100; break;
        case 3: sk.tcp.snd_nxt = sk.tcp.write_seq - 1439; break;
        case 4: retrans = 0; break;
        case 5: lost = 0; break;
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
        assert(sk.ca.app_loss_ms == 0);
        assert(sk.ca.actual_rate == lotserver_rate);
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
            assert(sk.ca.actual_rate == lotserver_rate && sk.ca.app_loss_ms == 0);
        } else {
            assert(sk.ca.actual_rate < lotserver_rate);
            assert(sk.ca.app_loss_ms == LOTSPEED_APP_LOSS_HOLD_MS + 1);
        }
    }
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
    printf("PASS: controller regression cases, HZ=%d, state=%zu bytes\n",
           HZ, sizeof(struct lotspeed));
    return 0;
}
