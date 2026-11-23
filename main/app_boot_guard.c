/*
 * Boot-loop guard. A node that crashes soon after every start (a setting, a
 * fixture on the line or a console's data setting off a bug) would restart
 * forever and the web UI would never be reachable long enough to fix it.
 * After BOOT_CRASHES_SAFE such crashes in a row it starts in safe mode: the
 * network and the web UI as usual, the DMX ports off until someone presses
 * "Start the DMX ports" (or it restarts on its own, without a crash).
 *
 * The count lives in RTC memory, which a crash or watchdog reset keeps and
 * power-on clears. New firmware that crashes is rolled back by the
 * bootloader before this matters (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 */
#include <stdio.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "diag.h"
#include "app.h"

static const char *TAG = "main";

#define BOOT_CRASHES_SAFE   3                        /* crashes in a row, each early */
#define BOOT_STABLE_US      (60LL * 1000 * 1000)     /* up this long: not a boot loop */
#define BOOT_GUARD_MAGIC    0x47484C42u              /* "GHLB" */

/* Kept across crash resets. check = ~crashes, so power-on garbage is seen */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t starting;    /* 1 from start-up until BOOT_STABLE_US */
    uint32_t crashes;     /* early crashes in a row */
    uint32_t check;
} s_guard;

static char s_notice[256];
static bool s_stable;

void app_boot_guard_begin(void)
{
    bool valid = s_guard.magic == BOOT_GUARD_MAGIC && s_guard.check == ~s_guard.crashes;
    if (valid && s_guard.starting && diag_reset_was_crash()) {
        s_guard.crashes++;
    } else {
        s_guard.crashes = 0;
    }
    s_guard.magic = BOOT_GUARD_MAGIC;
    s_guard.starting = 1;
    s_guard.check = ~s_guard.crashes;

    if (s_guard.crashes >= BOOT_CRASHES_SAFE) {
        g_app.safe_mode = true;
        snprintf(s_notice, sizeof(s_notice),
                 "The node restarted after a crash %u times in a row, each within a minute of "
                 "starting (last: %s), so it started with the DMX ports off. The network and "
                 "the settings work as usual. The crash report is under System.",
                 (unsigned)s_guard.crashes, diag_reset_reason());
        ESP_LOGE(TAG, "SAFE MODE: %u early crashes in a row, DMX ports stay off",
                 (unsigned)s_guard.crashes);
    } else if (s_guard.crashes) {
        ESP_LOGW(TAG, "Crash %u of %u in a row soon after start-up (safe mode after %u)",
                 (unsigned)s_guard.crashes, (unsigned)BOOT_CRASHES_SAFE,
                 (unsigned)BOOT_CRASHES_SAFE);
    }
}

void app_boot_guard_tick(void)
{
    if (s_stable || esp_timer_get_time() < BOOT_STABLE_US) return;
    s_stable = true;
    s_guard.starting = 0;    /* a crash from now on is not a boot loop */
    s_guard.crashes = 0;
    s_guard.check = ~s_guard.crashes;
}

const char *app_safe_mode_notice(void)
{
    return g_app.safe_mode ? s_notice : NULL;
}

void app_leave_safe_mode(void)
{
    if (!g_app.safe_mode) return;
    ESP_LOGW(TAG, "Leaving safe mode: starting the DMX ports");
    xSemaphoreTake(g_app.apply_lock, portMAX_DELAY);
    g_app.safe_mode = false;
    g_app.applied_valid = false;   /* every port starts as if new */
    xSemaphoreGive(g_app.apply_lock);
    artnet_receiver_set_node_report(ARTNET_RC_POWER_OK, "DMX ports started");
    xEventGroupSetBits(g_app.events, EV_APPLY);
}
