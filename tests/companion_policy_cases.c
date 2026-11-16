/*
 * Deterministic tests of the actual companion policy, including serial wrap.
 * @author lyt
 * @date 2026-11-17
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

#include "companion/policy.h"
#include "companion/policy.h"

static void test_scope(void)
{
	const u32 invalid[] = { 0, 65536U, 0x80000000U, UINT32_MAX };
	u32 port;
	unsigned int i;

	assert(lsdup_scope_valid(0, true));
	assert(!lsdup_scope_valid(0, false));
	assert(lsdup_scope_valid(1, false));
	assert(lsdup_scope_valid(65535, false));
	assert(!lsdup_scope_valid(1, true));
	assert(!lsdup_scope_valid(65535, true));
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		assert(!lsdup_scope_valid(invalid[i], false));
		assert(lsdup_scope_valid(invalid[i], true) == (invalid[i] == 0));
		assert(!lsdup_scope_matches(0, true, invalid[i]));
		assert(!lsdup_scope_matches(443, false, invalid[i]));
		assert(!lsdup_scope_matches(invalid[i], false, 443));
		assert(!lsdup_scope_matches(invalid[i], false, invalid[i]));
		assert(lsdup_scope_matches(invalid[i], true, 443) == (invalid[i] == 0));
	}
	for (port = 1; port <= 65535U; port++) {
		assert(lsdup_scope_valid(port, false));
		assert(!lsdup_scope_valid(port, true));
		assert(lsdup_scope_matches(0, true, port));
		assert(lsdup_scope_matches(port, false, port));
		assert(!lsdup_scope_matches(port, true, port));
		assert(!lsdup_scope_matches(port, true, 443));
		assert(lsdup_scope_matches(443, false, port) == (port == 443));
		assert(lsdup_scope_matches(1, false, port) == (port == 1));
		assert(lsdup_scope_matches(65535, false, port) == (port == 65535));
		assert(!lsdup_scope_matches(port, false, port == 65535U ? 1U : port + 1U));
	}
}

static void test_before(void)
{
	/* The existing Python runner invokes this group; no runner edit is needed. */
	test_scope();
	assert(!lsdup_before(0, 0));
	assert(lsdup_before(0, 1));
	assert(!lsdup_before(1, 0));
	assert(lsdup_before(UINT32_MAX, 0));
	assert(!lsdup_before(0, UINT32_MAX));
	assert(lsdup_before(0x7fffffffU, 0x80000000U));
	assert(lsdup_before(0, 0x7fffffffU));
	assert(!lsdup_before(0x7fffffffU, 0));
}

/* Independent oracle: lift both arcs onto a signed 64-bit number line. */
static bool overlap_reference(u32 a, u32 b, u32 c, u32 d)
{
	int64_t start_a = a;
	int64_t end_a = start_a + (u32)(b - a);
	int64_t start_c;
	int64_t end_c;
	int shift;

	if (a == b || c == d)
		return false;
	for (shift = -1; shift <= 1; shift++) {
		start_c = (int64_t)c + (int64_t)shift * 4294967296LL;
		end_c = start_c + (u32)(d - c);
		if (start_a < end_c && start_c < end_a)
			return true;
	}
	return false;
}

static u32 random_word(u32 *state)
{
	*state = *state * 1664525U + 1013904223U;
	return *state;
}

static void test_overlap(void)
{
	u32 state = 42;
	u32 a, b, c, d;
	unsigned int i;

	assert(!lsdup_overlap(10, 10, 0, 20));
	assert(!lsdup_overlap(0, 20, 10, 10));
	assert(!lsdup_overlap(10, 20, 20, 30));
	assert(!lsdup_overlap(20, 30, 10, 20));
	assert(lsdup_overlap(10, 20, 10, 20));
	assert(lsdup_overlap(10, 30, 15, 20));
	assert(lsdup_overlap(15, 20, 10, 30));
	assert(lsdup_overlap(0xfffffff0U, 16, 0, 32));
	assert(lsdup_overlap(0xfffffff0U, 16, 0xffffffe0U, 0));
	assert(!lsdup_overlap(0xfffffff0U, 16, 16, 32));
	assert(!lsdup_overlap(0, 16, 0x80000000U, 0x80000010U));
	assert(lsdup_overlap(0, 0x7fffffffU, 0x7ffffffeU, 0xfffffffdU));
	for (i = 0; i < 100000; i++) {
		a = random_word(&state);
		b = a + (random_word(&state) & 0x7fffffffU);
		c = random_word(&state);
		d = c + (random_word(&state) & 0x7fffffffU);
		assert(lsdup_overlap(a, b, c, d) == overlap_reference(a, b, c, d));
		assert(lsdup_overlap(a, b, c, d) == lsdup_overlap(c, d, a, b));
	}
}

static void test_prefix(void)
{
	const u32 starts[] = { 0, 12345, 0xffff0000U, 0xfffffff0U };
	const u32 prefix = 128U * 1024U;
	u32 start, end, offset, expected;
	unsigned int i;

	for (i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
		start = starts[i];
		end = start + prefix;
		assert(lsdup_prefix_bytes(start, 65535, end, false) == 65535);
		assert(lsdup_prefix_bytes(end - 1, 65535, end, false) == 1);
		assert(lsdup_prefix_bytes(end - 100, 100, end, false) == 100);
		assert(lsdup_prefix_bytes(end - 100, 101, end, false) == 100);
		assert(lsdup_prefix_bytes(end, 1, end, false) == 0);
		assert(lsdup_prefix_bytes(end + 1, 65535, end, false) == 0);
		assert(lsdup_prefix_bytes(start, 65535, end, true) == 0);
		assert(lsdup_prefix_bytes(start, 0, end, false) == 0);
		for (offset = 0; offset < prefix + 65535U; offset += 127U) {
			expected = offset < prefix ? prefix - offset : 0;
			if (expected > 1460U)
				expected = 1460U;
			assert(lsdup_prefix_bytes(start + offset, 1460, end, false) == expected);
		}
	}
}

static void test_init(void)
{
	struct lsdup_budget budget = { 7, 8, 9, 10 };

	lsdup_budget_init(&budget, UINT64_MAX);
	assert(budget.credit == 0 && budget.tokens == 0);
	assert(budget.stamp_ns == UINT64_MAX && budget.remainder == 0);
	assert(!lsdup_budget_take(&budget, 1, 1000, UINT64_MAX));
	lsdup_budget_init(&budget, 0);
	assert(budget.stamp_ns == 0);
}

static void test_credit(void)
{
	struct lsdup_budget budget;
	unsigned int i;

	lsdup_budget_init(&budget, 0);
	for (i = 0; i < 99; i++)
		lsdup_budget_earn(&budget, 1, 1);
	assert(budget.credit == 0 && budget.remainder == 99);
	lsdup_budget_earn(&budget, 1, 1);
	assert(budget.credit == 1 && budget.remainder == 0);
	lsdup_budget_earn(&budget, 65535, 100);
	assert(budget.credit == LS_DUP_CREDIT_CAP);
	for (i = 0; i < 10000; i++)
		lsdup_budget_earn(&budget, 65535, 100);
	assert(budget.credit == 65536 && budget.tokens == 0);
	lsdup_budget_earn(&budget, 1, 99);
	assert(budget.credit == 65536 && budget.remainder == 99);
	assert(lsdup_budget_take(&budget, 65535, 1000, 10000000));
	assert(budget.credit == 1);
	lsdup_budget_earn(&budget, 1, 1);
	assert(budget.credit == 2 && budget.remainder == 0);
}

static void test_take(void)
{
	struct lsdup_budget budget;

	lsdup_budget_init(&budget, 0);
	assert(!lsdup_budget_take(&budget, 1, 1, 1000000000));
	assert(budget.credit == 0 && budget.tokens == 2048);
	lsdup_budget_earn(&budget, 100, 100);
	assert(!lsdup_budget_take(&budget, 101, 1, 1000000000));
	assert(budget.credit == 100 && budget.tokens == 2048);
	assert(lsdup_budget_take(&budget, 100, 1, 1000000000));
	assert(budget.credit == 0 && budget.tokens == 1948);
	lsdup_budget_init(&budget, 0);
	lsdup_budget_earn(&budget, 2000, 100);
	assert(!lsdup_budget_take(&budget, 1, 1, 0));
	assert(budget.credit == 2000 && budget.tokens == 0);
	assert(!lsdup_budget_take(&budget, 126, 1, 1000000));
	assert(budget.credit == 2000 && budget.tokens == 125);
	assert(lsdup_budget_take(&budget, 125, 1, 1000000));
	assert(budget.credit == 1875 && budget.tokens == 0);
	assert(!lsdup_budget_take(&budget, 1, 1, 1000000));
	assert(budget.credit == 1875 && budget.tokens == 0);
	assert(lsdup_budget_take(&budget, 0, 1, 1000000));
}

static void test_idle(void)
{
	struct lsdup_budget budget;
	unsigned int i;

	lsdup_budget_init(&budget, 0);
	lsdup_budget_earn(&budget, 65535, 100);
	assert(!lsdup_budget_take(&budget, 2049, 1, UINT64_MAX));
	assert(budget.credit == 65535 && budget.tokens == 2048);
	assert(budget.stamp_ns == UINT64_MAX);
	assert(lsdup_budget_take(&budget, 2048, 1, UINT64_MAX));
	for (i = 0; i < 1000; i++)
		assert(!lsdup_budget_take(&budget, 1, 1, UINT64_MAX));
	assert(budget.tokens == 0 && budget.credit == 63487);
	lsdup_budget_init(&budget, 0);
	assert(!lsdup_budget_take(&budget, 1, 1000, UINT64_MAX));
	assert(budget.tokens == 1250000);
}

static void test_clock(void)
{
	struct lsdup_budget budget;

	lsdup_budget_init(&budget, 10000000);
	lsdup_budget_earn(&budget, 1000, 100);
	assert(!lsdup_budget_take(&budget, 1, 1, 0));
	assert(budget.tokens == 0 && budget.stamp_ns == 10000000);
	assert(lsdup_budget_take(&budget, 125, 1, 11000000));
	assert(!lsdup_budget_take(&budget, 1, 1, 10000000));
	assert(!lsdup_budget_take(&budget, 1, 1, 11000000));
	assert(budget.stamp_ns == 11000000 && budget.credit == 875);
	assert(lsdup_budget_take(&budget, 125, 1, 12000000));
	assert(budget.tokens == 0 && budget.credit == 750);
}

static void test_ratios(void)
{
	struct lsdup_budget budget;
	u32 pct, i, bytes, state = 7;
	u64 earned, sent, now;

	for (pct = 1; pct <= 100; pct++) {
		lsdup_budget_init(&budget, 0);
		earned = 0;
		sent = 0;
		now = 0;
		for (i = 0; i < 1000; i++) {
			bytes = 1U + random_word(&state) % 65535U;
			earned += (u64)bytes * pct;
			lsdup_budget_earn(&budget, bytes, pct);
			assert(budget.credit == earned / 100U - sent);
			assert(budget.remainder == earned % 100U);
			now += 10000000U;
			bytes = (u32)budget.credit;
			assert(lsdup_budget_take(&budget, bytes, 1000, now));
			sent += bytes;
			assert(sent == earned / 100U);
			assert(budget.credit == 0);
		}
	}
}

static void test_rates(void)
{
	struct lsdup_budget budget;
	u32 mbps, bytes, elapsed, state = 19;
	u64 rate, burst, sent, expected;
	unsigned int i;

	for (mbps = 1; mbps <= 1000; mbps++) {
		rate = (u64)mbps * 125000U;
		burst = rate / 100U;
		if (burst < 2048)
			burst = 2048;
		lsdup_budget_init(&budget, 0);
		assert(!lsdup_budget_take(&budget, 1, mbps, 8000));
		assert(budget.tokens == mbps);
		assert(!lsdup_budget_take(&budget, 1, mbps, 1008000));
		assert(budget.tokens == mbps + rate / 1000U);
		assert(!lsdup_budget_take(&budget, 1, mbps, 1000000000));
		assert(budget.tokens == burst);
		lsdup_budget_init(&budget, 0);
		sent = 0;
		for (i = 1; i <= 1000; i++) {
			lsdup_budget_earn(&budget, 65535, 100);
			bytes = rate / 1000U > 65535U ? 65535U : (u32)(rate / 1000U);
			assert(lsdup_budget_take(&budget, bytes, mbps, (u64)i * 1000000U));
			sent += bytes;
			assert(sent <= rate * i / 1000U);
			assert(budget.tokens <= burst);
		}
	}
	/* Compare the division-free refill with full-width reference arithmetic. */
	for (i = 0; i < 100000; i++) {
		mbps = 1U + random_word(&state) % 1000U;
		elapsed = 1U + random_word(&state) % 20000000U;
		expected = (u64)elapsed * mbps / 8000U;
		burst = mbps * 1250U;
		if (burst < 2048U)
			burst = 2048U;
		if (expected > burst)
			expected = burst;
		lsdup_budget_init(&budget, 0);
		assert(!lsdup_budget_take(&budget, 1, mbps, elapsed));
		assert(budget.tokens == expected);
	}
	/* A reduced rate immediately clamps previously accumulated tokens. */
	lsdup_budget_init(&budget, 0);
	assert(!lsdup_budget_take(&budget, 1, 1000, 1000000000));
	assert(!lsdup_budget_take(&budget, 1, 1, 1000000000));
	assert(budget.tokens == 2048);
	/* Sub-byte refill cannot manufacture a byte. */
	lsdup_budget_init(&budget, 0);
	lsdup_budget_earn(&budget, 1, 100);
	assert(!lsdup_budget_take(&budget, 1, 1, 7999));
	assert(budget.tokens == 0 && budget.credit == 1);
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(void);
	} cases[] = {
		{ "scope", test_scope },
		{ "before", test_before }, { "overlap", test_overlap },
		{ "prefix", test_prefix }, { "init", test_init },
		{ "credit", test_credit }, { "take", test_take },
		{ "idle", test_idle }, { "clock", test_clock },
		{ "ratios", test_ratios }, { "rates", test_rates }
	};
	unsigned int i;

	if (argc != 2)
		return 2;
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (strcmp(argv[1], cases[i].name) == 0) {
			cases[i].run();
			printf("%s: ok\n", cases[i].name);
			return 0;
		}
	}
	return 2;
}
