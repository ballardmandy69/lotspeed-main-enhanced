struct sock {
    struct tcp_sock tcp;
    struct lotspeed ca;
    u64 sk_pacing_rate, sk_max_pacing_rate;
};
#define tcp_sk(sk) (&(sk)->tcp)
#define inet_csk_ca(sk) (&(sk)->ca)

static void enter_state(struct sock *sk, enum lotspeed_state state)
{
    if (sk->ca.state != state) {
        sk->ca.state = state;
        sk->ca.last_state_ts = tcp_jiffies32;
    }
}

static void init_test(struct sock *sk)
{
    memset(sk, 0, sizeof(*sk));
    tcp_jiffies32 = 0;
    sk->ca.state = CRUISING;
    sk->ca.rtt_min = 50000;
    sk->ca.target_rate = lotserver_rate;
    sk->ca.mux_drained = true;
    sk->tcp.srtt_us = 50000 << 3;
    sk->tcp.mss_cache = 1440;
    sk->tcp.snd_cwnd_clamp = UINT32_MAX;
    sk->sk_max_pacing_rate = UINT64_MAX;
    lotserver_adaptive = true;
    lotserver_turbo = false;
    lotserver_loss_congest_pct = 30;
    lotserver_loss_recover_pct = 25;
    lotserver_loss_adapt_pct = 3;
    lotserver_loss_adapt_samples = 5;
}

static void advance_ms(u32 ms)
{
    tcp_jiffies32 += msecs_to_jiffies(ms);
}
