#pragma once
#include "Bridge.h"

#if defined(SHIP_FURY_RUNTIME_TEST_HOOKS)
#ifdef __cplusplus
extern "C" {
#endif

/* Private test seam, absent from non-testing builds. All calls except gate_release
 * and slot_waiting are confined to the context thread. One release thread may
 * call gate_release and slot_waiting;
 * it must be joined before context destruction or another gate_begin. */
typedef struct {
    uint64_t submitted, completed;
    uint64_t retired_textures, retired_pipelines;
    uint32_t pending_slots, slot_count;
} ShipFuryTestStats;

SHIP_FURY_EXPORT ShipFury* ship_fury_test_create(ShipFuryWindow window, uint32_t validation);
/* Flushes and waits for setup, then gates subsequent graphics flush submissions. */
SHIP_FURY_EXPORT int ship_fury_test_gate_begin(ShipFury* context);
/* Empty GPU signal submit on an independent compute queue; no CPU fence signal,
 * shared command recorder, graphics-queue access, or context error mutation. */
SHIP_FURY_EXPORT int ship_fury_test_gate_release(ShipFury* context);
/* Atomic observation only; safe on the release thread. */
SHIP_FURY_EXPORT int ship_fury_test_slot_waiting(ShipFury* context);
SHIP_FURY_EXPORT int ship_fury_test_stats(ShipFury* context, ShipFuryTestStats* stats);
/* Fault injection and observations are context-thread only. */
SHIP_FURY_EXPORT ShipFury* ship_fury_test_incomplete_context(void);
SHIP_FURY_EXPORT int ship_fury_test_fail_next_submit(ShipFury* context);
SHIP_FURY_EXPORT int ship_fury_test_surface_registered(ShipFury* context, uint32_t surface);

#ifdef __cplusplus
}
#endif
#endif
