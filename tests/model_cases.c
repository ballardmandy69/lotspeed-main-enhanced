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

int main(void)
{
    assert(sizeof(struct lotspeed) <= 88); // oldest advertised private area
    test_retained_samples();
    test_single_burst_and_persistent_loss();
    test_idle_vs_backlog();
    test_expiry_wrap_and_confirmation_settings();
    printf("PASS: controller regression cases, HZ=%d, state=%zu bytes\n",
           HZ, sizeof(struct lotspeed));
    return 0;
}
