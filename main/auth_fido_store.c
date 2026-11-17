#include "auth_fido_store.h"

#include <stdio.h>
#include <string.h>

static size_t rp_length(const char *rp)
{
    if (!rp) return 0;
    for (size_t i = 0; i < AUTH_FIDO_RP_MAX; ++i) {
        if (!rp[i]) return i;
        if ((unsigned char)rp[i] < 0x21 || (unsigned char)rp[i] > 0x7e) return 0;
    }
    return 0;
}

bool auth_fido_store_key(const auth_fido_io_t *io, const char *rp, char key[16])
{
    size_t len = rp_length(rp);
    uint8_t digest[32];
    if (!io || !io->sha256 || !key || !len || !io->sha256(io->ctx, rp, len, digest)) return false;
    memcpy(key, "r2_", 3);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 6; ++i) {
        key[3 + i * 2] = hex[digest[i] >> 4];
        key[4 + i * 2] = hex[digest[i] & 15];
    }
    key[15] = 0;
    return true;
}

bool auth_fido_store_decode(const void *data, size_t len, auth_fido_record_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!data || len != AUTH_FIDO_RECORD_SIZE) return false;
    const uint8_t *p = data;
    if (memcmp(p, "FRP2", 4) || (p[4] != 1 && p[4] != 2) ||
        p[5] > AUTH_FIDO_USER_MAX || !p[6] || p[6] >= AUTH_FIDO_RP_MAX || p[7]) return false;
    char rp[AUTH_FIDO_RP_MAX];
    memcpy(rp, p + 8, sizeof(rp));
    if (rp_length(rp) != p[6]) return false;
    if (p[4] == 2) {
        if (p[5]) return false;
        for (size_t i = 72; i < AUTH_FIDO_RECORD_SIZE; ++i) if (p[i]) return false;
    }
    memcpy(out->rp_id, rp, p[6]);
    memcpy(out->salt, p + 72, 16);
    memcpy(out->user_id, p + 88, p[5]);
    out->user_len = p[5];
    out->deleted = p[4] == 2;
    return true;
}

static bool save(const auth_fido_io_t *io, const char *key, const auth_fido_record_t *record)
{
    uint8_t blob[AUTH_FIDO_RECORD_SIZE] = {0};
    memcpy(blob, "FRP2", 4);
    blob[4] = record->deleted ? 2 : 1;
    blob[5] = record->deleted ? 0 : record->user_len;
    blob[6] = (uint8_t)strlen(record->rp_id);
    memcpy(blob + 8, record->rp_id, blob[6]);
    if (!record->deleted) {
        memcpy(blob + 72, record->salt, 16);
        memcpy(blob + 88, record->user_id, record->user_len);
    }
    return io->write(io->ctx, key, blob, sizeof(blob));
}

static auth_fido_result_t load_record(const auth_fido_io_t *io, const char *rp,
                                      const char *key, auth_fido_record_t *out)
{
    uint8_t blob[AUTH_FIDO_RECORD_SIZE];
    size_t len = sizeof(blob);
    auth_fido_result_t result = io->read(io->ctx, key, blob, &len);
    if (result != AUTH_FIDO_OK) return result;
    if (!auth_fido_store_decode(blob, len, out) || strcmp(out->rp_id, rp)) {
        memset(out, 0, sizeof(*out));
        return AUTH_FIDO_ERROR;
    }
    return AUTH_FIDO_OK;
}

auth_fido_result_t auth_fido_store_load(const auth_fido_io_t *io, const char *rp,
                                       bool create, auth_fido_record_t *out)
{
    if (!out) return AUTH_FIDO_ERROR;
    memset(out, 0, sizeof(*out));
    char key[16];
    if (!io || !io->read || !io->write || !io->random || !auth_fido_store_key(io, rp, key)) return AUTH_FIDO_ERROR;
    auth_fido_record_t record = {0};
    auth_fido_result_t result = load_record(io, rp, key, &record);
    if (result == AUTH_FIDO_ERROR) return result;
    if (result == AUTH_FIDO_OK && !record.deleted) { *out = record; return AUTH_FIDO_OK; }
    /* A tombstone always wins over legacy data, including on re-registration. */
    if (result == AUTH_FIDO_OK && !create) return AUTH_FIDO_MISSING;
    bool fresh = result == AUTH_FIDO_OK;
    memset(&record, 0, sizeof(record));
    snprintf(record.rp_id, sizeof(record.rp_id), "%s", rp);
    if (!fresh) {
        char legacy_key[16];
        snprintf(legacy_key, sizeof(legacy_key), "s_%.12s", rp);
        size_t len = sizeof(record.salt);
        result = io->read(io->ctx, legacy_key, record.salt, &len);
        if (result == AUTH_FIDO_ERROR || (result == AUTH_FIDO_OK && len != sizeof(record.salt))) return AUTH_FIDO_ERROR;
        if (result == AUTH_FIDO_MISSING) {
            if (!create) return AUTH_FIDO_MISSING;
            fresh = true;
        } else {
            snprintf(legacy_key, sizeof(legacy_key), "u_%.12s", rp);
            len = sizeof(record.user_id);
            result = io->read(io->ctx, legacy_key, record.user_id, &len);
            if (result == AUTH_FIDO_ERROR || (result == AUTH_FIDO_OK && (!len || len > sizeof(record.user_id)))) return AUTH_FIDO_ERROR;
            if (result == AUTH_FIDO_OK) record.user_len = (uint8_t)len;
        }
    }
    if (fresh) io->random(io->ctx, record.salt, sizeof(record.salt));
    if (!save(io, key, &record)) return AUTH_FIDO_ERROR;
    *out = record;
    return AUTH_FIDO_OK;
}

bool auth_fido_store_user(const auth_fido_io_t *io, const char *rp, const uint8_t *user, size_t len)
{
    if (!user || !len || len > AUTH_FIDO_USER_MAX) return false;
    auth_fido_record_t record;
    char key[16];
    if (auth_fido_store_load(io, rp, false, &record) != AUTH_FIDO_OK || !auth_fido_store_key(io, rp, key)) return false;
    memset(record.user_id, 0, sizeof(record.user_id));
    memcpy(record.user_id, user, len);
    record.user_len = (uint8_t)len;
    return save(io, key, &record);
}

bool auth_fido_store_delete(const auth_fido_io_t *io, const char *rp)
{
    auth_fido_record_t record;
    char key[16];
    if (auth_fido_store_load(io, rp, false, &record) != AUTH_FIDO_OK || !auth_fido_store_key(io, rp, key)) return false;
    record.deleted = true;
    return save(io, key, &record);
}
