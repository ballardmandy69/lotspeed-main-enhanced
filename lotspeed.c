// lotspeed.c - v3.10.2 strict congestion-gated Mux adaptive edition
// Author: uk0
// Conservative integration of the proven main behavior with selected
// high-delay, loss-guard and shallow ProbeRTT ideas from later branches.

#include <linux/module.h>
#include <linux/version.h>
#include <net/tcp.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/rtc.h>

// 定义一个宏来简化使用
#define CURRENT_TIMESTAMP ({ \
    static char __ts[32]; \
    struct timespec64 ts; \
    struct tm tm; \
    ktime_get_real_ts64(&ts); \
    time64_to_tm(ts.tv_sec, 0, &tm); \
    snprintf(__ts, sizeof(__ts), "%04ld-%02d-%02d %02d:%02d:%02d", \
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, \
            tm.tm_hour, tm.tm_min, tm.tm_sec); \
    __ts; \
})

#define LOTSPEED_BETA_SCALE 1024
#define LOTSPEED_STARTUP_GROWTH_TARGET 1280
#define LOTSPEED_STARTUP_EXIT_ROUNDS 2
#define LOTSPEED_RATE_SAMPLE_MS 200
#define LOTSPEED_PROBE_RATE_MS 500
#define LOTSPEED_CRUISE_TIME_MS 2000
#define LOTSPEED_MAX_GAIN 100
#define LOTSPEED_MAX_RATE 4000000000UL
#define LOTSPEED_MAX_U32 ((u32)~0U)
#define LOTSPEED_MAX_U64 ((u64)~0ULL)
#define LOTSPEED_LOSS_SCALE 1024
#define LOTSPEED_ACK_EXTRA_MAX_US 100000
#define LOTSPEED_MUX_WINDOW_MS 5000
#define LOTSPEED_MUX_IDLE_RESET_MS 10000
#define LOTSPEED_MUX_ACTIVE_PCT 10
#define LOTSPEED_MUX_LOW_WINDOWS 2
#define LOTSPEED_CONGEST_MIN_DELIVERED 32
#define LOTSPEED_CONGEST_MAX_SAMPLE_US (2ULL * USEC_PER_SEC)

// Linux 6.10 restored ack/flag arguments to cong_control().
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#define LOTSPEED_NEW_CONG_CONTROL_API 1
#else
#define LOTSPEED_OLD_CONG_CONTROL_API 1
#endif

// --- 可调参数 ---
static unsigned long lotserver_rate = 32000000ULL;   // 256Mbps per-flow ceiling
static unsigned int lotserver_min_rate_pct = 60;     // percent of rate ceiling
static unsigned int lotserver_gain = 30;               // 3.0x default gain
static unsigned int lotserver_min_cwnd = 32;           // 最小拥塞窗口
static unsigned int lotserver_max_cwnd = 10000;        // 最大拥塞窗口
static unsigned int lotserver_beta = 871;              // about 85% cwnd retained
static bool lotserver_adaptive = true;
static bool lotserver_turbo = false;
static bool lotserver_verbose = false;
static unsigned int lotserver_pacing_gain = 120;       // pacing rate percent
static unsigned int lotserver_probe_rtt_interval_ms = 300000;
static unsigned int lotserver_probe_rtt_duration_ms = 100;
static unsigned int lotserver_probe_rtt_cwnd_pct = 75;
static unsigned int lotserver_min_rtt_window_sec = 30;
static unsigned int lotserver_rtt_tolerance_pct = 120;
static unsigned int lotserver_min_flight_ms = 250;
static unsigned int lotserver_avoid_hold_ms = 250;
static unsigned int lotserver_loss_congest_pct = 50;
static unsigned int lotserver_loss_recover_pct = 40;
static unsigned int lotserver_rtt_confirm_samples = 40;
static bool lotserver_loss_guard = true;
static unsigned int lotserver_noncong_beta = 1000;
static bool lotserver_hd_enable = false;
static unsigned int lotserver_hd_thresh_us = 120000;
static unsigned int lotserver_hd_gain_boost = 20;

// --- 参数回调 (保留v2.1的详细日志格式) ---
static int param_set_rate(const char *val, const struct kernel_param *kp)
{
    unsigned long old_val = lotserver_rate;
    int ret = param_set_ulong(val, kp);

    if (!ret && lotserver_rate < 125000)
        lotserver_rate = 125000;
    if (!ret && lotserver_rate > LOTSPEED_MAX_RATE)
        lotserver_rate = LOTSPEED_MAX_RATE;

    if (ret == 0 && old_val != lotserver_rate && lotserver_verbose) {
        unsigned long gbps_int = lotserver_rate / 125000000;
        unsigned long gbps_frac = (lotserver_rate % 125000000) * 100 / 125000000;
        pr_info("lotspeed: [uk0@%s] rate changed: %lu -> %lu (%lu.%02lu Gbps)\n",
                CURRENT_TIMESTAMP, old_val, lotserver_rate, gbps_int, gbps_frac);
    }
    return ret;
}

static int param_set_gain(const char *val, const struct kernel_param *kp)
{
    unsigned int old_val = lotserver_gain;
    int ret = param_set_uint(val, kp);

    if (!ret)
        lotserver_gain = clamp_t(unsigned int, lotserver_gain, 10, LOTSPEED_MAX_GAIN);

    if (ret == 0 && old_val != lotserver_gain && lotserver_verbose) {
        unsigned int gain_int = lotserver_gain / 10;
        unsigned int gain_frac = lotserver_gain % 10;
        pr_info("lotspeed: [uk0@%s] gain changed: %u -> %u (%u.%ux)\n",
                CURRENT_TIMESTAMP, old_val, lotserver_gain, gain_int, gain_frac);
    }
    return ret;
}

static int param_set_min_cwnd(const char *val, const struct kernel_param *kp)
{
    unsigned int old_val = lotserver_min_cwnd;
    int ret = param_set_uint(val, kp);

    if (!ret) {
        lotserver_min_cwnd = max_t(unsigned int, lotserver_min_cwnd, 2);
        if (lotserver_max_cwnd < lotserver_min_cwnd)
            lotserver_max_cwnd = lotserver_min_cwnd;
    }

    if (ret == 0 && old_val != lotserver_min_cwnd && lotserver_verbose) {
        pr_info("lotspeed: [uk0@%s] min_cwnd changed: %u -> %u\n",
                CURRENT_TIMESTAMP, old_val, lotserver_min_cwnd);
    }
    return ret;
}

static int param_set_max_cwnd(const char *val, const struct kernel_param *kp)
{
    unsigned int old_val = lotserver_max_cwnd;
    int ret = param_set_uint(val, kp);

    if (!ret)
        lotserver_max_cwnd = max(lotserver_max_cwnd, lotserver_min_cwnd);

    if (ret == 0 && old_val != lotserver_max_cwnd && lotserver_verbose) {
        pr_info("lotspeed: [uk0@%s] max_cwnd changed: %u -> %u\n",
                CURRENT_TIMESTAMP, old_val, lotserver_max_cwnd);
    }
    return ret;
}

static int param_set_adaptive(const char *val, const struct kernel_param *kp)
{
    bool old_val = lotserver_adaptive;
    int ret = param_set_bool(val, kp);

    if (ret == 0 && old_val != lotserver_adaptive && lotserver_verbose) {
        pr_info("lotspeed: [uk0@%s] adaptive mode: %s -> %s\n",
                CURRENT_TIMESTAMP, old_val ? "ON" : "OFF", lotserver_adaptive ? "ON" : "OFF");
    }
    return ret;
}

static int param_set_turbo(const char *val, const struct kernel_param *kp)
{
    bool old_val = lotserver_turbo;
    int ret = param_set_bool(val, kp);

    if (ret == 0 && old_val != lotserver_turbo && lotserver_verbose) {
        if (lotserver_turbo) {
            pr_info("lotspeed: [uk0@%s] ⚡⚡⚡ TURBO MODE ACTIVATED ⚡⚡⚡\n", CURRENT_TIMESTAMP);
            pr_info("lotspeed: WARNING: Ignoring ALL congestion signals!\n");
        } else {
            pr_info("lotspeed: [uk0@%s] Turbo mode DEACTIVATED\n", CURRENT_TIMESTAMP);
        }
    }
    return ret;
}

static int param_set_beta(const char *val, const struct kernel_param *kp)
{
    unsigned int *value = kp->arg;
    unsigned int old_val = *value;
    int ret = param_set_uint(val, kp);

    if (!ret)
        *value = clamp_t(unsigned int, *value, 128, LOTSPEED_BETA_SCALE);

    if (ret == 0 && kp->arg == &lotserver_beta &&
        old_val != lotserver_beta && lotserver_verbose) {
        pr_info("lotspeed: [uk0@%s] fairness beta changed: %u -> %u (%u/1024)\n",
                CURRENT_TIMESTAMP, old_val, lotserver_beta, lotserver_beta);
    }
    return ret;
}

static int param_set_floor_percent(const char *val,
                                   const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 1, 100);
    return ret;
}

static int param_set_percent(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 1, 200);
    return ret;
}

static int param_set_probe_percent(const char *val,
                                   const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 10, 100);
    return ret;
}

static int param_set_loss_congest(const char *val,
                                  const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret) {
        *value = clamp_t(unsigned int, *value, 1, 100);
        if (*value <= lotserver_loss_recover_pct)
            lotserver_loss_recover_pct = *value - 1;
    }
    return ret;
}

static int param_set_loss_recover(const char *val,
                                  const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret) {
        *value = min_t(unsigned int, *value, 99);
        if (*value >= lotserver_loss_congest_pct)
            *value = lotserver_loss_congest_pct - 1;
    }
    return ret;
}

static int param_set_confirm_rounds(const char *val,
                                    const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 1, 255);
    return ret;
}

static int param_set_msec(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 50, 600000);
    return ret;
}

static int param_set_optional_flight_msec(const char *val,
                                          const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = min_t(unsigned int, *value, 2000);
    return ret;
}

static int param_set_usec(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 1000, 2000000);
    return ret;
}

static int param_set_seconds(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 1, 3600);
    return ret;
}

static const struct kernel_param_ops param_ops_rate = { .set = param_set_rate, .get = param_get_ulong, };
static const struct kernel_param_ops param_ops_floor_percent = { .set = param_set_floor_percent, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_gain = { .set = param_set_gain, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_min_cwnd = { .set = param_set_min_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_max_cwnd = { .set = param_set_max_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_adaptive = { .set = param_set_adaptive, .get = param_get_bool, };
static const struct kernel_param_ops param_ops_turbo = { .set = param_set_turbo, .get = param_get_bool, };
static const struct kernel_param_ops param_ops_beta = { .set = param_set_beta, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_percent = { .set = param_set_percent, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_probe_percent = { .set = param_set_probe_percent, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_loss_congest = { .set = param_set_loss_congest, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_loss_recover = { .set = param_set_loss_recover, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_confirm_rounds = { .set = param_set_confirm_rounds, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_msec = { .set = param_set_msec, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_optional_flight_msec = { .set = param_set_optional_flight_msec, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_usec = { .set = param_set_usec, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_seconds = { .set = param_set_seconds, .get = param_get_uint, };

// --- 注册参数 ---
module_param_cb(lotserver_rate, &param_ops_rate, &lotserver_rate, 0644);
MODULE_PARM_DESC(lotserver_rate, "Target rate in bytes/sec (default 256Mbps)");

module_param_cb(lotserver_min_rate_pct, &param_ops_floor_percent,
                &lotserver_min_rate_pct, 0644);
MODULE_PARM_DESC(lotserver_min_rate_pct,
                 "Minimum adaptive rate as percent of rate ceiling");

module_param_cb(lotserver_gain, &param_ops_gain, &lotserver_gain, 0644);
MODULE_PARM_DESC(lotserver_gain, "Gain multiplier x10 (default 30 = 3.0x)");

module_param_cb(lotserver_min_cwnd, &param_ops_min_cwnd, &lotserver_min_cwnd, 0644);
MODULE_PARM_DESC(lotserver_min_cwnd, "Minimum congestion window");

module_param_cb(lotserver_max_cwnd, &param_ops_max_cwnd, &lotserver_max_cwnd, 0644);
MODULE_PARM_DESC(lotserver_max_cwnd, "Maximum congestion window");

module_param_cb(lotserver_adaptive, &param_ops_adaptive, &lotserver_adaptive, 0644);
MODULE_PARM_DESC(lotserver_adaptive,
                 "Enable congestion-gated adaptive rate control");

module_param_cb(lotserver_turbo, &param_ops_turbo, &lotserver_turbo, 0644);
MODULE_PARM_DESC(lotserver_turbo, "Turbo mode - ignore all congestion signals");

module_param_cb(lotserver_beta, &param_ops_beta, &lotserver_beta, 0644);
MODULE_PARM_DESC(lotserver_beta, "Beta for congestion backoff (default 871/1024)");

module_param(lotserver_verbose, bool, 0644);
MODULE_PARM_DESC(lotserver_verbose, "Enable verbose logging");

module_param_cb(lotserver_pacing_gain, &param_ops_percent, &lotserver_pacing_gain, 0644);
MODULE_PARM_DESC(lotserver_pacing_gain, "Pacing gain percent (default 120)");

module_param_cb(lotserver_probe_rtt_interval_ms, &param_ops_msec, &lotserver_probe_rtt_interval_ms, 0644);
MODULE_PARM_DESC(lotserver_probe_rtt_interval_ms, "ProbeRTT interval in milliseconds");

module_param_cb(lotserver_probe_rtt_duration_ms, &param_ops_msec, &lotserver_probe_rtt_duration_ms, 0644);
MODULE_PARM_DESC(lotserver_probe_rtt_duration_ms, "ProbeRTT duration in milliseconds");

module_param_cb(lotserver_probe_rtt_cwnd_pct, &param_ops_probe_percent, &lotserver_probe_rtt_cwnd_pct, 0644);
MODULE_PARM_DESC(lotserver_probe_rtt_cwnd_pct, "Percent of prior cwnd retained during ProbeRTT");

module_param_cb(lotserver_min_rtt_window_sec, &param_ops_seconds, &lotserver_min_rtt_window_sec, 0644);
MODULE_PARM_DESC(lotserver_min_rtt_window_sec, "Minimum RTT refresh window in seconds");

module_param_cb(lotserver_rtt_tolerance_pct, &param_ops_percent, &lotserver_rtt_tolerance_pct, 0644);
MODULE_PARM_DESC(lotserver_rtt_tolerance_pct, "RTT inflation tolerance percent");

module_param_cb(lotserver_min_flight_ms, &param_ops_optional_flight_msec, &lotserver_min_flight_ms, 0644);
MODULE_PARM_DESC(lotserver_min_flight_ms, "Minimum target-rate flight window in milliseconds (0 disables)");

module_param_cb(lotserver_avoid_hold_ms, &param_ops_msec, &lotserver_avoid_hold_ms, 0644);
MODULE_PARM_DESC(lotserver_avoid_hold_ms, "Minimum congestion-avoidance hold time in milliseconds");

module_param_cb(lotserver_loss_congest_pct, &param_ops_loss_congest, &lotserver_loss_congest_pct, 0644);
MODULE_PARM_DESC(lotserver_loss_congest_pct, "Loss EWMA percent required to classify congestion");

module_param_cb(lotserver_loss_recover_pct, &param_ops_loss_recover, &lotserver_loss_recover_pct, 0644);
MODULE_PARM_DESC(lotserver_loss_recover_pct, "Loss EWMA percent required to remain congested");

module_param_cb(lotserver_rtt_confirm_samples, &param_ops_confirm_rounds, &lotserver_rtt_confirm_samples, 0644);
MODULE_PARM_DESC(lotserver_rtt_confirm_samples, "Packet-timed RTT rounds required for congestion");

module_param(lotserver_loss_guard, bool, 0644);
MODULE_PARM_DESC(lotserver_loss_guard, "Use RTT to distinguish likely random loss");

module_param_cb(lotserver_noncong_beta, &param_ops_beta, &lotserver_noncong_beta, 0644);
MODULE_PARM_DESC(lotserver_noncong_beta, "Beta used for likely non-congestive loss");

module_param(lotserver_hd_enable, bool, 0644);
MODULE_PARM_DESC(lotserver_hd_enable, "Enable high-delay cwnd gain compensation");

module_param_cb(lotserver_hd_thresh_us, &param_ops_usec, &lotserver_hd_thresh_us, 0644);
MODULE_PARM_DESC(lotserver_hd_thresh_us, "High-delay path threshold in microseconds");

module_param_cb(lotserver_hd_gain_boost, &param_ops_percent, &lotserver_hd_gain_boost, 0644);
MODULE_PARM_DESC(lotserver_hd_gain_boost, "High-delay cwnd gain boost percent");

// --- 统计信息 (整合v2.1的详细统计) ---
static atomic_t active_connections = ATOMIC_INIT(0);
static atomic64_t total_bytes_sent = ATOMIC64_INIT(0);
static atomic_t total_losses = ATOMIC_INIT(0);

// --- v3.0 核心状态机 ---
enum lotspeed_state {
    STARTUP,  // 智能慢启动
    PROBING,  // 探测更高带宽
    CRUISING, // 稳定在瓶颈带宽
    AVOIDING, // 拥塞规避
    PROBE_RTT // RTT 探测
};

enum lotspeed_path_mode {
    PATH_STABLE,
    PATH_JITTERY,
    PATH_CONGESTED
};

// --- v3.5 per-connection state ---
struct lotspeed {
    // Keep u64 fields together so the structure stays below older kernels'
    // congestion-control private-state limit.
    u64 target_rate;
    u64 actual_rate;
    u64 mux_tx_base;

    u32 last_state_ts;
    u32 probe_rtt_ts;
    u32 rtt_min;
    u32 rtt_candidate;
    u32 rtt_dev;
    u32 rtt_prev;
    u32 loss_ewma;
    u32 min_rtt_stamp;
    u32 probe_prior_cwnd;
    u32 mux_activity_stamp;
    u32 next_rtt_delivered;
    u32 round_stamp;
    u16 extra_acked;
    u8 state;
    u8 mux_low_windows;
    u8 rtt_high_count;
    u8 path_mode;
    bool ss_mode;
};

// 将状态转换为字符串，用于日志
static const char* state_to_str(enum lotspeed_state state) {
    switch (state) {
        case STARTUP: return "STARTUP";
        case PROBING: return "PROBING";
        case CRUISING: return "CRUISING";
        case AVOIDING: return "AVOIDING";
        case PROBE_RTT: return "PROBE_RTT";
        default: return "UNKNOWN";
    }
}

static const char *path_to_str(u8 mode)
{
    switch (mode) {
        case PATH_STABLE: return "STABLE";
        case PATH_JITTERY: return "JITTERY";
        case PATH_CONGESTED: return "CONGESTED";
        default: return "UNKNOWN";
    }
}

// 切换状态并记录日志
static void enter_state(struct sock *sk, enum lotspeed_state new_state) {
    struct lotspeed *ca = inet_csk_ca(sk);
    if (ca->state != new_state) {
        if (lotserver_verbose) {
            pr_info("lotspeed: [uk0@%s] state %s -> %s\n",
                    CURRENT_TIMESTAMP, state_to_str(ca->state), state_to_str(new_state));
        }
        ca->state = new_state;
        ca->last_state_ts = tcp_jiffies32;

    }
}


// 初始化连接
static void lotspeed_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    memset(ca, 0, sizeof(struct lotspeed));

    // 初始状态为智能启动
    ca->state = STARTUP;
    ca->last_state_ts = tcp_jiffies32;
    ca->probe_rtt_ts = tcp_jiffies32;
    ca->min_rtt_stamp = tcp_jiffies32;
    ca->mux_activity_stamp = tcp_jiffies32;
    ca->round_stamp = tcp_jiffies32;
    ca->next_rtt_delivered = tp->delivered;
    ca->path_mode = PATH_STABLE;
    ca->target_rate = lotserver_rate;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
    ca->mux_tx_base = tp->bytes_sent;
#else
    ca->mux_tx_base = tp->data_segs_out;
#endif

    // v2.1特性
    ca->ss_mode = true;
    ca->probe_prior_cwnd = tp->snd_cwnd;

    // 设置慢启动阈值
    tp->snd_ssthresh = lotserver_turbo ? TCP_INFINITE_SSTHRESH :
                        (u32)min_t(u64, (u64)tp->snd_cwnd * 2,
                                   LOTSPEED_MAX_U32);

    // 强制开启 pacing
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
#endif

    atomic_inc(&active_connections);

    if (lotserver_verbose) {
        unsigned long gbps_int = lotserver_rate / 125000000;
        unsigned long gbps_frac = (lotserver_rate % 125000000) * 100 / 125000000;
        unsigned int gain_int = lotserver_gain / 10;
        unsigned int gain_frac = lotserver_gain % 10;

        pr_info("lotspeed: [uk0@%s] NEW connection #%d | rate=%lu.%02lu Gbps | gain=%u.%ux | mode=%s | state=%s\n",
                CURRENT_TIMESTAMP,
                atomic_read(&active_connections),
                gbps_int, gbps_frac,
                gain_int, gain_frac,
                lotserver_turbo ? "TURBO" :
                lotserver_adaptive ? "congestion-adaptive" : "fixed",
                state_to_str(ca->state));
    }
}

// 释放连接
static void lotspeed_release(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    if (!ca) {
        pr_warn("lotspeed: [uk0@%s] release called with NULL ca\n", CURRENT_TIMESTAMP);
        atomic_dec(&active_connections);
        return;
    }

    atomic_dec(&active_connections);

    if (tp->bytes_acked > 0)
        atomic64_add(tp->bytes_acked, &total_bytes_sent);
    if (lotserver_verbose) {
        u64 mb_sent = tp->bytes_acked >> 20;
        pr_info("lotspeed: [uk0@%s] connection released | sent=%llu MB | losses=%u | active=%d\n",
                CURRENT_TIMESTAMP, mb_sent, tp->total_retrans,
                atomic_read(&active_connections));
    }

    memset(ca, 0, sizeof(struct lotspeed));
}

// 更新 RTT 统计
static void lotspeed_update_rtt(struct sock *sk, u32 rtt_us)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    u32 now = tcp_jiffies32;
    u32 jitter_sample = 0;
    bool expired;

    if (!rtt_us)
        return;

    if (ca->rtt_prev)
        jitter_sample = rtt_us > ca->rtt_prev ?
                        rtt_us - ca->rtt_prev :
                        ca->rtt_prev - rtt_us;
    ca->rtt_prev = rtt_us;
    ca->rtt_dev = (ca->rtt_dev * 7 + jitter_sample) / 8;

    expired = time_after32(now, ca->min_rtt_stamp +
                          lotserver_min_rtt_window_sec * HZ);

    if (!ca->rtt_min) {
        ca->rtt_min = rtt_us;
        ca->rtt_candidate = rtt_us;
        ca->min_rtt_stamp = now;
    } else {
        if (!ca->rtt_candidate || rtt_us < ca->rtt_candidate)
            ca->rtt_candidate = rtt_us;

        if (lotserver_verbose && ca->rtt_min > 0 && rtt_us < ca->rtt_min)
            pr_info("lotspeed: [uk0@%s] new min_rtt: %u us (was %u)\n",
                    CURRENT_TIMESTAMP, rtt_us, ca->rtt_min);

        if (rtt_us < ca->rtt_min)
            ca->rtt_min = rtt_us;

        // Move the baseline upward only to the minimum observed over a
        // complete window, never to an arbitrary queued sample.
        if (expired) {
            if (ca->rtt_candidate)
                ca->rtt_min = ca->rtt_candidate;
            ca->rtt_candidate = rtt_us;
            ca->min_rtt_stamp = now;
        }
    }
}

static bool lotspeed_rtt_inflated(const struct lotspeed *ca, u32 rtt_us)
{
    u64 threshold;

    if (!ca->rtt_min || !rtt_us)
        return false;

    threshold = ca->rtt_min;
    threshold += div_u64((u64)ca->rtt_min * lotserver_rtt_tolerance_pct, 100);
    threshold += (u64)ca->rtt_dev * 4;
    return rtt_us > threshold;
}

static void lotspeed_update_path_mode(struct sock *sk,
                                      u32 rtt_us,
                                      u32 delivered,
                                      u32 losses,
                                      bool qualified)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 total_packets;
    u32 sample_loss = 0;
    u32 loss_congest;
    u32 loss_recover;
    u32 jitter_threshold;
    u8 old_mode = ca->path_mode;
    u8 decay = 2;
    bool raw_inflated = lotspeed_rtt_inflated(ca, rtt_us);

    if (!qualified)
        return;

    if (lotserver_adaptive && ca->path_mode == PATH_CONGESTED)
        decay = 8;

    if (raw_inflated) {
        if (ca->rtt_high_count < 255)
            ca->rtt_high_count++;
    } else if (ca->rtt_high_count > 0) {
        ca->rtt_high_count = ca->rtt_high_count > decay ?
                             ca->rtt_high_count - decay : 0;
    }

    if (delivered || losses) {
        total_packets = (u64)delivered + losses;
        if (total_packets)
            sample_loss = (u32)min_t(u64,
                div64_u64((u64)losses * LOTSPEED_LOSS_SCALE,
                          total_packets),
                LOTSPEED_LOSS_SCALE);
        ca->loss_ewma = (ca->loss_ewma * 7 + sample_loss) / 8;
    }

    loss_congest = lotserver_loss_congest_pct * LOTSPEED_LOSS_SCALE / 100;
    loss_recover = lotserver_loss_recover_pct * LOTSPEED_LOSS_SCALE / 100;
    jitter_threshold = max_t(u32, ca->rtt_min / 4, 8000);

    if (ca->loss_ewma >= loss_congest ||
        (ca->rtt_high_count >= lotserver_rtt_confirm_samples &&
         ca->loss_ewma >= loss_recover)) {
        ca->path_mode = PATH_CONGESTED;
    } else if (ca->path_mode == PATH_CONGESTED &&
               (ca->loss_ewma > loss_recover ||
                (ca->rtt_high_count > lotserver_rtt_confirm_samples / 3 &&
                 ca->loss_ewma > loss_recover / 2))) {
        ca->path_mode = PATH_CONGESTED;
    } else if (ca->rtt_dev >= jitter_threshold ||
               (ca->path_mode == PATH_JITTERY &&
                ca->rtt_dev >= jitter_threshold * 3 / 4)) {
        ca->path_mode = PATH_JITTERY;
    } else {
        ca->path_mode = PATH_STABLE;
    }

    if (lotserver_verbose && old_mode != ca->path_mode)
        pr_info("lotspeed: [uk0@%s] path %s -> %s | min=%u us jitter=%u us loss=%u/1024\n",
                CURRENT_TIMESTAMP, path_to_str(old_mode),
                path_to_str(ca->path_mode), ca->rtt_min, ca->rtt_dev,
                ca->loss_ewma);
}

static u64 lotspeed_scale_percent(u64 value, u32 percent)
{
    if (!percent)
        return 0;
    if (value > div64_u64(LOTSPEED_MAX_U64, percent))
        return LOTSPEED_MAX_U64;
    return div64_u64(value * percent, 100);
}

static u64 lotspeed_adaptive_floor(void)
{
    return max_t(u64,
                 lotspeed_scale_percent(lotserver_rate,
                                         lotserver_min_rate_pct),
                 125000);
}

static u64 lotspeed_tcp_tx_counter(const struct tcp_sock *tp)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
    return tp->bytes_sent;
#else
    return tp->data_segs_out;
#endif
}

static u64 lotspeed_tcp_counter_delta(u64 current_counter, u64 previous,
                                      u32 mss)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
    (void)mss;
    return current_counter - previous;
#else
    return (u64)(u32)((u32)current_counter - (u32)previous) * mss;
#endif
}

static void lotspeed_reset_mux_history(struct sock *sk, u32 now)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    ca->target_rate = lotserver_rate;
    ca->actual_rate = 0;
    ca->loss_ewma = 0;
    ca->rtt_high_count = 0;
    ca->path_mode = PATH_STABLE;
    ca->extra_acked = 0;
    ca->mux_low_windows = 0;
    ca->mux_tx_base = lotspeed_tcp_tx_counter(tp);
    ca->mux_activity_stamp = now;

    if (ca->state == AVOIDING)
        enter_state(sk, CRUISING);
}

static void lotspeed_update_mux_activity(struct sock *sk, u32 mss, u32 now)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u32 elapsed_ms;
    u64 tx_now;
    u64 tx_bytes;
    u64 low_rate;
    u64 low_bytes;

    if (!lotserver_adaptive) {
        ca->target_rate = lotserver_rate;
        ca->mux_low_windows = 0;
        ca->mux_tx_base = lotspeed_tcp_tx_counter(tp);
        ca->mux_activity_stamp = now;
        return;
    }

    elapsed_ms = jiffies_to_msecs(now - ca->mux_activity_stamp);
    if (elapsed_ms < LOTSPEED_MUX_WINDOW_MS)
        return;

    tx_now = lotspeed_tcp_tx_counter(tp);
    tx_bytes = lotspeed_tcp_counter_delta(tx_now, ca->mux_tx_base,
                                           mss);
    low_rate = lotspeed_scale_percent(lotserver_rate,
                                      LOTSPEED_MUX_ACTIVE_PCT);
    low_bytes = div64_u64(low_rate * elapsed_ms, 1000);

    if (tx_bytes < low_bytes) {
        if (ca->mux_low_windows < LOTSPEED_MUX_LOW_WINDOWS)
            ca->mux_low_windows++;
    } else {
        ca->mux_low_windows = 0;
    }

    ca->mux_tx_base = tx_now;
    ca->mux_activity_stamp = now;

    if (ca->mux_low_windows >= LOTSPEED_MUX_LOW_WINDOWS) {
        if (lotserver_verbose)
            pr_info("lotspeed: [uk0@%s] low Mux traffic reset adaptive history\n",
                    CURRENT_TIMESTAMP);
        lotspeed_reset_mux_history(sk, now);
    }
}

/* Update delivery, loss, and ACK aggregation once per packet-timed RTT. */
static bool lotspeed_update_round_model(struct sock *sk,
                                        const struct rate_sample *rs,
                                        u32 mss,
                                        u32 path_rtt)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u32 now = tcp_jiffies32;
    u32 delivered;
    u32 losses;
    u32 path_delivered;
    bool qualified_round;
    u32 elapsed_jiffies;
    u64 elapsed_us;
    u64 delivered_bytes;
    u64 round_rate = 0;
    u64 prior_rate = ca->actual_rate;

    if (!rs || before(rs->prior_delivered, ca->next_rtt_delivered))
        return false;

    delivered = tp->delivered - ca->next_rtt_delivered;
    losses = rs->losses > 0 ? rs->losses : 0;
    elapsed_jiffies = now - ca->round_stamp;
    elapsed_us = jiffies_to_usecs(elapsed_jiffies);

    ca->next_rtt_delivered = tp->delivered;
    ca->round_stamp = now;

    if (delivered && elapsed_us) {
        delivered_bytes = (u64)delivered * mss;
        round_rate = div64_u64(delivered_bytes * USEC_PER_SEC,
                               elapsed_us);
        round_rate = min_t(u64, round_rate,
                           lotspeed_scale_percent(lotserver_rate, 200));

        if (!prior_rate) {
            ca->actual_rate = round_rate;
        } else if (!rs->is_app_limited || round_rate >= prior_rate) {
            /* Rise quickly, but let temporary weak rounds decay slowly. */
            if (round_rate >= prior_rate)
                ca->actual_rate = (prior_rate * 3 + round_rate) / 4;
            else
                ca->actual_rate = (prior_rate * 7 + round_rate) / 8;
        }
    }

    if (!rs->is_app_limited && prior_rate && elapsed_us &&
        elapsed_us <= 10ULL * USEC_PER_SEC && mss) {
        u64 expected = div64_u64(prior_rate * elapsed_us,
                                 (u64)mss * USEC_PER_SEC);
        u32 extra = delivered > expected ?
                    (u32)min_t(u64, delivered - expected, 0xffff) : 0;

        ca->extra_acked = max_t(u16, ca->extra_acked * 7 / 8,
                                (u16)extra);
    } else {
        ca->extra_acked = ca->extra_acked * 7 / 8;
    }

    path_delivered = rs->delivered > 0 ? (u32)rs->delivered : 0;
    qualified_round = !rs->is_app_limited &&
                      path_delivered >= LOTSPEED_CONGEST_MIN_DELIVERED &&
                      rs->interval_us > 0 &&
                      rs->interval_us <= LOTSPEED_CONGEST_MAX_SAMPLE_US;
    lotspeed_update_path_mode(sk, path_rtt, path_delivered, losses,
                              qualified_round);
    return true;
}

// --- v3.10.2 core: qualified congestion adaptation with Mux reset ---
static void lotspeed_adapt_and_control(struct sock *sk, const struct rate_sample *rs, int flag)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 pacing_rate;
    u64 target_rate;
    u32 rtt_us = tp->srtt_us >> 3;
    u32 path_rtt;
    u32 bdp_rtt;
    u32 cwnd;
    u32 target_cwnd = 0;
    u32 mss = tp->mss_cache ? : 1460;
    u32 now = tcp_jiffies32;
    u32 effective_gain;
    u32 cwnd_gain;
    u32 pacing_gain;
    u32 pipe;
    u64 adaptive_floor;
    bool congestion_detected = false;
    bool high_delay_path;

    (void)flag;

    if (rs && rs->rtt_us > 0) {
        rtt_us = (u32)min_t(unsigned long, rs->rtt_us,
                            LOTSPEED_MAX_U32);
        lotspeed_update_rtt(sk, rtt_us);
    } else if (!ca->rtt_min && rtt_us) {
        lotspeed_update_rtt(sk, rtt_us);
    }

    if (!rtt_us)
        rtt_us = ca->rtt_min ? : 1000;
    path_rtt = rtt_us;

    lotspeed_update_round_model(sk, rs, mss, path_rtt);
    lotspeed_update_mux_activity(sk, mss, now);
    adaptive_floor = lotspeed_adaptive_floor();
    high_delay_path = lotserver_hd_enable &&
                      ca->path_mode == PATH_STABLE &&
                      ca->rtt_min >= lotserver_hd_thresh_us;

    /* Only the qualified EWMA/RTT classifier may lower the target rate. */
    if (!lotserver_turbo && lotserver_adaptive &&
        ca->path_mode == PATH_CONGESTED)
        congestion_detected = true;

    if (ca->state != PROBE_RTT && ca->rtt_min > 0 &&
        time_after32(now, ca->probe_rtt_ts +
                     msecs_to_jiffies(lotserver_probe_rtt_interval_ms))) {
        ca->probe_prior_cwnd = tp->snd_cwnd;
        enter_state(sk, PROBE_RTT);
    }

    switch (ca->state) {
        case STARTUP:
            if (congestion_detected) {
                enter_state(sk, AVOIDING);
            } else if (time_after32(now, ca->last_state_ts +
                                    msecs_to_jiffies(LOTSPEED_PROBE_RATE_MS))) {
                ca->ss_mode = false;
                enter_state(sk, CRUISING);
            }
            break;

        case PROBING:
            if (congestion_detected) {
                enter_state(sk, AVOIDING);
            } else {
                enter_state(sk, CRUISING);
            }
            break;

        case CRUISING:
            if (congestion_detected)
                enter_state(sk, AVOIDING);
            break;

        case AVOIDING:
            if (!congestion_detected &&
                time_after32(now, ca->last_state_ts +
                             msecs_to_jiffies(lotserver_avoid_hold_ms))) {
                enter_state(sk, CRUISING);
            }
            break;

        case PROBE_RTT:
            if (time_after32(now, ca->last_state_ts +
                             msecs_to_jiffies(lotserver_probe_rtt_duration_ms))) {
                ca->probe_rtt_ts = now;
                enter_state(sk, ca->actual_rate ? CRUISING : STARTUP);
            }
            break;
    }

    if (lotserver_adaptive && ca->state == AVOIDING &&
        ca->actual_rate > 0) {
        ca->target_rate = clamp_t(u64,
            lotspeed_scale_percent(ca->actual_rate, 105),
            adaptive_floor, (u64)lotserver_rate);
    } else {
        ca->target_rate = lotserver_rate;
    }
    target_rate = ca->target_rate;

    effective_gain = lotserver_gain;
    if (high_delay_path)
        effective_gain = min_t(u32, LOTSPEED_MAX_GAIN,
                               effective_gain +
                               effective_gain * lotserver_hd_gain_boost / 100);
    if (ca->path_mode == PATH_CONGESTED)
        effective_gain = max_t(u32, effective_gain * 98 / 100, 10);

    switch (ca->state) {
        case STARTUP:
            cwnd_gain = min_t(u32, LOTSPEED_MAX_GAIN,
                              effective_gain * 12 / 10);
            break;
        case PROBING:
            cwnd_gain = effective_gain;
            break;
        case CRUISING:
            cwnd_gain = effective_gain;
            break;
        case AVOIDING:
            cwnd_gain = max_t(u32, effective_gain * 98 / 100, 10);
            break;
        case PROBE_RTT:
            cwnd_gain = effective_gain;
            break;
        default:
            cwnd_gain = effective_gain;
            break;
    }

    target_rate = clamp_t(u64, target_rate, 125000,
                          (u64)lotserver_rate);

    bdp_rtt = ca->rtt_min ? : rtt_us;
    if (ca->rtt_min && ca->rtt_dev) {
        u64 allowance = min_t(u64, (u64)ca->rtt_dev * 2,
                               ca->rtt_min / 4);

        bdp_rtt = (u32)min_t(u64, (u64)ca->rtt_min + allowance,
                              LOTSPEED_MAX_U32);
    }

    if (mss > 0 && bdp_rtt > 0 &&
        target_rate <= div64_u64(LOTSPEED_MAX_U64, bdp_rtt)) {
        u64 bdp = target_rate * bdp_rtt;
        u64 ack_rate;
        u32 ack_cap;
        u32 ack_extra;

        target_cwnd = (u32)min_t(u64,
            div64_u64(bdp, (u64)mss * USEC_PER_SEC), LOTSPEED_MAX_U32);
        target_cwnd = (u32)min_t(u64,
            div64_u64((u64)target_cwnd * cwnd_gain, 10),
            LOTSPEED_MAX_U32);

        if (lotserver_min_flight_ms &&
            target_rate <= div64_u64(LOTSPEED_MAX_U64,
                                     lotserver_min_flight_ms)) {
            u32 flight_floor = (u32)min_t(u64,
                div64_u64(target_rate * lotserver_min_flight_ms,
                          (u64)mss * 1000),
                LOTSPEED_MAX_U32);

            target_cwnd = max(target_cwnd, flight_floor);
        }

        ack_rate = ca->actual_rate ?
                   min_t(u64, ca->actual_rate, target_rate) :
                   target_rate;
        ack_cap = (u32)min_t(u64,
            div64_u64(ack_rate * LOTSPEED_ACK_EXTRA_MAX_US,
                      (u64)mss * USEC_PER_SEC),
            LOTSPEED_MAX_U32);
        ack_extra = min_t(u32, ca->extra_acked, ack_cap);
        target_cwnd = (u32)min_t(u64,
            (u64)target_cwnd + ack_extra, LOTSPEED_MAX_U32);
    }

    if (ca->state == PROBE_RTT) {
        u32 retained = (u32)div_u64((u64)ca->probe_prior_cwnd *
                                    min_t(u32, lotserver_probe_rtt_cwnd_pct, 100),
                                    100);
        u64 probe_rate = target_rate;
        u32 probe_floor = 0;

        if (mss && ca->rtt_min &&
            probe_rate <= div64_u64(LOTSPEED_MAX_U64, ca->rtt_min))
            probe_floor = (u32)min_t(u64,
                div64_u64(probe_rate * ca->rtt_min,
                          (u64)mss * USEC_PER_SEC),
                LOTSPEED_MAX_U32);

        retained = max(retained, probe_floor);
        cwnd = max(lotserver_min_cwnd, retained);
    } else if (ca->ss_mode && tp->snd_cwnd < tp->snd_ssthresh) {
        u32 acked = rs && rs->acked_sacked > 0 ? rs->acked_sacked : 1;

        cwnd = tp->snd_cwnd > LOTSPEED_MAX_U32 - acked ?
               LOTSPEED_MAX_U32 : tp->snd_cwnd + acked;
        if (target_cwnd > 0 && cwnd >= target_cwnd) {
            ca->ss_mode = false;
            cwnd = target_cwnd;
        }
    } else {
        cwnd = target_cwnd ? : lotserver_min_cwnd;
    }

    cwnd = clamp(cwnd, lotserver_min_cwnd, lotserver_max_cwnd);
    if (ca->state != PROBE_RTT) {
        pipe = tcp_packets_in_flight(tp);
        if (pipe > 0 && pipe < LOTSPEED_MAX_U32 && cwnd <= pipe)
            cwnd = pipe + 1;
    }

    tp->snd_cwnd = min_t(u32, cwnd, lotserver_max_cwnd);
    tp->snd_cwnd = min_t(u32, tp->snd_cwnd, tp->snd_cwnd_clamp);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    pacing_gain = lotserver_pacing_gain;
    if (ca->path_mode == PATH_JITTERY)
        pacing_gain = min_t(u32, pacing_gain, 110);
    else if (ca->path_mode == PATH_CONGESTED)
        pacing_gain = min_t(u32, pacing_gain, 100);
    pacing_rate = lotspeed_scale_percent(target_rate, pacing_gain);
    WRITE_ONCE(sk->sk_pacing_rate,
               min_t(u64, pacing_rate, sk->sk_max_pacing_rate));
#endif

    if (lotserver_verbose && rs && rs->losses > 0 &&
        tp->total_retrans > 0 &&
        tp->total_retrans % 100 == 0) {
        unsigned long gbps_int = target_rate / 125000000;
        unsigned long gbps_frac = (target_rate % 125000000) * 100 / 125000000;
        unsigned int gain_int = cwnd_gain / 10;
        unsigned int gain_frac = cwnd_gain % 10;

        pr_info("lotspeed: [uk0@%s] STATUS: [%s/%s] cwnd=%u | rate=%lu.%02lu Gbps | RTT=%u us | jitter=%u us | gain=%u.%ux | losses=%u\n",
                CURRENT_TIMESTAMP, state_to_str(ca->state),
                path_to_str(ca->path_mode),
                tp->snd_cwnd,
                gbps_int, gbps_frac, rtt_us, ca->rtt_dev,
                gain_int, gain_frac, tp->total_retrans);
    }
}

// 主拥塞控制函数 - 兼容不同内核版本
#ifdef LOTSPEED_NEW_CONG_CONTROL_API
static void lotspeed_cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
{
    lotspeed_adapt_and_control(sk, rs, flag);
}
#else // LOTSPEED_OLD_CONG_CONTROL_API
static void lotspeed_cong_control(struct sock *sk, const struct rate_sample *rs)
{
    lotspeed_adapt_and_control(sk, rs, 0);
}
#endif

// 处理丢包时的 ssthresh (引入公平性退避)
static u32 lotspeed_ssthresh(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u32 beta = lotserver_beta;
    u64 reduced;

    if (lotserver_turbo) {
        return TCP_INFINITE_SSTHRESH;
    }

    atomic_inc(&total_losses);

    if (lotserver_loss_guard) {
        if (ca->path_mode == PATH_STABLE) {
            beta = max(beta, lotserver_noncong_beta);
        } else if (ca->path_mode == PATH_JITTERY) {
            beta = max(beta, (lotserver_beta +
                             lotserver_noncong_beta) / 2);
        }
    }

    reduced = div_u64((u64)tp->snd_cwnd * beta, LOTSPEED_BETA_SCALE);
    return max_t(u32, (u32)min_t(u64, reduced, LOTSPEED_MAX_U32),
                 lotserver_min_cwnd);
}

// 处理状态变化 (TCP_CA_Loss)
static void lotspeed_set_state_hook(struct sock *sk, u8 new_state)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    switch (new_state) {
        case TCP_CA_Loss:
            if (lotserver_turbo) {
                if (lotserver_verbose && tp->total_retrans % 10 == 0) {
                    pr_info("lotspeed: [uk0@%s] TURBO: Ignoring loss #%u\n",
                            CURRENT_TIMESTAMP, tp->total_retrans + 1);
                }
                return;
            }
            /* A single RTO still reduces cwnd, but cannot lower the rate. */
            if (ca->path_mode == PATH_CONGESTED)
                enter_state(sk, AVOIDING);

            if (lotserver_verbose &&
                (tp->total_retrans == 1 || tp->total_retrans % 10 == 0))
                pr_info("lotspeed: [uk0@%s] LOSS detected after %u retransmissions\n",
                        CURRENT_TIMESTAMP, tp->total_retrans);
            break;

        case TCP_CA_Recovery:
            break;

        case TCP_CA_Open:
            ca->ss_mode = false;
            break;

        default:
            break;
    }
}

static u32 lotspeed_undo_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    // 误判恢复
    ca->ss_mode = false;

    return min_t(u32, max(tp->snd_cwnd, tp->prior_cwnd),
                 tp->snd_cwnd_clamp);
}

static void lotspeed_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u32 now = tcp_jiffies32;

    switch (event) {
        case CA_EVENT_TX_START:
            if (time_after32(now, ca->round_stamp +
                             msecs_to_jiffies(
                                  LOTSPEED_MUX_IDLE_RESET_MS))) {
                lotspeed_reset_mux_history(sk, now);
            }
            ca->ss_mode = true;
            ca->round_stamp = now;
            ca->next_rtt_delivered = tp->delivered;
            ca->extra_acked = 0;
            break;

        case CA_EVENT_CWND_RESTART:
            ca->ss_mode = true;
            if (time_after32(now, ca->round_stamp +
                             msecs_to_jiffies(
                                  LOTSPEED_MUX_IDLE_RESET_MS))) {
                lotspeed_reset_mux_history(sk, now);
            }
            ca->round_stamp = now;
            ca->next_rtt_delivered = tp->delivered;
            ca->extra_acked = 0;
            break;

        default:
            break;
    }
}

static struct tcp_congestion_ops lotspeed_ops __read_mostly = {
        .name           = "lotspeed",
        .owner          = THIS_MODULE,
        .init           = lotspeed_init,
        .release        = lotspeed_release,
        .cong_control   = lotspeed_cong_control,
        .ssthresh       = lotspeed_ssthresh,
        .set_state      = lotspeed_set_state_hook,
        .undo_cwnd      = lotspeed_undo_cwnd,
        .cwnd_event     = lotspeed_cwnd_event,
        .flags          = TCP_CONG_NON_RESTRICTED,
};

// 辅助函数来格式化带边框的行
static void print_boxed_line(const char *prefix, const char *content)
{
    int prefix_len = strlen(prefix);
    int content_len = strlen(content);
    int total_len = prefix_len + content_len;
    int padding = 56 - total_len;

    if (padding < 0) padding = 0;

    pr_info("║%s%s%*s║\n", prefix, content, padding, "");
}

// --- 模块初始化与退出 ---
static int __init lotspeed_module_init(void)
{
    unsigned long gbps_int, gbps_frac;
    unsigned int gain_int, gain_frac;
    char buffer[128];

    BUILD_BUG_ON(sizeof(struct lotspeed) > ICSK_CA_PRIV_SIZE);

    pr_info("╔════════════════════════════════════════════════════════╗\n");
    pr_info("║    LotSpeed v3.10.2 - strict adaptive Mux             ║\n");

    snprintf(buffer, sizeof(buffer), "uk0 @ 2025-11-20 18:58:51");
    print_boxed_line("          Created by ", buffer);

    snprintf(buffer, sizeof(buffer), "%u.%u.%u",
             LINUX_VERSION_CODE >> 16,
             (LINUX_VERSION_CODE >> 8) & 0xff,
             LINUX_VERSION_CODE & 0xff);
    print_boxed_line("          Kernel: ", buffer);

#ifdef LOTSPEED_NEW_CONG_CONTROL_API
    pr_info("║          API: NEW (6.10+)                              ║\n");
#else
    pr_info("║          API: LEGACY (6.9 and older)                   ║\n");
#endif

    pr_info("╚════════════════════════════════════════════════════════╝\n");

    gbps_int = lotserver_rate / 125000000;
    gbps_frac = (lotserver_rate % 125000000) * 100 / 125000000;
    gain_int = lotserver_gain / 10;
    gain_frac = lotserver_gain % 10;

    pr_info("Initial Parameters:\n");
    pr_info("  Max Rate: %lu.%02lu Gbps\n", gbps_int, gbps_frac);
    pr_info("  Max Gain: %u.%ux\n", gain_int, gain_frac);
    pr_info("  Min/Max CWND: %u/%u\n", lotserver_min_cwnd, lotserver_max_cwnd);
    pr_info("  Fairness Beta: %u/1024\n", lotserver_beta);
    pr_info("  Congestion Adaptive: %s | Turbo: %s | Verbose: %s\n",
             lotserver_adaptive ? "ON" : "OFF",
             lotserver_turbo ? "ON" : "OFF",
             lotserver_verbose ? "ON" : "OFF");
    pr_info("  Pacing Gain: %u%% | ProbeRTT: %ums/%ums/%u%% cwnd\n",
            lotserver_pacing_gain, lotserver_probe_rtt_interval_ms,
            lotserver_probe_rtt_duration_ms, lotserver_probe_rtt_cwnd_pct);
    pr_info("  Adaptive Floor: %u%% of rate ceiling\n",
            lotserver_min_rate_pct);
    pr_info("  Mux Reset: %u low windows of %ums below %u%% load\n",
            LOTSPEED_MUX_LOW_WINDOWS, LOTSPEED_MUX_WINDOW_MS,
            LOTSPEED_MUX_ACTIVE_PCT);
    pr_info("  Minimum Flight Window: %u ms\n",
            lotserver_min_flight_ms);
    pr_info("  Avoidance Hold: %u ms\n", lotserver_avoid_hold_ms);
    pr_info("  Congestion: loss %u%%/%u%% | RTT +%u%% for %u rounds\n",
            lotserver_loss_congest_pct, lotserver_loss_recover_pct,
            lotserver_rtt_tolerance_pct, lotserver_rtt_confirm_samples);
    pr_info("  Loss Guard: %s | High Delay: %s (%uus, +%u%% gain)\n",
            lotserver_loss_guard ? "ON" : "OFF",
            lotserver_hd_enable ? "ON" : "OFF",
            lotserver_hd_thresh_us, lotserver_hd_gain_boost);

    return tcp_register_congestion_control(&lotspeed_ops);
}

static void __exit lotspeed_module_exit(void)
{
    u64 total_bytes;
    u64 gb_sent, mb_sent;
    int active_conns;

    pr_info("lotspeed: [uk0@%s] Beginning module unload\n", CURRENT_TIMESTAMP);

    tcp_unregister_congestion_control(&lotspeed_ops);
    pr_info("lotspeed: Unregistered from TCP stack\n");

    active_conns = atomic_read(&active_connections);
    total_bytes = atomic64_read(&total_bytes_sent);
    gb_sent = total_bytes >> 30;
    mb_sent = (total_bytes >> 20) & 0x3FF;

    // v2.1风格的卸载统计
    pr_info("╔════════════════════════════════════════════════════════╗\n");
    pr_info("║        LotSpeed v3.10.2 Unloaded                       ║\n");
    pr_info("║          Time: %s                     ║\n", CURRENT_TIMESTAMP);
    pr_info("║          User: uk0                                     ║\n");
    pr_info("║          Active Connections: %-26d║\n", active_conns);
    pr_info("║          Total Losses: %-32d║\n", atomic_read(&total_losses));
    pr_info("║          Data Sent: %llu.%llu GB%*s║\n",
            gb_sent, mb_sent * 1000 / 1024,
            (int)(30 - snprintf(NULL, 0, "%llu.%llu GB", gb_sent, mb_sent * 1000 / 1024)), "");
    pr_info("╚════════════════════════════════════════════════════════╝\n");
}

module_init(lotspeed_module_init);
module_exit(lotspeed_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("uk0 <github.com/uk0>");
MODULE_VERSION("3.10.2-enhanced");
MODULE_DESCRIPTION("LotSpeed v3.10.2 - strict adaptive Mux with qualified congestion");
MODULE_ALIAS("tcp_lotspeed");
