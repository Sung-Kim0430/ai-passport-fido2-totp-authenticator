#include "auth_fido_store.h"

#ifdef ESP_PLATFORM
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static auth_fido_result_t read_blob(void *ctx, const char *key, void *data, size_t *len)
{
    nvs_handle_t handle = *(nvs_handle_t *)ctx;
    esp_err_t err = nvs_get_blob(handle, key, data, len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return AUTH_FIDO_MISSING;
    /* A prior failed commit may have left readable pending data. */
    if (err == ESP_OK) err = nvs_commit(handle);
    return err == ESP_OK ? AUTH_FIDO_OK : AUTH_FIDO_ERROR;
}

static bool write_blob(void *ctx, const char *key, const void *data, size_t len)
{
    nvs_handle_t handle = *(nvs_handle_t *)ctx;
    esp_err_t err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    return err == ESP_OK;
}

static void random_bytes(void *ctx, void *data, size_t len)
{ (void)ctx; esp_fill_random(data, len); }

static bool sha256(void *ctx, const void *data, size_t len, uint8_t digest[32])
{ (void)ctx; return mbedtls_sha256(data, len, digest, 0) == 0; }

static auth_fido_io_t make_io(nvs_handle_t *handle)
{
    return (auth_fido_io_t){handle, read_blob, write_blob, random_bytes, sha256};
}

auth_fido_result_t auth_fido_load(const char *rp, bool create, auth_fido_record_t *out)
{
    if (!out) return AUTH_FIDO_ERROR;
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    if (nvs_open("fido_sec", NVS_READWRITE, &handle) != ESP_OK) return AUTH_FIDO_ERROR;
    auth_fido_io_t io = make_io(&handle);
    auth_fido_result_t result = auth_fido_store_load(&io, rp, create, out);
    nvs_close(handle);
    return result;
}

bool auth_fido_save_user(const char *rp, const uint8_t *user, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open("fido_sec", NVS_READWRITE, &handle) != ESP_OK) return false;
    auth_fido_io_t io = make_io(&handle);
    bool ok = auth_fido_store_user(&io, rp, user, len);
    nvs_close(handle);
    return ok;
}

bool auth_fido_delete(const char *rp)
{
    nvs_handle_t handle;
    if (nvs_open("fido_sec", NVS_READWRITE, &handle) != ESP_OK) return false;
    auth_fido_io_t io = make_io(&handle);
    bool ok = auth_fido_store_delete(&io, rp);
    nvs_close(handle);
    return ok;
}

uint8_t auth_fido_list(auth_fido_site_t *sites, uint8_t max_sites)
{
    if (!sites || !max_sites) return 0;
    nvs_handle_t handle;
    if (nvs_open("fido_sec", NVS_READONLY, &handle) != ESP_OK) return 0;
    auth_fido_io_t io = make_io(&handle);
    uint8_t count = 0;
    /* Full identities take priority, so retained legacy rows cannot crowd out
     * migrated credentials in the bounded management list. */
    for (unsigned pass = 0; pass < 2 && count < max_sites; ++pass) {
        nvs_iterator_t it = NULL;
        esp_err_t err = nvs_entry_find("nvs", "fido_sec", NVS_TYPE_BLOB, &it);
        while (err == ESP_OK && it && count < max_sites) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (pass == 0 && strncmp(info.key, "r2_", 3) == 0) {
                uint8_t blob[AUTH_FIDO_RECORD_SIZE];
                size_t len = sizeof(blob);
                auth_fido_record_t record;
                char expected[16];
                if (nvs_get_blob(handle, info.key, blob, &len) == ESP_OK &&
                    auth_fido_store_decode(blob, len, &record) && !record.deleted && record.user_len &&
                    auth_fido_store_key(&io, record.rp_id, expected) && !strcmp(info.key, expected)) {
                    snprintf(sites[count].rp_id, sizeof(sites[count].rp_id), "%s", record.rp_id);
                    sites[count++].legacy = false;
                }
            } else if (pass == 1 && strncmp(info.key, "u_", 2) == 0 && info.key[2]) {
                snprintf(sites[count].rp_id, sizeof(sites[count].rp_id), "%.12s", info.key + 2);
                sites[count++].legacy = true;
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
    }
    nvs_close(handle);
    return count;
}
#endif
