// Host unit tests for the pure chamber PID helper.
#include "pb_pid.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

int main(void)
{
    pb_pid_state_t s = {0};

    // First sample below target should request a finite partial duty with the
    // conservative production gains used by pb_heater.
    float u = pb_pid_step(&s, 60.0f, 52.0f, 0.5f, 0.04f, 0.0008f, 0.02f, 0.20f);
    CHECK(isfinite(u));
    CHECK(u > 0.25f && u < 0.40f);

    // Integral should build slowly rather than immediately saturating.
    for (int i = 0; i < 120; ++i)
        u = pb_pid_step(&s, 60.0f, 52.0f, 0.5f, 0.04f, 0.0008f, 0.02f, 0.20f);
    CHECK(u > 0.30f && u < 0.75f);

    // At/above target a heater-only controller must command zero immediately.
    u = pb_pid_step(&s, 60.0f, 60.0f, 0.5f, 0.04f, 0.0008f, 0.02f, 0.20f);
    CHECK(u == 0.0f);
    u = pb_pid_step(&s, 60.0f, 61.0f, 0.5f, 0.04f, 0.0008f, 0.02f, 0.20f);
    CHECK(u == 0.0f);

    // Saturation anti-windup: a huge positive error may request full output, but
    // the integral must not grow without bound while pinned at 100%.
    pb_pid_reset(&s);
    for (int i = 0; i < 1000; ++i)
        u = pb_pid_step(&s, 70.0f, 0.0f, 0.5f, 0.04f, 0.0008f, 0.02f, 0.20f);
    CHECK(u == 1.0f);
    CHECK(s.integral < 1.0f);

    // Invalid input fails cold and resets state.
    u = pb_pid_step(&s, 60.0f, NAN, 0.5f, 0.04f, 0.0008f, 0.02f, 0.20f);
    CHECK(u == 0.0f);
    CHECK(!s.initialized);

    puts("pb_pid host checks: PASS");
    return 0;
}
