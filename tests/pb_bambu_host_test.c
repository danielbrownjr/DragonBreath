// Host unit test for the Bambu report parser (pb_bambu_parse.h): active-filament
// tri-state resolution and filament->zone matching. Pure logic, no ESP deps.
#include "pb_bambu_parse.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void expect_fila(const char *name, const char *json,
                        pb_fila_result_t want_r, const char *want_s)
{
    char got[16];
    pb_fila_result_t r = pb_bambu_active_filament(json, got, sizeof got);
    int ok = (r == want_r) && (want_r != PB_FILA_PRESENT || strcmp(got, want_s) == 0);
    if (!ok) fails++;
    static const char *R[] = { "ABSENT", "EMPTY", "PRESENT" };
    printf("[%s] %-22s want=%s/%-6s got=%s/%s\n", ok ? "PASS" : "FAIL", name,
           R[want_r], want_r == PB_FILA_PRESENT ? want_s : "-",
           R[r], r == PB_FILA_PRESENT ? got : "-");
}

static void expect_zone(const char *filament, int want_idx)
{
    static const char *const names[] = { "PLA", "PETG", "ABS", "ASA", "PC", "TPU" };
    int got = pb_bambu_zone_match(filament, names, 6);
    int ok = got == want_idx;
    if (!ok) fails++;
    printf("[%s] zone %-12s want=%d got=%d\n", ok ? "PASS" : "FAIL", filament, want_idx, got);
}

int main(void)
{
    // --- active-filament tri-state ---
    // PRESENT: AMS active tray carries a type.
    expect_fila("ams tray0 PETG",
        "{\"print\":{\"ams\":{\"ams\":[{\"tray\":[{\"tray_type\":\"PETG\"},{\"tray_type\":\"PLA\"}]}],\"tray_now\":\"0\"},\"vt_tray\":{\"tray_type\":\"\"}}}",
        PB_FILA_PRESENT, "PETG");
    expect_fila("ams tray1 PLA",
        "{\"print\":{\"ams\":{\"ams\":[{\"tray\":[{\"tray_type\":\"PETG\"},{\"tray_type\":\"PLA\"}]}],\"tray_now\":\"1\"}}}",
        PB_FILA_PRESENT, "PLA");
    // PRESENT: external spool.
    expect_fila("ext spool ABS",
        "{\"print\":{\"ams\":{\"tray_now\":\"254\"},\"vt_tray\":{\"id\":\"254\",\"tray_type\":\"ABS\"}}}",
        PB_FILA_PRESENT, "ABS");
    // EMPTY: explicit no-spool (tray_now 255) MUST clear a prior value.
    expect_fila("none tray_now=255",
        "{\"print\":{\"ams\":{\"tray_now\":\"255\"},\"vt_tray\":{\"tray_type\":\"\"}}}",
        PB_FILA_EMPTY, NULL);
    // EMPTY: active AMS slot with an empty type (unloaded) -> clear.
    expect_fila("ams tray0 empty",
        "{\"print\":{\"ams\":{\"ams\":[{\"tray\":[{\"tray_type\":\"\"}]}],\"tray_now\":\"0\"}}}",
        PB_FILA_EMPTY, NULL);
    // ABSENT: a delta report that omits filament state -> keep prior (no clobber).
    expect_fila("delta no filament",
        "{\"print\":{\"bed_temper\":60.0,\"chamber_temper\":40.0}}",
        PB_FILA_ABSENT, NULL);
    // ABSENT: partial-AMS delta — slot is named (tray_now) but the tray payload is
    // absent. Must NOT clear a known filament (review round 2).
    expect_fila("partial-ams delta",
        "{\"print\":{\"ams\":{\"tray_now\":\"0\"}}}",
        PB_FILA_ABSENT, NULL);
    // ABSENT: external selected (254) but no vt_tray payload in this delta -> keep.
    expect_fila("ext selected, no vt",
        "{\"print\":{\"ams\":{\"tray_now\":\"254\"}}}",
        PB_FILA_ABSENT, NULL);
    // EMPTY: external spool present but explicitly empty -> clear.
    expect_fila("ext spool empty",
        "{\"print\":{\"vt_tray\":{\"id\":\"254\",\"tray_type\":\"\"}}}",
        PB_FILA_EMPTY, NULL);
    // The reviewer's PETG -> no-spool transition: an EMPTY report tells parse_report
    // to clear the stored PETG (see pb_bambu.c); ABSENT deltas leave it untouched.

    // --- filament -> zone matching (case-insensitive prefix) ---
    expect_zone("PETG", 1);
    expect_zone("PETG-CF", 1);
    expect_zone("PLA Basic", 0);
    expect_zone("ASA-CF", 3);
    expect_zone("petg", 1);      // case-insensitive
    expect_zone("PVA", -1);      // unknown -> no zone
    expect_zone("", -1);

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
