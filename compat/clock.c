/* OWL's core reads time through clock_time_us(). CMake renames state.c's
 * monotonic version so this one can be pointed at a simulated clock in tests. */
#include <stdint.h>

uint64_t owl_clock_monotonic_us(void);

static uint64_t (*clock_fn)(void) = owl_clock_monotonic_us;

void awdl_clock_set(uint64_t (*now_us)(void)) {
	clock_fn = now_us ? now_us : owl_clock_monotonic_us;
}

uint64_t clock_time_us(void) {
	return clock_fn();
}
