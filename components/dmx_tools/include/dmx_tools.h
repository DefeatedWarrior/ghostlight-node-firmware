/**
 * @file dmx_tools.h
 * @brief Output test patterns and the factory loopback self-test
 *
 * Test patterns replace the whole output of a port (network data, scenes
 * and the local layer are ignored) until they are stopped. The application's
 * DMX frame source calls dmx_test_fill() first for every frame.
 *
 * The self-test drives known patterns on one port and checks that they come
 * back unchanged on another, connected with a DMX cable (port 1 out ->
 * port 2 in). It temporarily changes both port modes and calls the restore
 * callback afterwards so the application re-applies the saved settings.
 */
#ifndef DMX_TOOLS_H
#define DMX_TOOLS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DMX_TEST_OFF = 0,
    DMX_TEST_STATIC,     /**< every channel of the range at @c level */
    DMX_TEST_FULL,       /**< every channel of the range at 255 */
    DMX_TEST_CHASE,      /**< one channel at a time at @c level, stepping every @c speed_ms */
    DMX_TEST_RAMP,       /**< the range fades 0 -> 255 -> 0 over 2 x @c speed_ms */
} dmx_test_mode_t;
#define DMX_TEST_LAST DMX_TEST_RAMP

typedef struct {
    dmx_test_mode_t mode;
    uint16_t first;      /**< 1..512 */
    uint16_t last;       /**< first..512 */
    uint8_t level;       /**< static / chase level */
    uint16_t speed_ms;   /**< chase step or half ramp period, 20..60000 */
} dmx_test_t;

/** Start (or change) a test on @p port; mode OFF stops it */
esp_err_t dmx_test_start(uint8_t port, const dmx_test_t *test);
esp_err_t dmx_test_stop(uint8_t port);
/** true and *test filled while a test runs on @p port */
bool dmx_test_get(uint8_t port, dmx_test_t *test);
/** Bit mask of ports running a test or the self-test (bit 0 = port 1) */
uint8_t dmx_test_active_mask(void);

/**
 * Frame source hook: if a test (or the self-test) drives @p port, write the
 * next frame into @p data (512 slots) and return true.
 */
bool dmx_test_fill(uint8_t port, uint8_t data[512]);

const char *dmx_test_mode_name(dmx_test_mode_t mode);
/** Parse "static" / "full" / "chase" / "ramp" / "off"; -1 if unknown */
int dmx_test_mode_from_name(const char *name);

// ---------------------------------------------------------------------------
// Loopback self-test
// ---------------------------------------------------------------------------

#define DMX_SELFTEST_PATTERNS 5

typedef enum {
    DMX_SELFTEST_IDLE = 0,
    DMX_SELFTEST_RUNNING,
    DMX_SELFTEST_PASSED,
    DMX_SELFTEST_FAILED,
} dmx_selftest_state_t;

typedef struct {
    const char *name;
    bool passed;
    uint16_t slots;          /**< slots received in the checked frame */
    uint16_t mismatches;     /**< slots that differed */
    uint16_t first_bad;      /**< first differing channel (1-based, 0 = none) */
    uint8_t sent, got;       /**< values at first_bad */
} dmx_selftest_step_t;

typedef struct {
    dmx_selftest_state_t state;
    uint8_t out_port, in_port;
    uint8_t steps_done;
    dmx_selftest_step_t steps[DMX_SELFTEST_PATTERNS];
    uint16_t refresh_hz;     /**< measured on the input */
    uint32_t break_us;       /**< measured on the input (0 = not measured) */
    uint32_t mab_us;
    uint32_t errors;         /**< framing / overflow errors seen on the input */
    char message[96];
} dmx_selftest_result_t;

/** Called when the self-test ends, to re-apply the saved port settings */
typedef void (*dmx_selftest_restore_cb_t)(void *user_data);
void dmx_selftest_set_restore_callback(dmx_selftest_restore_cb_t cb, void *user_data);

/** Start the self-test in the background (about 6 s) */
esp_err_t dmx_selftest_start(uint8_t out_port, uint8_t in_port);
void dmx_selftest_get(dmx_selftest_result_t *out);
bool dmx_selftest_running(void);

#ifdef __cplusplus
}
#endif

#endif // DMX_TOOLS_H
