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
#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))
#define div64_u64(a, b) ((u64)(a) / (u64)(b))
#define div_u64(a, b) div64_u64(a, b)
#define msecs_to_jiffies(ms) ((u32)(((u64)(ms) * HZ + 999) / 1000))
#define jiffies_to_usecs(j) ((u64)(j) * USEC_PER_SEC / HZ)
#define time_after32(a, b) ((int32_t)((u32)(b) - (u32)(a)) < 0)
#define before(a, b) ((int32_t)((u32)(a) - (u32)(b)) < 0)
#define pr_info(...) ((void)0)

static u32 tcp_jiffies32;
struct rate_sample {
    u32 prior_delivered;
    bool is_app_limited;
};
struct tcp_sock {
    u32 delivered, lost, write_seq, snd_una, packets_out;
};
enum tcp_ca_event { CA_EVENT_TX_START, CA_EVENT_CWND_RESTART };
