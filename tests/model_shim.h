#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define USEC_PER_SEC 1000000ULL
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(t, a, b) min((t)(a), (t)(b))
#define max_t(t, a, b) max((t)(a), (t)(b))
#define clamp_t(t, value, low, high) min_t(t, max_t(t, value, low), high)
#define clamp(value, low, high) min(max(value, low), high)
#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))
#define div64_u64(a, b) ((u64)(a) / (u64)(b))
#define div_u64(a, b) div64_u64(a, b)
#define msecs_to_jiffies(ms) ((u32)(((u64)(ms) * HZ + 999) / 1000))
#define jiffies_to_usecs(j) ((u64)(j) * USEC_PER_SEC / HZ)
#define time_after32(a, b) ((int32_t)((u32)(b) - (u32)(a)) < 0)
#define before(a, b) ((int32_t)((u32)(a) - (u32)(b)) < 0)
#define pr_info(...) ((void)0)
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 8, 0)
#define WRITE_ONCE(value, new_value) ((value) = (new_value))

static u32 tcp_jiffies32;
struct rate_sample {
    u32 prior_delivered;
    bool is_app_limited;
    long rtt_us;
    int acked_sacked, losses;
};
struct tcp_sock {
    u32 delivered, lost, write_seq, snd_una, snd_nxt, packets_out, total_retrans;
    u32 srtt_us, mss_cache, snd_cwnd, snd_ssthresh, snd_cwnd_clamp;
    u32 sacked_out, lost_out, retrans_out;
};

static u32 tcp_packets_in_flight(const struct tcp_sock *tp)
{
    return tp->packets_out - (tp->sacked_out + tp->lost_out) + tp->retrans_out;
}
enum tcp_ca_event { CA_EVENT_TX_START, CA_EVENT_CWND_RESTART };
enum tcp_ca_state { TCP_CA_Open, TCP_CA_Disorder, TCP_CA_CWR,
                    TCP_CA_Recovery, TCP_CA_Loss, TCP_CA_Startup };
