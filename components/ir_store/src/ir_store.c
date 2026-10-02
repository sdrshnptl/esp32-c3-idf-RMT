/*
 * ir_store — persistence for the IR remote: NVS for metadata, SPIFFS for raw frames.
 *
 * Layout
 *   NVS namespace "irstore"
 *     ver       u8    schema version; a mismatch wipes the store (protects layout drift)
 *     nextprof  u32   next profile id
 *     nextcmd   u32   next command id
 *     pmeta     blob  { count, ir_profile_meta_t[count] }
 *     but<id>   blob  { count, ir_button_meta_t[count] } per profile
 *     hk        blob  { profile_id, step_count, step[step_count] }
 *   SPIFFS  <mount>/c%08x.bin   command_header_t followed by edge_count uint16 durations
 */

#include "ir_store.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "ir_store";

#define NVS_NAMESPACE  "irstore"
#define KEY_VERSION    "ver"
#define KEY_NEXTPROF   "nextprof"
#define KEY_NEXTCMD    "nextcmd"
#define KEY_PROFILES   "pmeta"
#define KEY_HOTKEY     "hk"
#define BUTTON_KEY_FMT "but%u"

#define SCHEMA_VERSION 1u

#define BLOB_MAGIC   0x31425249u /* "IRB1" */
#define BUNDLE_MAGIC 0x58455249u /* "IREX" */

#define MAX_PROFILES CONFIG_IR_STORE_MAX_PROFILES
#define MAX_BUTTONS  CONFIG_IR_STORE_MAX_BUTTONS
#define MAX_COMMANDS (MAX_PROFILES * MAX_BUTTONS)

/** @brief On-flash header for one stored frame. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t start_level;
    uint32_t edge_count;
    uint32_t carrier_hz;
    uint16_t repeat_gap_ms;
    uint8_t duty_percent;
    uint8_t repeats;
    uint16_t reserved;
} command_header_t;

typedef struct {
    uint8_t count;
    ir_profile_meta_t items[MAX_PROFILES];
} profile_table_t;

typedef struct {
    uint8_t count;
    ir_button_meta_t items[MAX_BUTTONS];
} button_table_t;

typedef struct __attribute__((packed)) {
    uint8_t profile_id;
    uint8_t step_count;
    ir_hotkey_step_t steps[IR_STORE_MAX_HOTKEY_STEPS];
} hotkey_record_t;

static nvs_handle_t s_nvs;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_initialized;

/* Scratch buffers. All access is serialised by s_lock, so one set is enough - but they must
 * never be used for two overlapping tables at once. */
static profile_table_t s_prof_tbl;
static button_table_t s_btn_tbl;
static hotkey_record_t s_hk;
static uint32_t s_cmd_ids[MAX_COMMANDS];
static uint8_t s_io_buf[512];

/* ---- small helpers ------------------------------------------------------- */

static esp_err_t lock_take(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(xSemaphoreTakeRecursive(s_lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "lock timeout");
    return ESP_OK;
}

static void lock_give(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void copy_name(char *dst, const char *src)
{
    strncpy(dst, src, IR_STORE_NAME_LEN - 1);
    dst[IR_STORE_NAME_LEN - 1] = '\0';
}

static void command_path(ir_command_id_t id, char *out, size_t len)
{
    /* The mount point is bounded to 32 characters so the compiler can prove the result fits
     * the 48-byte buffers used by the callers; mount points are short by nature ("/spiffs"). */
    snprintf(out, len, "%.32s/c%08" PRIx32 ".bin", CONFIG_IR_STORE_MOUNT_POINT, id);
}

/* ---- NVS tables ---------------------------------------------------------- */

static esp_err_t load_profile_table(void)
{
    size_t size = sizeof(s_prof_tbl);
    esp_err_t err = nvs_get_blob(s_nvs, KEY_PROFILES, &s_prof_tbl, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_prof_tbl, 0, sizeof(s_prof_tbl));
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read profile table failed");
    ESP_RETURN_ON_FALSE(size == sizeof(s_prof_tbl), ESP_ERR_INVALID_SIZE, TAG,
                        "profile table size mismatch (%u)", (unsigned)size);
    return ESP_OK;
}

static esp_err_t save_profile_table(void)
{
    return nvs_set_blob(s_nvs, KEY_PROFILES, &s_prof_tbl, sizeof(s_prof_tbl));
}

static esp_err_t load_button_table(uint8_t profile_id)
{
    char key[16];
    snprintf(key, sizeof(key), BUTTON_KEY_FMT, (unsigned)profile_id);

    size_t size = sizeof(s_btn_tbl);
    esp_err_t err = nvs_get_blob(s_nvs, key, &s_btn_tbl, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_btn_tbl, 0, sizeof(s_btn_tbl));
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read button table %s failed", key);
    ESP_RETURN_ON_FALSE(size == sizeof(s_btn_tbl), ESP_ERR_INVALID_SIZE, TAG,
                        "button table %s size mismatch", key);
    return ESP_OK;
}

static esp_err_t save_button_table(uint8_t profile_id)
{
    char key[16];
    snprintf(key, sizeof(key), BUTTON_KEY_FMT, (unsigned)profile_id);
    return nvs_set_blob(s_nvs, key, &s_btn_tbl, sizeof(s_btn_tbl));
}

static esp_err_t find_profile(uint8_t profile_id, size_t *out_index)
{
    for (size_t i = 0; i < s_prof_tbl.count && i < MAX_PROFILES; i++) {
        if (s_prof_tbl.items[i].id == profile_id) {
            *out_index = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t find_button(uint8_t button_id, size_t *out_index)
{
    for (size_t i = 0; i < s_btn_tbl.count && i < MAX_BUTTONS; i++) {
        if (s_btn_tbl.items[i].id == button_id) {
            *out_index = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ---- command blobs ------------------------------------------------------- */

static esp_err_t command_write(ir_command_id_t id, const ir_frame_t *frame,
                              const ir_tx_params_t *params)
{
    ESP_RETURN_ON_FALSE(frame->edge_count > 0 && frame->edge_count <= IR_FRAME_MAX_EDGES,
                        ESP_ERR_INVALID_ARG, TAG, "bad edge count %u", (unsigned)frame->edge_count);

    const command_header_t hdr = {
        .magic = BLOB_MAGIC,
        .version = SCHEMA_VERSION,
        .start_level = frame->start_level,
        .edge_count = frame->edge_count,
        .carrier_hz = params->carrier_hz,
        .repeat_gap_ms = params->repeat_gap_ms,
        .duty_percent = params->duty_percent,
        .repeats = params->repeats,
    };

    char path[48];
    command_path(id, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f != NULL, ESP_ERR_NOT_FOUND, TAG, "open %s failed", path);

    esp_err_t err = ESP_OK;
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1 ||
        fwrite(frame->durations, sizeof(uint16_t), frame->edge_count, f) != frame->edge_count) {
        ESP_LOGE(TAG, "write %s failed", path);
        err = ESP_FAIL;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "close %s failed", path);
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        remove(path);
    }
    return err;
}

static esp_err_t command_read(ir_command_id_t id, ir_frame_t *out_frame,
                             ir_tx_params_t *out_params)
{
    char path[48];
    command_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f != NULL, ESP_ERR_NOT_FOUND, TAG, "open %s failed", path);

    command_header_t hdr;
    esp_err_t err = ESP_OK;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (hdr.magic != BLOB_MAGIC || hdr.version != SCHEMA_VERSION) {
        err = ESP_ERR_INVALID_VERSION;
    } else if (hdr.edge_count == 0 || hdr.edge_count > IR_FRAME_MAX_EDGES) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (fread(out_frame->durations, sizeof(uint16_t), hdr.edge_count, f) !=
               hdr.edge_count) {
        err = ESP_ERR_INVALID_SIZE;
    }

    if (fclose(f) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read %s failed", path);

    out_frame->start_level = (uint8_t)hdr.start_level;
    out_frame->edge_count = (uint16_t)hdr.edge_count;
    out_params->carrier_hz = hdr.carrier_hz;
    out_params->duty_percent = hdr.duty_percent;
    out_params->repeats = hdr.repeats;
    out_params->repeat_gap_ms = hdr.repeat_gap_ms;
    return ESP_OK;
}

static void command_delete(ir_command_id_t id)
{
    if (id == 0) {
        return;
    }
    char path[48];
    command_path(id, path, sizeof(path));
    if (remove(path) != 0) {
        ESP_LOGW(TAG, "could not remove %s (already gone?)", path);
    }
}

static esp_err_t next_command_id(ir_command_id_t *out_id)
{
    uint32_t next = 0;
    esp_err_t err = nvs_get_u32(s_nvs, KEY_NEXTCMD, &next);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        next = 1;
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "read nextcmd failed");
    }
    ESP_RETURN_ON_FALSE(next != 0, ESP_ERR_INVALID_STATE, TAG, "command id space exhausted");

    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, KEY_NEXTCMD, next + 1), TAG, "write nextcmd failed");
    *out_id = next;
    return ESP_OK;
}

/* ---- init / reset -------------------------------------------------------- */

static esp_err_t wipe_all(void)
{
    /* Remove every command file, then clear the metadata. */
    DIR *dir = opendir(CONFIG_IR_STORE_MOUNT_POINT);
    if (dir != NULL) {
        struct dirent *entry;
        char path[128];
        while ((entry = readdir(dir)) != NULL) {
            /* Only command files are ours; everything else is left alone. Both the mount point
             * and the name are bounded so the compiler can prove the result fits. */
            if (entry->d_name[0] != 'c') {
                continue;
            }
            snprintf(path, sizeof(path), "%.32s/%.48s", CONFIG_IR_STORE_MOUNT_POINT,
                     entry->d_name);
            if (remove(path) != 0) {
                ESP_LOGW(TAG, "could not remove %s", path);
            }
        }
        closedir(dir);
    } else {
        ESP_LOGW(TAG, "could not open %s to enumerate commands", CONFIG_IR_STORE_MOUNT_POINT);
    }

    ESP_RETURN_ON_ERROR(nvs_erase_all(s_nvs), TAG, "erase namespace failed");
    ESP_RETURN_ON_ERROR(nvs_set_u8(s_nvs, KEY_VERSION, SCHEMA_VERSION), TAG, "write version failed");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, KEY_NEXTPROF, 1), TAG, "write nextprof failed");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, KEY_NEXTCMD, 1), TAG, "write nextcmd failed");
    return nvs_commit(s_nvs);
}

static esp_err_t open_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs a fresh partition (%s), erasing", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");
    return ESP_OK;
}

static esp_err_t open_spiffs(void)
{
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_IR_STORE_MOUNT_POINT,
        .partition_label = CONFIG_IR_STORE_PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "mount SPIFFS failed");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info(CONFIG_IR_STORE_PARTITION_LABEL, &total, &used), TAG,
                        "SPIFFS info failed");
    ESP_LOGI(TAG, "SPIFFS on '%s' at %s: %u/%u bytes used",
             CONFIG_IR_STORE_PARTITION_LABEL, CONFIG_IR_STORE_MOUNT_POINT, (unsigned)used,
             (unsigned)total);
    return ESP_OK;
}

esp_err_t ir_store_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock failed");

    ESP_RETURN_ON_ERROR(open_nvs(), TAG, "NVS unavailable");
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs), TAG, "open namespace failed");

    s_initialized = true; /* so lock_take() works below */

    /* A schema change invalidates the blobs, because they mirror the in-memory structs. */
    esp_err_t ret = ESP_OK;
    uint8_t version = 0;
    ret = nvs_get_u8(s_nvs, KEY_VERSION, &version);
    if (ret == ESP_ERR_NVS_NOT_FOUND || version != SCHEMA_VERSION) {
        ESP_LOGW(TAG, "schema %u -> %u, wiping store", (unsigned)version, SCHEMA_VERSION);
        ret = wipe_all();
        if (ret != ESP_OK) {
            s_initialized = false;
            return ret;
        }
    } else if (ret != ESP_OK) {
        s_initialized = false;
        ESP_RETURN_ON_ERROR(ret, TAG, "read version failed");
    }

    ESP_GOTO_ON_ERROR(open_spiffs(), fail, TAG, "SPIFFS unavailable");

    size_t profiles = 0;
    size_t commands = 0;
    size_t used = 0;
    if (ir_store_stats(&profiles, &commands, &used) == ESP_OK) {
        ESP_LOGI(TAG, "ready: %u profile(s), %u command(s), %u bytes in SPIFFS",
                 (unsigned)profiles, (unsigned)commands, (unsigned)used);
    }
    return ESP_OK;

fail:
    nvs_close(s_nvs);
    s_initialized = false;
    return ret;
}

esp_err_t ir_store_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    if (esp_vfs_spiffs_unregister(CONFIG_IR_STORE_PARTITION_LABEL) != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS unregister failed");
    }
    nvs_close(s_nvs);
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_initialized = false;
    return ESP_OK;
}

/* ---- profiles ------------------------------------------------------------ */

esp_err_t ir_store_profile_list(ir_profile_meta_t *out, size_t capacity, size_t *count)
{
    ESP_RETURN_ON_FALSE(out != NULL && count != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");
    esp_err_t err = load_profile_table();
    if (err == ESP_OK) {
        size_t n = (s_prof_tbl.count < capacity) ? s_prof_tbl.count : capacity;
        memcpy(out, s_prof_tbl.items, n * sizeof(ir_profile_meta_t));
        *count = n;
    }
    lock_give();
    return err;
}

esp_err_t ir_store_profile_get(uint8_t profile_id, ir_profile_meta_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = load_profile_table();
    size_t index = 0;
    if (err == ESP_OK) {
        err = find_profile(profile_id, &index);
        if (err == ESP_OK) {
            *out = s_prof_tbl.items[index];
        }
    }
    lock_give();
    return err;
}

esp_err_t ir_store_profile_create(const char *name, uint8_t *out_id)
{
    ESP_RETURN_ON_FALSE(name != NULL && out_id != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t ret = load_profile_table();
    uint8_t id = 0;
    if (ret == ESP_OK) {
        ESP_GOTO_ON_FALSE(s_prof_tbl.count < MAX_PROFILES, ESP_ERR_NO_MEM, out, TAG,
                          "profile limit %d reached", MAX_PROFILES);
        uint32_t next = 1;
        ret = nvs_get_u32(s_nvs, KEY_NEXTPROF, &next);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
            next = 1;
        }
        ESP_GOTO_ON_ERROR(ret, out, TAG, "read nextprof failed");
        ESP_GOTO_ON_FALSE(next > 0 && next <= UINT8_MAX, ESP_ERR_INVALID_STATE, out, TAG,
                          "profile id space exhausted");

        id = (uint8_t)next;
        ir_profile_meta_t *p = &s_prof_tbl.items[s_prof_tbl.count];
        memset(p, 0, sizeof(*p));
        p->id = id;
        copy_name(p->name, name);
        s_prof_tbl.count++;

        ESP_GOTO_ON_ERROR(save_profile_table(), out, TAG, "save profile table failed");
        ESP_GOTO_ON_ERROR(nvs_set_u32(s_nvs, KEY_NEXTPROF, next + 1), out, TAG,
                          "write nextprof failed");
        ESP_GOTO_ON_ERROR(nvs_commit(s_nvs), out, TAG, "commit failed");
        *out_id = id;
    }
out:
    lock_give();
    return ret;
}

esp_err_t ir_store_profile_rename(uint8_t profile_id, const char *name)
{
    ESP_RETURN_ON_FALSE(name != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = load_profile_table();
    size_t index = 0;
    if (err == ESP_OK) {
        err = find_profile(profile_id, &index);
    }
    if (err == ESP_OK) {
        copy_name(s_prof_tbl.items[index].name, name);
        err = save_profile_table();
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
    }
    lock_give();
    return err;
}

esp_err_t ir_store_profile_delete(uint8_t profile_id)
{
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = load_profile_table();
    size_t index = 0;
    if (err == ESP_OK) {
        err = find_profile(profile_id, &index);
    }
    if (err == ESP_OK) {
        /* Drop every stored frame of this profile before forgetting its metadata. */
        if (load_button_table(profile_id) == ESP_OK) {
            for (size_t i = 0; i < s_btn_tbl.count && i < MAX_BUTTONS; i++) {
                command_delete(s_btn_tbl.items[i].command_id);
            }
        }
        char key[16];
        snprintf(key, sizeof(key), BUTTON_KEY_FMT, (unsigned)profile_id);
        esp_err_t erase_err = nvs_erase_key(s_nvs, key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "erase %s failed: %s", key, esp_err_to_name(erase_err));
        }

        memmove(&s_prof_tbl.items[index], &s_prof_tbl.items[index + 1],
                (size_t)(s_prof_tbl.count - index - 1) * sizeof(ir_profile_meta_t));
        s_prof_tbl.count--;
        err = save_profile_table();
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
    }
    lock_give();
    return err;
}

/* ---- buttons ------------------------------------------------------------- */

esp_err_t ir_store_button_list(uint8_t profile_id, ir_button_meta_t *out, size_t capacity,
                              size_t *count)
{
    ESP_RETURN_ON_FALSE(out != NULL && count != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = load_profile_table();
    size_t index = 0;
    if (err == ESP_OK) {
        err = find_profile(profile_id, &index);
    }
    if (err == ESP_OK) {
        err = load_button_table(profile_id);
    }
    if (err == ESP_OK) {
        size_t n = (s_btn_tbl.count < capacity) ? s_btn_tbl.count : capacity;
        memcpy(out, s_btn_tbl.items, n * sizeof(ir_button_meta_t));
        *count = n;
    }
    lock_give();
    return err;
}

esp_err_t ir_store_button_get(uint8_t profile_id, uint8_t button_id, ir_button_meta_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = load_button_table(profile_id);
    size_t index = 0;
    if (err == ESP_OK) {
        err = find_button(button_id, &index);
        if (err == ESP_OK) {
            *out = s_btn_tbl.items[index];
        }
    }
    lock_give();
    return err;
}

esp_err_t ir_store_button_save(uint8_t profile_id, const char *name, const ir_frame_t *frame,
                              const ir_tx_params_t *params, uint8_t *out_button_id)
{
    ESP_RETURN_ON_FALSE(name != NULL && frame != NULL && params != NULL && out_button_id != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t ret = load_profile_table();
    size_t pindex = 0;
    if (ret == ESP_OK) {
        ret = find_profile(profile_id, &pindex);
    }
    if (ret == ESP_OK) {
        ret = load_button_table(profile_id);
    }

    ir_command_id_t command_id = 0;
    uint8_t button_id = 0;

    if (ret == ESP_OK) {
        ESP_GOTO_ON_FALSE(s_btn_tbl.count < MAX_BUTTONS, ESP_ERR_NO_MEM, out, TAG,
                          "button limit %d reached", MAX_BUTTONS);
        ESP_GOTO_ON_ERROR(next_command_id(&command_id), out, TAG, "allocate command id failed");
        ESP_GOTO_ON_ERROR(command_write(command_id, frame, params), out, TAG,
                          "store frame failed");

        button_id = (uint8_t)(s_btn_tbl.count + 1);
        ir_button_meta_t *b = &s_btn_tbl.items[s_btn_tbl.count];
        memset(b, 0, sizeof(*b));
        b->id = button_id;
        copy_name(b->name, name);
        b->command_id = command_id;
        s_btn_tbl.count++;
        s_prof_tbl.items[pindex].button_count = s_btn_tbl.count;

        ret = save_button_table(profile_id);
        if (ret == ESP_OK) {
            ret = save_profile_table();
        }
        if (ret == ESP_OK) {
            ret = nvs_commit(s_nvs);
        }
        if (ret != ESP_OK) {
            /* Roll back so metadata and files cannot disagree. */
            s_btn_tbl.count--;
            s_prof_tbl.items[pindex].button_count = s_btn_tbl.count;
            command_delete(command_id);
        } else {
            *out_button_id = button_id;
        }
    }
out:
    lock_give();
    return ret;
}

esp_err_t ir_store_button_rename(uint8_t profile_id, uint8_t button_id, const char *name)
{
    ESP_RETURN_ON_FALSE(name != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = load_button_table(profile_id);
    size_t index = 0;
    if (err == ESP_OK) {
        err = find_button(button_id, &index);
    }
    if (err == ESP_OK) {
        copy_name(s_btn_tbl.items[index].name, name);
        err = save_button_table(profile_id);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
    }
    lock_give();
    return err;
}

esp_err_t ir_store_button_delete(uint8_t profile_id, uint8_t button_id)
{
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = load_profile_table();
    size_t pindex = 0;
    if (err == ESP_OK) {
        err = find_profile(profile_id, &pindex);
    }
    if (err == ESP_OK) {
        err = load_button_table(profile_id);
    }
    if (err == ESP_OK) {
        size_t index = 0;
        err = find_button(button_id, &index);
        if (err == ESP_OK) {
            command_delete(s_btn_tbl.items[index].command_id);
            memmove(&s_btn_tbl.items[index], &s_btn_tbl.items[index + 1],
                    (size_t)(s_btn_tbl.count - index - 1) * sizeof(ir_button_meta_t));
            s_btn_tbl.count--;
            s_prof_tbl.items[pindex].button_count = s_btn_tbl.count;
            err = save_button_table(profile_id);
            if (err == ESP_OK) {
                err = save_profile_table();
            }
            if (err == ESP_OK) {
                err = nvs_commit(s_nvs);
            }
        }
    }
    lock_give();
    return err;
}

/* ---- commands ------------------------------------------------------------ */

esp_err_t ir_store_command_load(ir_command_id_t command_id, ir_frame_t *out_frame,
                               ir_tx_params_t *out_params)
{
    ESP_RETURN_ON_FALSE(out_frame != NULL && out_params != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bad arguments");
    ESP_RETURN_ON_FALSE(command_id != 0, ESP_ERR_INVALID_ARG, TAG, "command id 0 is not valid");

    /* No lock: file IO only, and the file name depends solely on the caller's id. */
    return command_read(command_id, out_frame, out_params);
}

/* ---- hotkey -------------------------------------------------------------- */

esp_err_t ir_store_hotkey_get(ir_hotkey_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    memset(&s_hk, 0, sizeof(s_hk));
    size_t size = sizeof(s_hk);
    esp_err_t err = nvs_get_blob(s_nvs, KEY_HOTKEY, &s_hk, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* empty hotkey: step_count stays 0 */
    } else if (err == ESP_OK) {
        if (size != sizeof(s_hk) || s_hk.step_count > IR_STORE_MAX_HOTKEY_STEPS) {
            ESP_LOGW(TAG, "hotkey blob is invalid, treating as empty");
            memset(&s_hk, 0, sizeof(s_hk));
        } else {
            out->profile_id = s_hk.profile_id;
            out->step_count = s_hk.step_count;
            memcpy(out->steps, s_hk.steps, sizeof(out->steps));
        }
    }
    lock_give();
    return err;
}

esp_err_t ir_store_hotkey_set(const ir_hotkey_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_FALSE(in->step_count <= IR_STORE_MAX_HOTKEY_STEPS, ESP_ERR_INVALID_ARG, TAG,
                        "at most %d steps", IR_STORE_MAX_HOTKEY_STEPS);
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    memset(&s_hk, 0, sizeof(s_hk));
    s_hk.profile_id = in->profile_id;
    s_hk.step_count = in->step_count;
    memcpy(s_hk.steps, in->steps, sizeof(s_hk.steps));

    esp_err_t err = nvs_set_blob(s_nvs, KEY_HOTKEY, &s_hk, sizeof(s_hk));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    lock_give();
    return err;
}

esp_err_t ir_store_hotkey_clear(void)
{
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t err = nvs_erase_key(s_nvs, KEY_HOTKEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    lock_give();
    return err;
}

/* ---- stats / reset ------------------------------------------------------- */

esp_err_t ir_store_stats(size_t *out_profiles, size_t *out_commands, size_t *out_used_bytes)
{
    size_t profiles = 0;
    size_t commands = 0;

    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");
    esp_err_t err = load_profile_table();
    if (err == ESP_OK) {
        profiles = s_prof_tbl.count;
        for (size_t i = 0; i < s_prof_tbl.count && i < MAX_PROFILES; i++) {
            if (load_button_table(s_prof_tbl.items[i].id) == ESP_OK) {
                for (size_t j = 0; j < s_btn_tbl.count && j < MAX_BUTTONS; j++) {
                    if (s_btn_tbl.items[j].command_id != 0) {
                        commands++;
                    }
                }
            }
        }
    }
    lock_give();
    ESP_RETURN_ON_ERROR(err, TAG, "stats failed");

    if (out_profiles != NULL) {
        *out_profiles = profiles;
    }
    if (out_commands != NULL) {
        *out_commands = commands;
    }
    if (out_used_bytes != NULL) {
        size_t total = 0;
        size_t used = 0;
        if (esp_spiffs_info(CONFIG_IR_STORE_PARTITION_LABEL, &total, &used) == ESP_OK) {
            *out_used_bytes = used;
        } else {
            *out_used_bytes = 0;
        }
    }
    return ESP_OK;
}

esp_err_t ir_store_factory_reset(void)
{
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");
    esp_err_t err = wipe_all();
    lock_give();
    ESP_RETURN_ON_ERROR(err, TAG, "factory reset failed");
    ESP_LOGW(TAG, "store erased");
    return ESP_OK;
}

/* ---- export / import ----------------------------------------------------- */

typedef struct {
    ir_store_write_fn write;
    void *ctx;
    esp_err_t err;
} bundle_writer_t;

static void bw_raw(bundle_writer_t *w, const void *data, size_t len)
{
    if (w->err == ESP_OK && len > 0) {
        w->err = w->write(data, len, w->ctx);
    }
}

static void bw_u8(bundle_writer_t *w, uint8_t v)
{
    bw_raw(w, &v, sizeof(v));
}

static void bw_u16(bundle_writer_t *w, uint16_t v)
{
    bw_raw(w, &v, sizeof(v));
}

static void bw_u32(bundle_writer_t *w, uint32_t v)
{
    bw_raw(w, &v, sizeof(v));
}

static void bw_name(bundle_writer_t *w, const char *name)
{
    char buf[IR_STORE_NAME_LEN];
    memset(buf, 0, sizeof(buf));
    strncpy(buf, name, IR_STORE_NAME_LEN - 1);
    bw_raw(w, buf, sizeof(buf));
}

typedef struct {
    ir_store_read_fn read;
    void *ctx;
    esp_err_t err;
} bundle_reader_t;

static void br_raw(bundle_reader_t *r, void *data, size_t len)
{
    if (r->err == ESP_OK && len > 0) {
        r->err = r->read(data, len, r->ctx);
    }
}

static uint8_t br_u8(bundle_reader_t *r)
{
    uint8_t v = 0;
    br_raw(r, &v, sizeof(v));
    return v;
}

static uint16_t br_u16(bundle_reader_t *r)
{
    uint16_t v = 0;
    br_raw(r, &v, sizeof(v));
    return v;
}

static uint32_t br_u32(bundle_reader_t *r)
{
    uint32_t v = 0;
    br_raw(r, &v, sizeof(v));
    return v;
}

static void br_name(bundle_reader_t *r, char *out)
{
    br_raw(r, out, IR_STORE_NAME_LEN);
    out[IR_STORE_NAME_LEN - 1] = '\0';
}

esp_err_t ir_store_export(ir_store_write_fn write, void *ctx)
{
    ESP_RETURN_ON_FALSE(write != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    esp_err_t ret = load_profile_table();
    ESP_GOTO_ON_ERROR(ret, out, TAG, "read profiles failed");

    /* Collect every referenced command so the bundle is self-contained. */
    size_t command_count = 0;
    for (size_t i = 0; i < s_prof_tbl.count && i < MAX_PROFILES; i++) {
        if (load_button_table(s_prof_tbl.items[i].id) != ESP_OK) {
            continue;
        }
        for (size_t j = 0; j < s_btn_tbl.count && j < MAX_BUTTONS; j++) {
            ir_command_id_t id = s_btn_tbl.items[j].command_id;
            if (id == 0) {
                continue;
            }
            bool seen = false;
            for (size_t k = 0; k < command_count; k++) {
                if (s_cmd_ids[k] == id) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                ESP_GOTO_ON_FALSE(command_count < MAX_COMMANDS, ESP_ERR_NO_MEM, out, TAG,
                                  "too many commands to export");
                s_cmd_ids[command_count++] = id;
            }
        }
    }

    bundle_writer_t w = { .write = write, .ctx = ctx, .err = ESP_OK };
    bw_u32(&w, BUNDLE_MAGIC);
    bw_u16(&w, SCHEMA_VERSION);
    bw_u8(&w, s_prof_tbl.count);
    bw_u32(&w, (uint32_t)command_count);

    for (size_t i = 0; i < s_prof_tbl.count && w.err == ESP_OK; i++) {
        const ir_profile_meta_t *p = &s_prof_tbl.items[i];
        bw_u8(&w, p->id);

        if (load_button_table(p->id) != ESP_OK) {
            memset(&s_btn_tbl, 0, sizeof(s_btn_tbl));
        }
        bw_u8(&w, s_btn_tbl.count);
        for (size_t j = 0; j < s_btn_tbl.count; j++) {
            bw_u8(&w, s_btn_tbl.items[j].id);
            bw_u32(&w, s_btn_tbl.items[j].command_id);
            bw_name(&w, s_btn_tbl.items[j].name);
        }
        bw_name(&w, p->name);
    }

    /* Hotkey is an independent blob, so re-read it rather than trusting locals. */
    memset(&s_hk, 0, sizeof(s_hk));
    size_t hk_size = sizeof(s_hk);
    if (nvs_get_blob(s_nvs, KEY_HOTKEY, &s_hk, &hk_size) != ESP_OK ||
        hk_size != sizeof(s_hk) || s_hk.step_count > IR_STORE_MAX_HOTKEY_STEPS) {
        memset(&s_hk, 0, sizeof(s_hk));
    }
    bw_u8(&w, s_hk.profile_id);
    bw_u8(&w, s_hk.step_count);
    for (size_t i = 0; i < s_hk.step_count; i++) {
        bw_u32(&w, s_hk.steps[i].command_id);
        bw_u16(&w, s_hk.steps[i].delay_ms);
        bw_u8(&w, s_hk.steps[i].repeats);
    }

    for (size_t i = 0; i < command_count && w.err == ESP_OK; i++) {
        char path[48];
        command_path(s_cmd_ids[i], path, sizeof(path));
        FILE *f = fopen(path, "rb");
        ESP_GOTO_ON_FALSE(f != NULL, ESP_ERR_NOT_FOUND, out, TAG, "open %s failed", path);

        long size = 0;
        if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            ESP_GOTO_ON_FALSE(false, ESP_FAIL, out, TAG, "size %s failed", path);
        }

        bw_u32(&w, s_cmd_ids[i]);
        bw_u32(&w, (uint32_t)size);
        while (size > 0 && w.err == ESP_OK) {
            size_t chunk = ((size_t)size < sizeof(s_io_buf)) ? (size_t)size : sizeof(s_io_buf);
            if (fread(s_io_buf, 1, chunk, f) != chunk) {
                fclose(f);
                ESP_GOTO_ON_FALSE(false, ESP_FAIL, out, TAG, "read %s failed", path);
            }
            bw_raw(&w, s_io_buf, chunk);
            size -= (long)chunk;
        }
        if (fclose(f) != 0) {
            ESP_GOTO_ON_FALSE(false, ESP_FAIL, out, TAG, "close %s failed", path);
        }
    }

    ret = w.err;
out:
    lock_give();
    ESP_RETURN_ON_ERROR(ret, TAG, "export failed");
    return ESP_OK;
}

esp_err_t ir_store_import(ir_store_read_fn read, void *ctx)
{
    ESP_RETURN_ON_FALSE(read != NULL, ESP_ERR_INVALID_ARG, TAG, "bad arguments");
    ESP_RETURN_ON_ERROR(lock_take(), TAG, "lock");

    bundle_reader_t r = { .read = read, .ctx = ctx, .err = ESP_OK };

    uint32_t magic = br_u32(&r);
    uint16_t version = br_u16(&r);
    uint8_t profile_count = br_u8(&r);
    uint32_t command_count = br_u32(&r);

    esp_err_t ret = r.err;
    if (ret != ESP_OK) {
        goto out;
    }
    if (magic != BUNDLE_MAGIC) {
        ESP_LOGE(TAG, "bad bundle magic 0x%08" PRIx32, magic);
        ret = ESP_ERR_INVALID_ARG;
        goto out;
    }
    if (version != SCHEMA_VERSION) {
        ESP_LOGE(TAG, "bundle schema %u does not match firmware %u", (unsigned)version,
                 SCHEMA_VERSION);
        ret = ESP_ERR_INVALID_VERSION;
        goto out;
    }
    if (profile_count > MAX_PROFILES || command_count > MAX_COMMANDS) {
        ESP_LOGE(TAG, "bundle too large (%u profiles, %u commands)", (unsigned)profile_count,
                 (unsigned)command_count);
        ret = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    /* Wipe first: a partial import then leaves an empty store, never a mixture. */
    ESP_GOTO_ON_ERROR(wipe_all(), out, TAG, "wipe failed");

    memset(&s_prof_tbl, 0, sizeof(s_prof_tbl));
    s_prof_tbl.count = profile_count;

    for (size_t i = 0; i < profile_count && r.err == ESP_OK; i++) {
        ir_profile_meta_t *p = &s_prof_tbl.items[i];
        p->id = br_u8(&r);
        p->button_count = br_u8(&r);
        ESP_GOTO_ON_FALSE(p->button_count <= MAX_BUTTONS, ESP_ERR_INVALID_SIZE, out, TAG,
                          "too many buttons (%u)", (unsigned)p->button_count);

        memset(&s_btn_tbl, 0, sizeof(s_btn_tbl));
        s_btn_tbl.count = p->button_count;
        for (size_t j = 0; j < p->button_count; j++) {
            s_btn_tbl.items[j].id = br_u8(&r);
            s_btn_tbl.items[j].command_id = br_u32(&r);
            br_name(&r, s_btn_tbl.items[j].name);
        }
        br_name(&r, p->name);
        ESP_GOTO_ON_ERROR(r.err, out, TAG, "truncated profile %u", (unsigned)i);
        ESP_GOTO_ON_ERROR(save_button_table(p->id), out, TAG, "save buttons failed");
    }
    ESP_GOTO_ON_ERROR(r.err, out, TAG, "truncated profile table");
    ESP_GOTO_ON_ERROR(save_profile_table(), out, TAG, "save profiles failed");

    memset(&s_hk, 0, sizeof(s_hk));
    s_hk.profile_id = br_u8(&r);
    s_hk.step_count = br_u8(&r);
    ESP_GOTO_ON_FALSE(s_hk.step_count <= IR_STORE_MAX_HOTKEY_STEPS, ESP_ERR_INVALID_SIZE, out, TAG,
                      "too many hotkey steps (%u)", (unsigned)s_hk.step_count);
    for (size_t i = 0; i < s_hk.step_count; i++) {
        s_hk.steps[i].command_id = br_u32(&r);
        s_hk.steps[i].delay_ms = br_u16(&r);
        s_hk.steps[i].repeats = br_u8(&r);
    }
    ESP_GOTO_ON_ERROR(r.err, out, TAG, "truncated hotkey");
    ESP_GOTO_ON_ERROR(nvs_set_blob(s_nvs, KEY_HOTKEY, &s_hk, sizeof(s_hk)), out, TAG,
                      "save hotkey failed");

    ir_command_id_t highest = 0;
    for (size_t i = 0; i < command_count && r.err == ESP_OK; i++) {
        ir_command_id_t id = br_u32(&r);
        uint32_t size = br_u32(&r);
        ESP_GOTO_ON_FALSE(size > 0 && size <= 4096, ESP_ERR_INVALID_SIZE, out, TAG,
                          "bad command size %" PRIu32, size);

        char path[48];
        command_path(id, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        ESP_GOTO_ON_FALSE(f != NULL, ESP_ERR_NOT_FOUND, out, TAG, "open %s failed", path);

        uint32_t remaining = size;
        while (remaining > 0) {
            size_t chunk = (remaining < sizeof(s_io_buf)) ? remaining : sizeof(s_io_buf);
            br_raw(&r, s_io_buf, chunk);
            if (r.err != ESP_OK || fwrite(s_io_buf, 1, chunk, f) != chunk) {
                fclose(f);
                remove(path);
                ESP_GOTO_ON_FALSE(false, ESP_FAIL, out, TAG, "copy %s failed", path);
            }
            remaining -= (uint32_t)chunk;
        }
        if (fclose(f) != 0) {
            ESP_GOTO_ON_FALSE(false, ESP_FAIL, out, TAG, "close %s failed", path);
        }
        if (id > highest) {
            highest = id;
        }
    }

    /* Keep new ids above everything that was imported, so nothing can collide. */
    uint8_t max_profile = 0;
    for (size_t i = 0; i < s_prof_tbl.count; i++) {
        if (s_prof_tbl.items[i].id > max_profile) {
            max_profile = s_prof_tbl.items[i].id;
        }
    }
    ESP_GOTO_ON_ERROR(nvs_set_u32(s_nvs, KEY_NEXTPROF, (uint32_t)max_profile + 1), out, TAG,
                      "write nextprof failed");
    ESP_GOTO_ON_ERROR(nvs_set_u32(s_nvs, KEY_NEXTCMD, highest + 1), out, TAG,
                      "write nextcmd failed");
    ESP_GOTO_ON_ERROR(nvs_commit(s_nvs), out, TAG, "commit failed");

out:
    lock_give();
    ESP_RETURN_ON_ERROR(ret, TAG, "import failed");
    ESP_LOGI(TAG, "imported %u profile(s), %u command(s)", (unsigned)profile_count,
             (unsigned)command_count);
    return ESP_OK;
}
