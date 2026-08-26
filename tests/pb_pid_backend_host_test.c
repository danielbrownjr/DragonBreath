// SPDX-License-Identifier: MIT
#include "pb_pid_backend.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    pb_pid_backend_state_t state = {0};
    float output = pb_pid_backend_step(&state, 60.0f, 52.0f, 0.5f,
                                       0.04f, 0.0008f, 0.02f, 0.20f);
    assert(isfinite(output));
    assert(output > 0.0f && output <= 1.0f);

    for (int i = 0; i < 1000; ++i)
        output = pb_pid_backend_step(&state, 70.0f, 0.0f, 0.5f,
                                     0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 1.0f);

    // Heater policy lives in the adapter, independent of the math backend.
    output = pb_pid_backend_step(&state, 60.0f, 60.0f, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 0.0f);
    output = pb_pid_backend_step(&state, 60.0f, 61.0f, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 0.0f);

    output = pb_pid_backend_step(&state, 60.0f, NAN, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 0.0f);

    pb_pid_backend_reset(&state);
    puts("pb_pid backend host checks: PASS");
    return 0;
}
