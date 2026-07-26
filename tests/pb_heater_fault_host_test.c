// SPDX-License-Identifier: MIT
// Host test for the SAFETY-CRITICAL boot-time fault-restore decision
// (pb_heater_fault_decide) — a pure header inline, verified without an NVS
// backend. This logic is what guarantees a device that latched a safety fault
// comes back up LOCKED after a power cycle, and fails SAFE (latched) whenever the
// persisted fault state cannot be read reliably.
#include "pb_heater.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

// Float-equality check for the foldback duty (values are exact-ish ratios of 5).
#define FEQ(a, b) (fabsf((a) - (b)) < 1e-4f)

int main(void)
{
    pb_fault_reason_t code;

    // Fresh device: the NVS namespace was never written -> NOT latched.
    CHECK(pb_heater_fault_decide(false, /*ns_not_found=*/true,
                                 false, false, 0, 0, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Namespace exists but the latch key was never written -> NOT latched.
    CHECK(pb_heater_fault_decide(true, false,
                                 false, /*latch_not_found=*/true, 0, 0, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Latched with a valid stored code -> come up latched with that exact code.
    CHECK(pb_heater_fault_decide(true, false, /*latch_read_ok=*/true, false,
                                 1, PB_FAULT_CHAMBER_OVERTEMP, &code) == true);
    CHECK(code == PB_FAULT_CHAMBER_OVERTEMP);

    // Latch flag == 0 -> not latched even if a stale code sits in NVS.
    CHECK(pb_heater_fault_decide(true, false, true, false,
                                 0, PB_FAULT_PTC_OVERTEMP, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Latched but the stored code is out of range (corrupt) -> latch anyway,
    // mapped to a generic cause rather than trusting the garbage byte.
    CHECK(pb_heater_fault_decide(true, false, true, false, 1, 250, &code) == true);
    CHECK(code == PB_FAULT_EMERGENCY);

    // Latched with a NONE(0) code is inconsistent -> also generic, still latched.
    CHECK(pb_heater_fault_decide(true, false, true, false,
                                 1, PB_FAULT_NONE, &code) == true);
    CHECK(code == PB_FAULT_EMERGENCY);

    // FAIL-SAFE: a genuine nvs_open error (not "namespace not found") -> latch.
    CHECK(pb_heater_fault_decide(/*open_ok=*/false, false, false, false,
                                 0, 0, &code) == true);
    CHECK(code == PB_FAULT_NVS_UNREADABLE);

    // FAIL-SAFE: opened OK but the latch key read genuinely failed -> latch.
    CHECK(pb_heater_fault_decide(true, false, /*latch_read_ok=*/false,
                                 /*latch_not_found=*/false, 0, 0, &code) == true);
    CHECK(code == PB_FAULT_NVS_UNREADABLE);

    puts("pb_heater fault-restore checks: PASS");

    // --- Element-temperature foldback limiter (pb_heater_foldback_duty) ---------
    // Expectations are DERIVED from the two defines so the single-source-of-truth
    // #define PB_HEATER_PTC_FOLDBACK_START_C can be retuned without editing the test.
    const float start = PB_HEATER_PTC_FOLDBACK_START_C;
    const float cutoff = PB_HEATER_PTC_CUTOFF_C;
    const float span = cutoff - start;
    const float mid = start + span * 0.5f;   // duty 0.5
    const float qtr = start + span * 0.25f;  // duty 0.75
    // Below the foldback band the base demand passes through untouched.
    CHECK(FEQ(pb_heater_foldback_duty(1.0f, 25.0f), 1.0f));
    CHECK(FEQ(pb_heater_foldback_duty(1.0f, start - 1.0f), 1.0f));   // just below band -> full
    CHECK(FEQ(pb_heater_foldback_duty(1.0f, start), 1.0f));          // at start -> full
    // Inside the band the duty ramps linearly to 0 across the span.
    CHECK(FEQ(pb_heater_foldback_duty(1.0f, qtr), 0.75f));
    CHECK(FEQ(pb_heater_foldback_duty(1.0f, mid), 0.5f));
    // At/above the cutoff the foldback duty is 0 (the hard latching trip owns >= cutoff).
    CHECK(FEQ(pb_heater_foldback_duty(1.0f, cutoff), 0.0f));
    CHECK(FEQ(pb_heater_foldback_duty(1.0f, cutoff + 5.0f), 0.0f));
    // It only ever REDUCES: a base demand of 0 (chamber satisfied) stays 0 even when
    // the element is cool, and the cap never exceeds the base demand.
    CHECK(FEQ(pb_heater_foldback_duty(0.0f, 25.0f), 0.0f));
    CHECK(FEQ(pb_heater_foldback_duty(0.0f, mid), 0.0f));
    // A base demand already below the foldback cap is left as-is (min, not overwrite):
    // at the band midpoint the cap is 0.5, so a base of 0.3 stays 0.3.
    CHECK(FEQ(pb_heater_foldback_duty(0.3f, mid), 0.3f));
    puts("pb_heater element-foldback checks: PASS");
    return 0;
}
