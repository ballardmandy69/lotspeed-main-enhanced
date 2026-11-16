/*
 * Pure TCP duplication policy. Caller supplies bool, u32, u64 and s32.
 * Calls on a shared budget must be serialized by the caller.
 * @author lyt
 * @date 2026-11-17
 */
#ifndef LS_DUP_POLICY_H
#define LS_DUP_POLICY_H

#define LS_DUP_CREDIT_CAP 65536U

/* All-port scope is explicit; an omitted/zero port alone must never widen it. */
static inline bool lsdup_scope_valid(u32 selected_port, bool all_ports)
{
	return all_ports ? selected_port == 0 : selected_port > 0 && selected_port <= 65535U;
}

static inline bool lsdup_scope_matches(u32 selected_port, bool all_ports, u32 local_port)
{
	return lsdup_scope_valid(selected_port, all_ports) && local_port > 0 &&
	       local_port <= 65535U && (all_ports || selected_port == local_port);
}

/* TCP serial ordering is meaningful only within half the sequence space. */
static inline bool lsdup_before(u32 a, u32 b)
{
	return (s32)(a - b) < 0;
}

/* Half-open ranges, each shorter than 2^31; equal endpoints are empty. */
static inline bool lsdup_overlap(u32 a, u32 b, u32 c, u32 d)
{
	if (a == b || c == d)
		return 0;
	return (u32)(c - a) < (u32)(b - a) ||
	       (u32)(a - c) < (u32)(d - c);
}

static inline u32 lsdup_prefix_bytes(u32 seq, u32 len, u32 prefix_end,
				     bool prefix_done)
{
	u32 remaining;

	if (prefix_done || !lsdup_before(seq, prefix_end))
		return 0;
	remaining = prefix_end - seq;
	return len < remaining ? len : remaining;
}

struct lsdup_budget {
	u64 credit;
	u64 tokens;
	u64 stamp_ns;
	u32 remainder;
};

static inline void lsdup_budget_init(struct lsdup_budget *budget, u64 now_ns)
{
	budget->credit = 0;
	budget->tokens = 0;
	budget->stamp_ns = now_ns;
	budget->remainder = 0;
}

/* pct is 1..100. Original GSO wire accounting includes per-segment headers
 * and can reach about 6.6 MB; bytes * pct + remainder must fit u32.
 */
static inline void lsdup_budget_earn(struct lsdup_budget *budget,
				    u32 original_wire_bytes, u32 pct)
{
	u32 scaled = original_wire_bytes * pct + budget->remainder;
	u64 credit = budget->credit + scaled / 100U;

	budget->remainder = scaled % 100U;
	budget->credit = credit > LS_DUP_CREDIT_CAP ? LS_DUP_CREDIT_CAP : credit;
}

/* max_mbps is 1..1000 decimal Mbps. Failed takes may refill, never debit. */
static inline bool lsdup_budget_take(struct lsdup_budget *budget,
				    u32 duplicate_wire_bytes, u32 max_mbps,
				    u64 now_ns)
{
	u32 burst = max_mbps * 1250U;
	u64 delta;
	u32 elapsed;
	u32 refill;

	if (burst < 2048U)
		burst = 2048U;
	if (budget->tokens > burst)
		budget->tokens = burst;
	/* Keep the high-water timestamp across clock regressions. */
	if (now_ns > budget->stamp_ns) {
		delta = now_ns - budget->stamp_ns;
		budget->stamp_ns = now_ns;
		elapsed = delta > 1000000000ULL ? 1000000000U : (u32)delta;
		/* Exact floor(elapsed * Mbps / 8000), without 64-bit division.
		 * Each product fits u32; fractional tokens are discarded.
		 */
		refill = (elapsed / 8000U) * max_mbps +
			 (elapsed % 8000U) * max_mbps / 8000U;
		if (refill >= burst - budget->tokens)
			budget->tokens = burst;
		else
			budget->tokens += refill;
	}
	if (budget->credit < duplicate_wire_bytes ||
	    budget->tokens < duplicate_wire_bytes)
		return 0;
	budget->credit -= duplicate_wire_bytes;
	budget->tokens -= duplicate_wire_bytes;
	return 1;
}

#endif /* LS_DUP_POLICY_H */
