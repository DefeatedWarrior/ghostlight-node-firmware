/**
 * @file scene_manager.c
 * @brief DMX Scene Manager — stores/replays up to 16 DMX snapshots in LittleFS.
 *
 * Scenes are persisted as JSON files at /littlefs/scenes/scene_XXXX.json.
 * Scene IDs are 1-16. The manager scans for the first unused slot when
 * recording a new scene.
 */

#include "scene_manager.h"
#include "merge_engine.h"
#include "storage_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

static const char *TAG = "scene_mgr";

#define SCENES_DIR      STORAGE_BASE_PATH "/scenes"
#define SCENE_PORTS     CONFIG_DMX_PORTS
/* Maximum path length for a scene file */
#define SCENE_PATH_LEN  64

// ============================================================================
// Internal helpers
// ============================================================================

/** Build the file path for a given scene ID into @p buf (SCENE_PATH_LEN bytes) */
static void scene_path(uint32_t id, char *buf)
{
    snprintf(buf, SCENE_PATH_LEN, SCENES_DIR "/scene_%04" PRIu32 ".json", id);
}

/** Check whether a scene file exists for a given ID */
static bool scene_exists(uint32_t id)
{
    char path[SCENE_PATH_LEN];
    scene_path(id, path);
    struct stat st;
    return (stat(path, &st) == 0);
}

/** Parse a scene JSON object into a dmx_scene_t struct */
static esp_err_t parse_scene_json(cJSON *root, dmx_scene_t *out)
{
    if (!root || !out) return ESP_ERR_INVALID_ARG;

    cJSON *jid   = cJSON_GetObjectItem(root, "id");
    cJSON *jname = cJSON_GetObjectItem(root, "name");
    cJSON *jport = cJSON_GetObjectItem(root, "port");
    cJSON *jfade = cJSON_GetObjectItem(root, "fade_ms");
    cJSON *jts   = cJSON_GetObjectItem(root, "timestamp");
    cJSON *jdata = cJSON_GetObjectItem(root, "dmx_data");

    if (!cJSON_IsNumber(jid) || !cJSON_IsString(jname) ||
        !cJSON_IsNumber(jport) || !cJSON_IsNumber(jfade) ||
        !cJSON_IsArray(jdata)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (jport->valueint < 1 || jport->valueint > SCENE_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }
    out->scene_id    = (uint32_t)jid->valueint;
    out->port        = (uint8_t)jport->valueint;
    out->fade_time_ms = (uint16_t)jfade->valueint;
    out->timestamp   = cJSON_IsNumber(jts) ? (uint32_t)jts->valueint : 0;
    size_t nlen = strnlen(jname->valuestring, SCENE_NAME_MAX_LEN - 1);
    memcpy(out->name, jname->valuestring, nlen);
    out->name[nlen] = '\0';

    memset(out->dmx_data, 0, sizeof(out->dmx_data));
    int i = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, jdata) {
        if (i >= 512) break;
        int v = cJSON_IsNumber(item) ? item->valueint : 0;
        out->dmx_data[i++] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    return ESP_OK;
}

/** Write a dmx_scene_t to its JSON file */
static esp_err_t write_scene_file(const dmx_scene_t *scene)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(root, "id",        (double)scene->scene_id);
    cJSON_AddStringToObject(root, "name",      scene->name);
    cJSON_AddNumberToObject(root, "port",      scene->port);
    cJSON_AddNumberToObject(root, "fade_ms",   scene->fade_time_ms);
    cJSON_AddNumberToObject(root, "timestamp", (double)scene->timestamp);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 512; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(scene->dmx_data[i]));
    }
    cJSON_AddItemToObject(root, "dmx_data", arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    char rel[SCENE_PATH_LEN];
    snprintf(rel, sizeof(rel), "scenes/scene_%04" PRIu32 ".json", scene->scene_id);
    esp_err_t err = storage_write_file(rel, json_str, strlen(json_str));
    free(json_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot write scene %" PRIu32, scene->scene_id);
        return err;
    }
    ESP_LOGI(TAG, "Scene %" PRIu32 " saved", scene->scene_id);
    return ESP_OK;
}

/** Read and parse a scene file by ID */
static esp_err_t read_scene_file(uint32_t id, dmx_scene_t *out)
{
    char path[SCENE_PATH_LEN];
    scene_path(id, path);

    storage_lock();   /* a save can't rename over a file that is open */
    FILE *f = fopen(path, "r");
    if (!f) {
        storage_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    /* Determine file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 16384) {
        fclose(f);
        storage_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); storage_unlock(); return ESP_ERR_NO_MEM; }

    size_t got = fread(buf, 1, (size_t)fsize, f);
    buf[got] = '\0';
    fclose(f);
    storage_unlock();

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_ERR_INVALID_ARG;

    esp_err_t err = parse_scene_json(root, out);
    cJSON_Delete(root);
    return err;
}

// ============================================================================
// Fade task
// ============================================================================

#define FADE_STEP_MS 25

typedef struct {
    uint8_t target[512];
    uint8_t port;
    uint16_t fade_ms;
    uint32_t gen;
    bool release;       /* clear the local layer when done */
} fade_args_t;

/* Per-port generation: a newer playback/release cancels a running fade */
static volatile uint32_t s_gen[SCENE_PORTS];
static volatile uint32_t s_active_scene[SCENE_PORTS];

static void fade_task(void *arg)
{
    fade_args_t *fa = (fade_args_t *)arg;
    uint8_t start[512];
    uint8_t frame[512];
    bool active = false;

    if (merge_engine_get_local(fa->port, start, &active) != ESP_OK || !active) {
        memset(start, 0, sizeof(start));
    }

    int steps = fa->fade_ms / FADE_STEP_MS;
    if (steps < 1) steps = 1;
    TickType_t last = xTaskGetTickCount();

    for (int step = 1; step <= steps; step++) {
        if (s_gen[fa->port - 1] != fa->gen) {
            goto out;   /* superseded */
        }
        for (int i = 0; i < 512; i++) {
            int delta = (int)fa->target[i] - (int)start[i];
            frame[i] = (uint8_t)(start[i] + (delta * step) / steps);
        }
        merge_engine_set_local(fa->port, frame);
        if (step < steps) {
            vTaskDelayUntil(&last, pdMS_TO_TICKS(FADE_STEP_MS) ? pdMS_TO_TICKS(FADE_STEP_MS) : 1);
        }
    }
    if (s_gen[fa->port - 1] == fa->gen && fa->release) {
        merge_engine_clear_local(fa->port);
    }

out:
    free(fa);
    vTaskDelete(NULL);
}

static esp_err_t start_fade(uint8_t port, const uint8_t *target, uint16_t fade_ms, bool release)
{
    uint32_t gen = ++s_gen[port - 1];

    if (fade_ms == 0) {
        if (release) {
            merge_engine_clear_local(port);
        } else {
            merge_engine_set_local(port, target);
        }
        return ESP_OK;
    }

    fade_args_t *fa = malloc(sizeof(fade_args_t));
    if (!fa) return ESP_ERR_NO_MEM;
    if (target) {
        memcpy(fa->target, target, 512);
    } else {
        memset(fa->target, 0, 512);
    }
    fa->port = port;
    fa->fade_ms = fade_ms;
    fa->gen = gen;
    fa->release = release;

    if (xTaskCreate(fade_task, "scene_fade", 3072, fa, 5, NULL) != pdPASS) {
        free(fa);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t scene_manager_init(void)
{
    /* Create the scenes directory if it does not exist */
    struct stat st;
    if (stat(SCENES_DIR, &st) != 0) {
        if (mkdir(SCENES_DIR, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create %s: %d", SCENES_DIR, errno);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Scene manager initialized (dir: %s)", SCENES_DIR);
    return ESP_OK;
}

esp_err_t scene_record(uint8_t port, const char *name, uint32_t *scene_id_out)
{
    if (port < 1 || port > SCENE_PORTS || !name || !scene_id_out) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find first free slot */
    uint32_t new_id = 0;
    for (uint32_t id = 1; id <= SCENE_MAX_COUNT; id++) {
        if (!scene_exists(id)) {
            new_id = id;
            break;
        }
    }
    if (new_id == 0) {
        ESP_LOGW(TAG, "All %d scene slots are used", SCENE_MAX_COUNT);
        return ESP_ERR_NO_MEM;
    }

    dmx_scene_t scene;
    memset(&scene, 0, sizeof(scene));
    scene.scene_id = new_id;
    scene.port     = port;
    size_t nlen = strnlen(name, SCENE_NAME_MAX_LEN - 1);
    memcpy(scene.name, name, nlen);
    scene.name[nlen] = '\0';

    /* Capture what the port is outputting right now */
    merge_engine_get_output(port, scene.dmx_data);

    /* Timestamp */
    scene.timestamp = (uint32_t)time(NULL);

    esp_err_t err = write_scene_file(&scene);
    if (err == ESP_OK) {
        *scene_id_out = new_id;
        ESP_LOGI(TAG, "Recorded scene %" PRIu32 " \"%s\" port %d", new_id, name, port);
    }
    return err;
}

esp_err_t scene_playback(uint32_t scene_id, uint16_t fade_ms)
{
    return scene_playback_on(scene_id, 0, fade_ms);
}

esp_err_t scene_playback_on(uint32_t scene_id, uint8_t port, uint16_t fade_ms)
{
    if (scene_id < 1 || scene_id > SCENE_MAX_COUNT || port > SCENE_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }

    dmx_scene_t scene;
    esp_err_t err = read_scene_file(scene_id, &scene);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scene_playback: scene %" PRIu32 " not found", scene_id);
        return err;
    }

    if (port == 0) port = scene.port;
    ESP_LOGI(TAG, "Playback scene %" PRIu32 " \"%s\" on port %d fade_ms=%d",
             scene_id, scene.name, port, fade_ms);

    err = start_fade(port, scene.dmx_data, fade_ms, false);
    if (err == ESP_OK) {
        s_active_scene[port - 1] = scene_id;
    }
    return err;
}

esp_err_t scene_release(uint8_t port, uint16_t fade_ms)
{
    if (port > SCENE_PORTS) return ESP_ERR_INVALID_ARG;
    for (uint8_t p = 1; p <= SCENE_PORTS; p++) {
        if (port != 0 && p != port) continue;
        s_active_scene[p - 1] = 0;
        esp_err_t err = start_fade(p, NULL, fade_ms, true);
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(TAG, "Scene released on port %u (fade %u ms)", port, fade_ms);
    return ESP_OK;
}

uint32_t scene_active(uint8_t port)
{
    if (port < 1 || port > SCENE_PORTS) return 0;
    return s_active_scene[port - 1];
}

esp_err_t scene_delete(uint32_t scene_id)
{
    if (scene_id < 1 || scene_id > SCENE_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[SCENE_PATH_LEN];
    scene_path(scene_id, path);

    storage_lock();
    int removed = remove(path);
    int why = errno;
    storage_unlock();
    if (removed != 0) {
        if (why == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "Failed to delete %s: %d", path, why);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted scene %" PRIu32, scene_id);
    return ESP_OK;
}

esp_err_t scene_list(dmx_scene_t *scenes, size_t *count)
{
    if (!scenes || !count) return ESP_ERR_INVALID_ARG;

    size_t found = 0;
    size_t max   = *count;

    for (uint32_t id = 1; id <= SCENE_MAX_COUNT && found < max; id++) {
        if (scene_exists(id)) {
            esp_err_t err = read_scene_file(id, &scenes[found]);
            if (err == ESP_OK) {
                found++;
            } else {
                ESP_LOGW(TAG, "Failed to read scene %" PRIu32 ": %d", id, err);
            }
        }
    }

    *count = found;
    return ESP_OK;
}

esp_err_t scene_get(uint32_t scene_id, dmx_scene_t *scene_out)
{
    if (scene_id < 1 || scene_id > SCENE_MAX_COUNT || !scene_out) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_scene_file(scene_id, scene_out);
}
