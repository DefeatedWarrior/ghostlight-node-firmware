/* libFuzzer target: config_from_json() (the web API) and config_load() (the
 * config file) on the same input, then checks that
 *  - the resulting configuration is already sanitised (config_sanitize()
 *    finds nothing to fix), and
 *  - config_to_json() -> config_from_json() gives back the same
 *    configuration and the same JSON. */
#include "config_manager.h"
#include "fakes.h"
#include <stdlib.h>
#include <string.h>

static void check_settled(void)
{
    config_t *g = config_get();
    config_t copy = *g;
    if (config_sanitize(&copy)) __builtin_trap();

    char *a = NULL, *b = NULL;
    if (config_to_json(&a) != ESP_OK) __builtin_trap();
    config_t before = *g;
    if (config_from_json(a) != ESP_OK) __builtin_trap();
    if (memcmp(&before, g, sizeof(before)) != 0) __builtin_trap();
    if (config_to_json(&b) != ESP_OK) __builtin_trap();
    if (strcmp(a, b) != 0) __builtin_trap();
    free(a);
    free(b);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 16384) return 0;
    char *s = malloc(size + 1);
    memcpy(s, data, size);
    s[size] = '\0';

    host_fakes_reset();
    config_init();
    config_from_json(s);
    check_settled();

    host_fakes_reset();
    config_init();
    host_storage_put("config.json", s, size);
    config_load();
    check_settled();

    free(s);
    return 0;
}
