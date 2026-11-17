#ifndef AUTH_FIDO_STORE_H
#define AUTH_FIDO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_FIDO_RP_MAX 64
#define AUTH_FIDO_USER_MAX 64
#define AUTH_FIDO_RECORD_SIZE 152
#define AUTH_FIDO_NVS_KEY_SIZE 16

typedef enum {
    AUTH_FIDO_OK,
    AUTH_FIDO_MISSING,
    AUTH_FIDO_ERROR
} auth_fido_result_t;

typedef struct {
    char rp_id[AUTH_FIDO_RP_MAX];
    uint8_t salt[16];
    uint8_t user_id[AUTH_FIDO_USER_MAX];
    uint8_t user_len;
    bool deleted;
} auth_fido_record_t;

typedef struct {
    char rp_id[AUTH_FIDO_RP_MAX];
    bool legacy;
} auth_fido_site_t;

/* Callbacks return ERROR for corrupt data and failed commits, never MISSING.
 * A successful write must be durable. The caller serializes store operations. */
typedef struct {
    void *ctx;
    auth_fido_result_t (*read)(void *, const char *, void *, size_t *);
    bool (*write)(void *, const char *, const void *, size_t);
    void (*random)(void *, void *, size_t);
    bool (*sha256)(void *, const void *, size_t, uint8_t[32]);
} auth_fido_io_t;

bool auth_fido_store_key(const auth_fido_io_t *io, const char *rp_id,
                         char key[AUTH_FIDO_NVS_KEY_SIZE]);
bool auth_fido_store_decode(const void *blob, size_t len, auth_fido_record_t *out);
auth_fido_result_t auth_fido_store_load(const auth_fido_io_t *io, const char *rp_id,
                                       bool create, auth_fido_record_t *out);
bool auth_fido_store_user(const auth_fido_io_t *io, const char *rp_id,
                          const uint8_t *user, size_t len);
bool auth_fido_store_delete(const auth_fido_io_t *io, const char *rp_id);

/* ESP-IDF adapter; only call from the application task, alongside seed/counter
 * operations and Lab list/delete/reset. No BLE callback may mutate the store. */
auth_fido_result_t auth_fido_load(const char *rp_id, bool create, auth_fido_record_t *out);
bool auth_fido_save_user(const char *rp_id, const uint8_t *user, size_t len);
bool auth_fido_delete(const char *rp_id);
uint8_t auth_fido_list(auth_fido_site_t *sites, uint8_t max_sites);

#endif
