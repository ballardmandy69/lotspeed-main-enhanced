// lotspeed.c - v3.4.1 enhanced mainline
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

// Linux 6.10 restored ack/flag arguments to cong_control().
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#define LOTSPEED_NEW_CONG_CONTROL_API 1
#else
#define LOTSPEED_OLD_CONG_CONTROL_API 1
#endif

// --- 可调参数 ---
static unsigned long lotserver_rate = 125000000ULL;  // 1Gbps 最高速率上限
static unsigned int lotserver_gain = 15;               // 1.5x 默认增益 (BBR-style)
static unsigned int lotserver_min_cwnd = 32;           // 最小拥塞窗口
static unsigned int lotserver_max_cwnd = 10000;        // 最大拥塞窗口
static unsigned int lotserver_beta = 616;              // about 60% cwnd on congestion
static bool lotserver_adaptive = true;
static bool lotserver_turbo = false;
static bool lotserver_verbose = false;
static unsigned int lotserver_pacing_gain = 120;       // pacing rate percent
static unsigned int lotserver_probe_rtt_interval_ms = 30000;
static unsigned int lotserver_probe_rtt_duration_ms = 150;
static unsigned int lotserver_probe_rtt_cwnd_pct = 50;
static unsigned int lotserver_min_rtt_window_sec = 10;
static unsigned int lotserver_rtt_tolerance_pct = 25;
static bool lotserver_loss_guard = true;
static unsigned int lotserver_noncong_beta = 972;      // 95% cwnd on likely random loss
static bool lotserver_hd_enable = true;
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

static int param_set_percent(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    unsigned int *value = kp->arg;

    if (!ret)
        *value = clamp_t(unsigned int, *value, 1, 200);
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
static const struct kernel_param_ops param_ops_gain = { .set = param_set_gain, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_min_cwnd = { .set = param_set_min_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_max_cwnd = { .set = param_set_max_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_adaptive = { .set = param_set_adaptive, .get = param_get_bool, };
static const struct kernel_param_ops param_ops_turbo = { .set = param_set_turbo, .get = param_get_bool, };
static const struct kernel_param_ops param_ops_beta = { .set = param_set_beta, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_percent = { .set = param_set_percent, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_msec = { .set = param_set_msec, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_usec = { .set = param_set_usec, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_seconds = { .set = param_set_seconds, .get = param_get_uint, };

// --- 注册参数 ---
module_param_cb(lotserver_rate, &param_ops_rate, &lotserver_rate, 0644);
MODULE_PARM_DESC(lotserver_rate, "Target rate in bytes/sec (default 1Gbps)");

module_param_cb(lotserver_gain, &param_ops_gain, &lotserver_gain, 0644);
MODULE_PARM_DESC(lotserver_gain, "Gain multiplier x10 (20 = 2.0x)");

module_param_cb(lotserver_min_cwnd, &param_ops_min_cwnd, &lotserver_min_cwnd, 0644);
MODULE_PARM_DESC(lotserver_min_cwnd, "Minimum congestion window");

module_param_cb(lotserver_max_cwnd, &param_ops_max_cwnd, &lotserver_max_cwnd, 0644);
MODULE_PARM_DESC(lotserver_max_cwnd, "Maximum congestion window");

module_param_cb(lotserver_adaptive, &param_ops_adaptive, &lotserver_adaptive, 0644);
MODULE_PARM_DESC(lotserver_adaptive, "Enable adaptive rate control");

module_param_cb(lotserver_turbo, &param_ops_turbo, &lotserver_turbo, 0644);
MODULE_PARM_DESC(lotserver_turbo, "Turbo mode - ignore all congestion signals");

module_param_cb(lotserver_beta, &param_ops_beta, &lotserver_beta, 0644);
MODULE_PARM_DESC(lotserver_beta, "Beta for congestion backoff (default 616/1024)");

module_param(lotserver_verbose, bool, 0644);
MODULE_PARM_DESC(lotserver_verbose, "Enable verbose logging");

module_param_cb(lotserver_pacing_gain, &param_ops_percent, &lotserver_pacing_gain, 0644);
MODULE_PARM_DESC(lotserver_pacing_gain, "Pacing gain percent (default 120)");

module_param_cb(lotserver_probe_rtt_interval_ms, &param_ops_msec, &lotserver_probe_rtt_interval_ms, 0644);
MODULE_PARM_DESC(lotserver_probe_rtt_interval_ms, "ProbeRTT interval in milliseconds");

module_param_cb(lotserver_probe_rtt_duration_ms, &param_ops_msec, &lotserver_probe_rtt_duration_ms, 0644);
MODULE_PARM_DESC(lotserver_probe_rtt_duration_ms, "ProbeRTT duration in milliseconds");

module_param_cb(lotserver_probe_rtt_cwnd_pct, &param_ops_percent, &lotserver_probe_rtt_cwnd_pct, 0644);
MODULE_PARM_DESC(lotserver_probe_rtt_cwnd_pct, "Percent of prior cwnd retained during ProbeRTT");

module_param_cb(lotserver_min_rtt_window_sec, &param_ops_seconds, &lotserver_min_rtt_window_sec, 0644);
MODULE_PARM_DESC(lotserver_min_rtt_window_sec, "Minimum RTT refresh window in seconds");

module_param_cb(lotserver_rtt_tolerance_pct, &param_ops_percent, &lotserver_rtt_tolerance_pct, 0644);
MODULE_PARM_DESC(lotserver_rtt_tolerance_pct, "RTT inflation tolerance percent");

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

// --- v3.4 per-connection state ---
struct lotspeed {
    // Keep u64 fields together so the structure stays below older kernels'
    // congestion-control private-state limit.
    u64 target_rate;
    u64 actual_rate;
    u64 last_bw;
    u64 bytes_sent;
    u64 start_time;

    u32 cwnd_gain;
    enum lotspeed_state state;
    u32 last_state_ts;
    u32 probe_rtt_ts;
    u32 last_cruise_ts;
    u32 rtt_min;
    u32 rtt_cnt;
    u32 loss_count;
    u32 rtt_dev;
    u32 min_rtt_stamp;
    u32 probe_prior_cwnd;
    u32 bw_stalled_rounds;
    u32 probe_cnt;
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

        // 特殊状态处理
        if (new_state == CRUISING) {
            ca->last_cruise_ts = tcp_jiffies32;
        }
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
    ca->last_cruise_ts = 0;
    ca->min_rtt_stamp = tcp_jiffies32;
    ca->probe_cnt = tcp_jiffies32;

    // 初始目标速率设为全局上限，让智能启动去探索
    ca->target_rate = lotserver_rate;
    ca->cwnd_gain = lotserver_gain;
    ca->start_time = ktime_get_real_seconds();

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
        unsigned long gbps_int = ca->target_rate / 125000000;
        unsigned long gbps_frac = (ca->target_rate % 125000000) * 100 / 125000000;
        unsigned int gain_int = ca->cwnd_gain / 10;
        unsigned int gain_frac = ca->cwnd_gain % 10;

        pr_info("lotspeed: [uk0@%s] NEW connection #%d | rate=%lu.%02lu Gbps | gain=%u.%ux | mode=%s | state=%s\n",
                CURRENT_TIMESTAMP,
                atomic_read(&active_connections),
                gbps_int, gbps_frac,
                gain_int, gain_frac,
                lotserver_turbo ? "TURBO" : (lotserver_adaptive ? "adaptive" : "fixed"),
                state_to_str(ca->state));
    }
}

// 释放连接
static void lotspeed_release(struct sock *sk)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 duration;

    if (!ca) {
        pr_warn("lotspeed: [uk0@%s] release called with NULL ca\n", CURRENT_TIMESTAMP);
        atomic_dec(&active_connections);
        return;
    }

    // 计算连接持续时间
    if (ca->start_time > 0) {
        duration = ktime_get_real_seconds() - ca->start_time;
    } else {
        duration = 0;
    }

    atomic_dec(&active_connections);

    if (ca->bytes_sent > 0) {
        atomic64_add(ca->bytes_sent, &total_bytes_sent);
    }
    if (ca->loss_count > 0) {
        atomic_add(ca->loss_count, &total_losses);
    }

    if (lotserver_verbose) {
        u64 mb_sent = ca->bytes_sent >> 20;
        pr_info("lotspeed: [uk0@%s] connection released | duration=%llu s | sent=%llu MB | losses=%u | active=%d\n",
                CURRENT_TIMESTAMP, duration, mb_sent, ca->loss_count,
                atomic_read(&active_connections));
    }

    memset(ca, 0, sizeof(struct lotspeed));
}

// 更新 RTT 统计
static void lotspeed_update_rtt(struct sock *sk, u32 rtt_us)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    u32 now = tcp_jiffies32;
    u32 diff;
    bool expired;

    if (!rtt_us)
        return;

    expired = time_after32(now, ca->min_rtt_stamp +
                          lotserver_min_rtt_window_sec * HZ);

    // Allow min_rtt to follow a path whose baseline moved upward.
    if (!ca->rtt_min || rtt_us < ca->rtt_min || expired) {
        if (lotserver_verbose && ca->rtt_min > 0 && rtt_us < ca->rtt_min)
            pr_info("lotspeed: [uk0@%s] new min_rtt: %u us (was %u)\n",
                    CURRENT_TIMESTAMP, rtt_us, ca->rtt_min);
        ca->rtt_min = rtt_us;
        ca->min_rtt_stamp = now;
    }

    diff = rtt_us > ca->rtt_min ? rtt_us - ca->rtt_min : 0;
    ca->rtt_dev = (ca->rtt_dev * 3 + diff) / 4;
    ca->rtt_cnt++;
}

static bool lotspeed_rtt_inflated(const struct lotspeed *ca, u32 rtt_us)
{
    u64 threshold;

    if (!ca->rtt_min || !rtt_us)
        return false;

    threshold = ca->rtt_min;
    threshold += div_u64((u64)ca->rtt_min * lotserver_rtt_tolerance_pct, 100);
    threshold += ca->rtt_dev;
    return rtt_us > threshold;
}

static u64 lotspeed_scale_percent(u64 value, u32 percent)
{
    if (!percent)
        return 0;
    if (value > div64_u64(LOTSPEED_MAX_U64, percent))
        return LOTSPEED_MAX_U64;
    return div64_u64(value * percent, 100);
}

// --- v3.4 core: fixed-rate baseline plus corrected adaptive mode ---
static void lotspeed_adapt_and_control(struct sock *sk, const struct rate_sample *rs, int flag)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 sample_rate = 0;
    u64 delivered_bytes;
    u64 pacing_rate;
    u32 rtt_us = tp->srtt_us >> 3;
    u32 cwnd;
    u32 target_cwnd = 0;
    u32 mss = tp->mss_cache ? : 1460;
    u32 now = tcp_jiffies32;
    u32 effective_gain;
    u32 pipe;
    bool congestion_detected = false;
    bool rtt_inflated;
    bool high_delay_path;

    lotspeed_update_rtt(sk, rtt_us);
    if (!rtt_us)
        rtt_us = ca->rtt_min ? : 1000;

    // rate_sample::delivered is packets, not bytes.
    if (rs && rs->delivered > 0 && rs->interval_us > 0) {
        delivered_bytes = (u64)rs->delivered * mss;
        sample_rate = div64_u64(delivered_bytes * USEC_PER_SEC,
                                (u64)rs->interval_us);
        sample_rate = min_t(u64, sample_rate,
                            lotspeed_scale_percent(lotserver_rate, 200));
        ca->bytes_sent += delivered_bytes;
        ca->actual_rate = ca->actual_rate ?
                          ca->actual_rate - ca->actual_rate / 8 +
                          sample_rate / 8 :
                          sample_rate;
    }

    rtt_inflated = lotspeed_rtt_inflated(ca, rtt_us);
    high_delay_path = lotserver_hd_enable &&
                      ca->rtt_min >= lotserver_hd_thresh_us;

    if (!lotserver_turbo) {
        if (flag & CA_ACK_ECE)
            congestion_detected = true;
        if (rtt_inflated)
            congestion_detected = true;
        if (rs && rs->losses > 0 &&
            (!lotserver_loss_guard || rtt_inflated))
            congestion_detected = true;
    }

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
            } else if (lotserver_adaptive && ca->actual_rate > 0 &&
                       time_after32(now, ca->probe_cnt +
                                    msecs_to_jiffies(LOTSPEED_RATE_SAMPLE_MS))) {
                if (!ca->last_bw ||
                    ca->actual_rate * LOTSPEED_BETA_SCALE >
                    ca->last_bw * LOTSPEED_STARTUP_GROWTH_TARGET) {
                    ca->last_bw = ca->actual_rate;
                    ca->bw_stalled_rounds = 0;
                } else {
                    ca->bw_stalled_rounds++;
                }
                ca->probe_cnt = now;
                if (ca->bw_stalled_rounds >= LOTSPEED_STARTUP_EXIT_ROUNDS) {
                    ca->target_rate = min_t(u64, lotserver_rate,
                                            lotspeed_scale_percent(
                                                ca->actual_rate, 105));
                    ca->ss_mode = false;
                    enter_state(sk, PROBING);
                }
            } else if (!lotserver_adaptive &&
                       time_after32(now, ca->last_state_ts +
                                    msecs_to_jiffies(LOTSPEED_PROBE_RATE_MS))) {
                ca->ss_mode = false;
                enter_state(sk, CRUISING);
            }
            break;

        case PROBING:
            if (congestion_detected) {
                enter_state(sk, AVOIDING);
            } else if (!lotserver_adaptive) {
                enter_state(sk, CRUISING);
            } else if (time_after32(now, ca->probe_cnt +
                                    msecs_to_jiffies(LOTSPEED_PROBE_RATE_MS))) {
                ca->target_rate = min_t(u64, lotserver_rate,
                                        lotspeed_scale_percent(
                                            ca->target_rate, 105));
                ca->probe_cnt = now;
                if (ca->actual_rate >
                    lotspeed_scale_percent(ca->target_rate, 90))
                    enter_state(sk, CRUISING);
            }
            break;

        case CRUISING:
            if (congestion_detected) {
                enter_state(sk, AVOIDING);
            } else if (lotserver_adaptive && ca->actual_rate > 0) {
                ca->target_rate = clamp_t(u64,
                                          lotspeed_scale_percent(
                                              ca->actual_rate, 105),
                                          max_t(u64, lotserver_rate / 20, 125000),
                                          (u64)lotserver_rate);
            }
            if (!congestion_detected && lotserver_adaptive &&
                time_after32(now, ca->last_cruise_ts +
                             msecs_to_jiffies(LOTSPEED_CRUISE_TIME_MS))) {
                ca->probe_cnt = now;
                enter_state(sk, PROBING);
            }
            break;

        case AVOIDING:
            if (lotserver_adaptive && ca->actual_rate > 0) {
                ca->target_rate = clamp_t(u64,
                                          lotspeed_scale_percent(
                                              ca->actual_rate, 95),
                                          max_t(u64, lotserver_rate / 20, 125000),
                                          (u64)lotserver_rate);
            }
            if (!congestion_detected &&
                time_after32(now, ca->last_state_ts +
                             msecs_to_jiffies(LOTSPEED_PROBE_RATE_MS))) {
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

    if (!lotserver_adaptive)
        ca->target_rate = lotserver_rate;

    effective_gain = lotserver_gain;
    if (high_delay_path)
        effective_gain = min_t(u32, LOTSPEED_MAX_GAIN,
                               effective_gain +
                               effective_gain * lotserver_hd_gain_boost / 100);

    switch (ca->state) {
        case STARTUP:
            ca->cwnd_gain = min_t(u32, LOTSPEED_MAX_GAIN,
                                  effective_gain * 12 / 10);
            if (lotserver_adaptive)
                ca->target_rate = lotserver_rate;
            break;
        case PROBING:
            ca->cwnd_gain = effective_gain;
            break;
        case CRUISING:
            ca->cwnd_gain = effective_gain;
            break;
        case AVOIDING:
            ca->cwnd_gain = max_t(u32, effective_gain * 9 / 10, 10);
            break;
        case PROBE_RTT:
            ca->cwnd_gain = effective_gain;
            break;
    }

    ca->target_rate = clamp_t(u64, ca->target_rate, 125000,
                              (u64)lotserver_rate);

    if (mss > 0 && rtt_us > 0 &&
        ca->target_rate <= div64_u64(LOTSPEED_MAX_U64, rtt_us)) {
        u64 bdp = ca->target_rate * rtt_us;

        target_cwnd = (u32)min_t(u64,
            div64_u64(bdp, (u64)mss * USEC_PER_SEC), LOTSPEED_MAX_U32);
        target_cwnd = (u32)min_t(u64,
            div64_u64((u64)target_cwnd * ca->cwnd_gain, 10),
            LOTSPEED_MAX_U32);
    }

    if (ca->state == PROBE_RTT) {
        u32 retained = (u32)div_u64((u64)ca->probe_prior_cwnd *
                                    min_t(u32, lotserver_probe_rtt_cwnd_pct, 100),
                                    100);
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
    pacing_rate = lotspeed_scale_percent(ca->target_rate,
                                         lotserver_pacing_gain);
    sk->sk_pacing_rate = min_t(u64, pacing_rate, sk->sk_max_pacing_rate);
#endif

    if (lotserver_verbose && ca->rtt_cnt > 0 && ca->rtt_cnt % 1000 == 0) {
        unsigned long gbps_int = ca->target_rate / 125000000;
        unsigned long gbps_frac = (ca->target_rate % 125000000) * 100 / 125000000;
        unsigned int gain_int = ca->cwnd_gain / 10;
        unsigned int gain_frac = ca->cwnd_gain % 10;

        pr_info("lotspeed: [uk0@%s] STATUS: [%s] cwnd=%u | rate=%lu.%02lu Gbps | RTT=%u us | gain=%u.%ux | losses=%u\n",
                CURRENT_TIMESTAMP, state_to_str(ca->state), tp->snd_cwnd,
                gbps_int, gbps_frac, rtt_us, gain_int, gain_frac, ca->loss_count);
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
    u32 rtt_us = tp->srtt_us >> 3;
    u32 beta = lotserver_beta;
    u64 reduced;

    if (lotserver_turbo) {
        return TCP_INFINITE_SSTHRESH;
    }

    ca->loss_count++;
    ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 10);

    // Random loss without RTT inflation gets a gentler response.
    if (lotserver_loss_guard && !lotspeed_rtt_inflated(ca, rtt_us))
        beta = max(beta, lotserver_noncong_beta);

    reduced = div_u64((u64)tp->snd_cwnd * beta, LOTSPEED_BETA_SCALE);
    return max_t(u32, (u32)min_t(u64, reduced, LOTSPEED_MAX_U32),
                 lotserver_min_cwnd);
}

// 处理状态变化 (TCP_CA_Loss)
static void lotspeed_set_state_hook(struct sock *sk, u8 new_state)
{
    struct lotspeed *ca = inet_csk_ca(sk);

    switch (new_state) {
        case TCP_CA_Loss:
            if (lotserver_turbo) {
                if (lotserver_verbose && ca->loss_count % 10 == 0) {
                    pr_info("lotspeed: [uk0@%s] TURBO: Ignoring loss #%u\n",
                            CURRENT_TIMESTAMP, ca->loss_count + 1);
                }
                return;
            }
            enter_state(sk, AVOIDING);

            if (lotserver_verbose && (ca->loss_count == 1 || ca->loss_count % 10 == 0)) {
                unsigned int gain_int = ca->cwnd_gain / 10;
                unsigned int gain_frac = ca->cwnd_gain % 10;
                pr_info("lotspeed: [uk0@%s] LOSS #%u detected, gain reduced to %u.%ux\n",
                        CURRENT_TIMESTAMP, ca->loss_count, gain_int, gain_frac);
            }
            break;

        case TCP_CA_Recovery:
            if (!lotserver_turbo) {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 15);
            }
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

    // 误判恢复，重置丢包计数
    ca->loss_count = 0;
    ca->ss_mode = false;

    return min_t(u32, max(tp->snd_cwnd, tp->prior_cwnd),
                 tp->snd_cwnd_clamp);
}

static void lotspeed_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct lotspeed *ca = inet_csk_ca(sk);

    switch (event) {
        case CA_EVENT_LOSS:
            if (!lotserver_turbo) {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 10);
            }
            break;

        case CA_EVENT_TX_START:
            ca->ss_mode = true;
            ca->probe_cnt = tcp_jiffies32;
            break;

        case CA_EVENT_CWND_RESTART:
            ca->ss_mode = true;
            ca->loss_count = 0;
            ca->probe_cnt = tcp_jiffies32;
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
    pr_info("║      LotSpeed v3.4.1 - enhanced mainline                ║\n");

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
    pr_info("  Adaptive: %s | Turbo: %s | Verbose: %s\n",
             lotserver_adaptive ? "ON" : "OFF",
             lotserver_turbo ? "ON" : "OFF",
             lotserver_verbose ? "ON" : "OFF");
    pr_info("  Pacing Gain: %u%% | ProbeRTT: %ums/%ums/%u%% cwnd\n",
            lotserver_pacing_gain, lotserver_probe_rtt_interval_ms,
            lotserver_probe_rtt_duration_ms, lotserver_probe_rtt_cwnd_pct);
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
    pr_info("║          LotSpeed v3.4.1 Unloaded                      ║\n");
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
MODULE_VERSION("3.4.1-enhanced");
MODULE_DESCRIPTION("LotSpeed v3.4.1 - fixed-rate WAN and corrected adaptive congestion control");
MODULE_ALIAS("tcp_lotspeed");
