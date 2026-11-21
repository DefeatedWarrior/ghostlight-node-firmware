#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define SCENE_MAX_COUNT 16
#define SCENE_NAME_MAX_LEN 32

typedef struct {
    uint32_t scene_id;          // 1-16
    char name[SCENE_NAME_MAX_LEN];
    uint8_t port;               // port it was recorded on (1..CONFIG_DMX_PORTS)
    uint16_t fade_time_ms;
    uint8_t dmx_data[512];
    uint32_t timestamp;         // unix timestamp or 0
} dmx_scene_t;

esp_err_t scene_manager_init(void);
esp_err_t scene_record(uint8_t port, const char *name, uint32_t *scene_id_out);
/**
 * Play a scene on its port. The scene is held on the merge engine's local
 * layer (HTP on top of network data) until released or replaced, fading from
 * whatever the local layer showed before.
 */
esp_err_t scene_playback(uint32_t scene_id, uint16_t fade_ms);

/** Play a scene's levels on @p port (0 = the port it was recorded on) */
esp_err_t scene_playback_on(uint32_t scene_id, uint8_t port, uint16_t fade_ms);

/** Fade the scene on @p port (0 = every port) out and release the local layer */
esp_err_t scene_release(uint8_t port, uint16_t fade_ms);

/** Scene currently held on @p port (0 if none) */
uint32_t scene_active(uint8_t port);
esp_err_t scene_delete(uint32_t scene_id);
esp_err_t scene_list(dmx_scene_t *scenes, size_t *count);
esp_err_t scene_get(uint32_t scene_id, dmx_scene_t *scene_out);
#endif
