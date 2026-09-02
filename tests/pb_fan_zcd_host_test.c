// SPDX-License-Identifier: MIT
#include "pb_fan_zcd.h"

#include <inttypes.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static bool edge(pb_fan_zcd_filter_t *filter, uint64_t at_us, uint32_t expected)
{
    uint32_t interval_us = UINT32_MAX;
    bool accepted = pb_fan_zcd_accept(filter, at_us, &interval_us);
    if (accepted) CHECK(interval_us == expected);
    return accepted;
}

static void test_nominal_50_and_60_hz(void)
{
    pb_fan_zcd_filter_t filter = {0};
    CHECK(edge(&filter, 1000, 0));
    CHECK(edge(&filter, 9333, 8333));
    CHECK(edge(&filter, 19333, 10000));
}

static void test_duplicate_and_repeated_noise_rejected(void)
{
    pb_fan_zcd_filter_t filter = {0};
    CHECK(edge(&filter, 10000, 0));
    CHECK(!edge(&filter, 11000, 0));
    CHECK(!edge(&filter, 11500, 0));
    CHECK(!edge(&filter, 12000, 0));
    CHECK(!edge(&filter, 12500, 0));
    CHECK(edge(&filter, 18333, 8333));
}

static void test_rejected_edge_does_not_move_window(void)
{
    pb_fan_zcd_filter_t filter = {0};
    CHECK(edge(&filter, 50000, 0));
    CHECK(!edge(&filter, 51000, 0));
    CHECK(edge(&filter, 60000, 10000));
}

static void test_threshold_boundary(void)
{
    pb_fan_zcd_filter_t filter = {0};
    CHECK(edge(&filter, 1000, 0));
    CHECK(!edge(&filter, 4999, 0));
    CHECK(edge(&filter, 5000, PB_FAN_ZCD_MIN_SPACING_US));
}

static void test_uint64_timestamp_wrap(void)
{
    pb_fan_zcd_filter_t filter = {0};
    CHECK(edge(&filter, UINT64_MAX - 5000, 0));
    CHECK(edge(&filter, 3332, 8333));
}

int main(void)
{
    CHECK(PB_FAN_ZCD_MIN_SPACING_US < 8333U);
    test_nominal_50_and_60_hz();
    test_duplicate_and_repeated_noise_rejected();
    test_rejected_edge_does_not_move_window();
    test_threshold_boundary();
    test_uint64_timestamp_wrap();

    if (failures) return 1;
    puts("pb_fan ZCD qualifier host tests: PASS");
    return 0;
}
