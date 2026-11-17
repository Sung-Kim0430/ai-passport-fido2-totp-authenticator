#include "auth_fido_store.h"
#include "totp_sha2.h"
#include "fido_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX_BLOBS 24
typedef struct { char key[16]; uint8_t data[AUTH_FIDO_RECORD_SIZE]; size_t len; } blob_t;
static blob_t blobs[MAX_BLOBS];
static unsigned count, randoms, writes;
static bool fail_read, fail_write, collision;
static const char *a = "same-prefix-site-a.example";
static const char *b = "same-prefix-site-b.example";

static auth_fido_result_t read_blob(void *ctx, const char *key, void *out, size_t *len)
{
    (void)ctx;
    if (fail_read) return AUTH_FIDO_ERROR;
    for (unsigned i = 0; i < count; ++i) {
        if (strcmp(blobs[i].key, key)) continue;
        if (*len < blobs[i].len) return AUTH_FIDO_ERROR;
        *len = blobs[i].len; memcpy(out, blobs[i].data, *len); return AUTH_FIDO_OK;
    }
    return AUTH_FIDO_MISSING;
}
static bool write_blob(void *ctx, const char *key, const void *data, size_t len)
{
    (void)ctx;
    ++writes;
    if (fail_write) return false;
    unsigned i = 0;
    while (i < count && strcmp(blobs[i].key, key)) ++i;
    assert(i < MAX_BLOBS && len <= sizeof(blobs[i].data));
    if (i == count) ++count;
    snprintf(blobs[i].key, sizeof(blobs[i].key), "%s", key);
    memcpy(blobs[i].data, data, len); blobs[i].len = len;
    return true;
}
static void random_bytes(void *ctx, void *out, size_t len)
{ (void)ctx; memset(out, (int)++randoms, len); }
/* Stable full-input digest for exercising store identity; the adapter uses SHA-256. */
static bool digest(void *ctx, const void *data, size_t len, uint8_t out[32])
{
    (void)ctx;
    totp_hmac_sha2((const uint8_t *)"test", 4, data, len, false, out);
    if (collision) memset(out, 0, 32);
    return true;
}
static const auth_fido_io_t io = {NULL, read_blob, write_blob, random_bytes, digest};
static void reset(void)
{ memset(blobs,0,sizeof(blobs));count=randoms=writes=0;fail_read=fail_write=collision=false; }
static void add_legacy(void)
{
    uint8_t salt[16]; memset(salt,0x39,sizeof(salt));
    assert(write_blob(NULL,"s_same-prefix-",salt,sizeof(salt)));
    assert(write_blob(NULL,"u_same-prefix-","old",3));
}
static void credential(const auth_fido_record_t *record, const char *prefix, uint8_t out[32])
{
    uint8_t msg[100], seed[32]={0x31};
    size_t n=strlen(record->rp_id);memcpy(msg,prefix,4);memcpy(msg+4,record->rp_id,n);
    memcpy(msg+4+n,record->salt,16);totp_hmac_sha2(seed,32,msg,n+20,false,out);
}
static void same_credential(const auth_fido_record_t *x, const auth_fido_record_t *y)
{
    uint8_t dx[32],dy[32];
    credential(x,"cred",dx);credential(y,"cred",dy);assert(!memcmp(dx,dy,32));
    credential(x,"priv",dx);credential(y,"priv",dy);assert(!memcmp(dx,dy,32));
}
static void rejects_old_credential(const auth_fido_record_t *old, const auth_fido_record_t *fresh)
{
    uint8_t old_id[32],new_id[32];credential(old,"cred",old_id);credential(fresh,"cred",new_id);
    uint8_t list[]={0x81,0xa1,0x62,'i','d',0x44,0,0,0,0};
    memcpy(list+6,old_id,4);
    assert(ctap2_allow_list_contains(list,sizeof(list),old_id,4));
    assert(!ctap2_allow_list_contains(list,sizeof(list),new_id,4));
}

int main(void)
{
    auth_fido_record_t ra, rb, old, again;
    char ka[16],kb[16];
    reset();
    assert(auth_fido_store_key(&io,a,ka) && auth_fido_store_key(&io,b,kb));
    assert(strlen(ka)==15 && strcmp(ka,kb));
    assert(auth_fido_store_load(&io,a,false,&ra)==AUTH_FIDO_MISSING && !randoms && !writes);
    assert(auth_fido_store_load(&io,a,true,&ra)==AUTH_FIDO_OK);
    assert(auth_fido_store_load(&io,b,true,&rb)==AUTH_FIDO_OK);
    assert(memcmp(ra.salt,rb.salt,16));
    assert(auth_fido_store_user(&io,a,(const uint8_t *)"Alice",5));
    assert(auth_fido_store_user(&io,b,(const uint8_t *)"Bob",3));
    assert(auth_fido_store_load(&io,a,false,&ra)==AUTH_FIDO_OK && ra.user_len==5 && !memcmp(ra.user_id,"Alice",5));
    assert(auth_fido_store_load(&io,b,false,&rb)==AUTH_FIDO_OK && rb.user_len==3 && !memcmp(rb.user_id,"Bob",3));
    assert(auth_fido_store_delete(&io,a));
    assert(auth_fido_store_load(&io,a,false,&again)==AUTH_FIDO_MISSING);
    assert(auth_fido_store_load(&io,b,false,&again)==AUTH_FIDO_OK);same_credential(&rb,&again);
    assert(auth_fido_store_load(&io,a,true,&again)==AUTH_FIDO_OK && memcmp(ra.salt,again.salt,16));
    rejects_old_credential(&ra,&again);
    reset();add_legacy();
    assert(auth_fido_store_load(&io,a,false,&ra)==AUTH_FIDO_OK && !randoms);
    old=ra;memset(old.salt,0x39,16);same_credential(&old,&ra);
    assert(ra.user_len==3 && !memcmp(ra.user_id,"old",3));
    assert(auth_fido_store_user(&io,a,(const uint8_t *)"Alice",5));
    assert(auth_fido_store_delete(&io,a));
    assert(auth_fido_store_load(&io,a,false,&again)==AUTH_FIDO_MISSING);
    /* The second old site still migrates the original salt and user handle. */
    assert(auth_fido_store_load(&io,b,false,&rb)==AUTH_FIDO_OK && !randoms);
    assert(!memcmp(rb.salt,old.salt,16) && rb.user_len==3);
    assert(auth_fido_store_load(&io,a,true,&again)==AUTH_FIDO_OK && memcmp(again.salt,old.salt,16));
    rejects_old_credential(&old,&again);
    assert(auth_fido_store_load(&io,b,false,&again)==AUTH_FIDO_OK);same_credential(&rb,&again);
    reset();collision=true;
    assert(auth_fido_store_load(&io,a,true,&ra)==AUTH_FIDO_OK);
    unsigned before=writes;
    assert(auth_fido_store_load(&io,b,true,&rb)==AUTH_FIDO_ERROR && writes==before);
    assert(!auth_fido_store_delete(&io,b) && !auth_fido_store_user(&io,b,(const uint8_t *)"x",1));
    reset();add_legacy();fail_read=true;
    assert(auth_fido_store_load(&io,a,true,&ra)==AUTH_FIDO_ERROR && !randoms);
    fail_read=false;fail_write=true;
    assert(auth_fido_store_load(&io,a,false,&ra)==AUTH_FIDO_ERROR && !randoms);
    fail_write=false;assert(auth_fido_store_load(&io,a,false,&ra)==AUTH_FIDO_OK);
    fail_write=true;assert(!auth_fido_store_delete(&io,a));
    assert(!auth_fido_store_user(&io,a,(const uint8_t *)"new",3));
    fail_write=false;assert(auth_fido_store_load(&io,a,false,&again)==AUTH_FIDO_OK);same_credential(&ra,&again);
    assert(again.user_len==3 && !memcmp(again.user_id,"old",3));
    /* Existing corrupt v2 material never falls back to the still-valid legacy salt. */
    assert(auth_fido_store_key(&io,a,ka));
    for(unsigned i=0;i<count;++i)if(!strcmp(blobs[i].key,ka))blobs[i].len--;
    before=writes;
    assert(auth_fido_store_load(&io,a,true,&ra)==AUTH_FIDO_ERROR && writes==before && !randoms);
    reset();add_legacy();blobs[0].len=15;
    assert(auth_fido_store_load(&io,a,true,&ra)==AUTH_FIDO_ERROR && !randoms);
    reset();
    assert(!auth_fido_store_key(&io,"",ka));
    char too_long[65];memset(too_long,'a',64);too_long[64]=0;
    assert(!auth_fido_store_key(&io,too_long,ka));
    assert(!auth_fido_store_user(&io,a,(const uint8_t *)"",0));
    assert(!auth_fido_store_user(&io,a,(const uint8_t *)too_long,65));
    /* Maximum full identities and binary user handles round-trip exactly. */
    reset();char longest[64];memset(longest,'a',63);longest[63]=0;
    uint8_t binary_user[64];for(unsigned i=0;i<64;++i)binary_user[i]=(uint8_t)i;
    assert(auth_fido_store_load(&io,longest,true,&ra)==AUTH_FIDO_OK);
    assert(auth_fido_store_user(&io,longest,binary_user,sizeof(binary_user)));
    assert(auth_fido_store_load(&io,longest,false,&ra)==AUTH_FIDO_OK);
    assert(ra.user_len==64 && !memcmp(ra.user_id,binary_user,64) && !strcmp(ra.rp_id,longest));
    uint8_t valid[AUTH_FIDO_RECORD_SIZE];memcpy(valid,blobs[0].data,sizeof(valid));
    const unsigned offsets[]={0,4,5,6,7,8,71};
    const uint8_t bad_values[]={0,3,65,64,1,0,'x'};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
        memcpy(blobs[0].data,valid,sizeof(valid));blobs[0].data[offsets[i]]=bad_values[i];
        assert(auth_fido_store_load(&io,longest,true,&again)==AUTH_FIDO_ERROR);
    }
    memcpy(blobs[0].data,valid,sizeof(valid));blobs[0].len=sizeof(valid)-1;
    assert(!auth_fido_store_decode(blobs[0].data,blobs[0].len,&again));
    puts("Full RP-ID storage, migration, deletion and failure isolation: PASS");
}
