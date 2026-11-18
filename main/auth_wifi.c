/* 热点管理：HTTP 接收有界请求，命令经队列交给应用执行；共享快照不包含 OTP 密钥。
 * DNS 任务独占套接字并自行关闭，主任务等待退出后才释放 HTTP、Wi-Fi 与 netif。
 * 部分停止失败时保持资源占用状态，禁止误以为已空闲而启动另一个无线栈。
 */
#include "auth_wifi.h"
#include "totp_crypto.h"
#include "rtc_clock.h"
#include "auth_time_sync.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static auth_identity_t s_identity;
static bool s_identity_ready;

const auth_identity_t *auth_wifi_get_identity(void)
{
    return s_identity_ready ? &s_identity : NULL;
}

#ifdef ESP_PLATFORM
#include "auth_web_page.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <unistd.h>

static const char *TAG = "auth_wifi";
static volatile bool s_dns_run;
static volatile bool s_wifi_started;
static bool s_wifi_initialized;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_server;
static auth_wifi_cmd_cb_t s_cmd_cb;
static TaskHandle_t volatile s_dns_task;
static StaticSemaphore_t s_dns_ready_storage;
static SemaphoreHandle_t s_dns_ready;
static volatile bool s_dns_ok;
static char s_session[33];
static uint32_t s_request_id;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    char issuer[AUTH_ISSUER_MAX_LEN];
    char account[AUTH_ACCOUNT_MAX_LEN];
    uint8_t digits;
    uint8_t period;
    uint8_t algorithm;
} public_account_t;

typedef struct {
    uint32_t id;
    bool ok;
} command_result_t;

static struct {
    uint64_t time;
    uint32_t revision;
    uint8_t count;
    uint8_t brightness;
    bool sound;
    bool unlocked;
    bool time_set;
    rtc_clock_diagnostics_t clock;
    public_account_t accounts[AUTH_MAX_ACCOUNTS];
    command_result_t results[4];
} s_snapshot;

typedef struct {
    uint32_t id;
    int64_t hrt_us;
    int64_t window_us;
    bool valid;
    uint64_t reference_id;
} time_probe_t;

static time_probe_t s_time_probes[4];
static uint32_t s_time_probe_counter;
static uint32_t s_time_probe_nonce;
static auth_time_sync_cache_t s_precise_time_probes;

static void time_probe_new_session(void)
{
    memset(s_time_probes, 0, sizeof(s_time_probes));
    s_time_probe_nonce = esp_random();
    auth_time_sync_reset(&s_precise_time_probes, esp_random());
}

static time_probe_t time_probe_record(int64_t hrt_us, int64_t window_us, bool ordered)
{
    if (++s_time_probe_counter == 0) {
        time_probe_new_session();
        ++s_time_probe_counter;
    }
    bool valid = ordered && hrt_us >= 0 && window_us >= 0 && window_us <= 1000;
    time_probe_t probe = {
        .id = s_time_probe_counter,
        .hrt_us = valid ? hrt_us : 0,
        .window_us = valid ? window_us : 0,
        .valid = valid,
        .reference_id = ((uint64_t)s_time_probe_nonce << 32) | s_time_probe_counter,
    };
    s_time_probes[(probe.id - 1) % 4] = probe;
    return probe;
}

bool auth_wifi_init_identity(void)
{
    if (s_identity_ready) return true;
    uint8_t chip_id[6];
    if (esp_efuse_mac_get_default(chip_id) != ESP_OK) return false;
    s_identity_ready = auth_identity_from_chip_id(chip_id, &s_identity);
    return s_identity_ready;
}

void auth_wifi_publish_config(const auth_config_t *cfg, bool unlocked)
{
    if (!cfg || cfg->account_count > AUTH_MAX_ACCOUNTS) return;
    uint64_t now = auth_get_current_timestamp(cfg);
    const bool time_set = auth_time_is_set();
    const rtc_clock_diagnostics_t clock_evidence = rtc_clock_diagnostics();
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.time = now;
    s_snapshot.time_set = time_set;
    s_snapshot.clock = clock_evidence;
    s_snapshot.revision = auth_storage_account_revision();
    memset(s_snapshot.accounts, 0, sizeof(s_snapshot.accounts));
    s_snapshot.count = cfg->account_count;
    s_snapshot.brightness = cfg->brightness;
    s_snapshot.sound = cfg->sound_enabled;
    s_snapshot.unlocked = unlocked;
    for (uint8_t i = 0; i < cfg->account_count; i++) {
        memcpy(s_snapshot.accounts[i].issuer, cfg->accounts[i].issuer, AUTH_ISSUER_MAX_LEN);
        memcpy(s_snapshot.accounts[i].account, cfg->accounts[i].account, AUTH_ACCOUNT_MAX_LEN);
        s_snapshot.accounts[i].digits = cfg->accounts[i].digits;
        s_snapshot.accounts[i].period = cfg->accounts[i].period;
        s_snapshot.accounts[i].algorithm = cfg->accounts[i].algorithm;
    }
    portEXIT_CRITICAL(&s_snapshot_lock);
}

void auth_wifi_command_result(uint32_t id, bool ok)
{
    if (!id) return;
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.results[id % 4] = (command_result_t){.id = id, .ok = ok};
    portEXIT_CRITICAL(&s_snapshot_lock);
}

typedef struct {
    uint64_t generation;
    size_t cursor;
    uint64_t id;
    bool has_generation;
    bool enabled;
    bool pending;
    bool processing;
    bool complete;
    unsigned result;
    uint32_t learning_reason;
    rtc_clock_history_summary_t summary;
    rtc_history_page_t page;
} history_mailbox_t;

static history_mailbox_t s_history_mailbox;
static portMUX_TYPE s_history_lock = portMUX_INITIALIZER_UNLOCKED;
_Static_assert(sizeof(history_mailbox_t) <= 1024, "History mailbox RAM bound");

static esp_err_t error_response(httpd_req_t *req, const char *status, const char *message);

static void history_mailbox_reset(bool enabled)
{
    portENTER_CRITICAL(&s_history_lock);
    s_history_mailbox.enabled = enabled;
    s_history_mailbox.pending = false;
    s_history_mailbox.processing = false;
    s_history_mailbox.complete = false;
    ++s_history_mailbox.id;
    portEXIT_CRITICAL(&s_history_lock);
}

void auth_wifi_history_service(bool unlocked)
{
    uint64_t id, generation;
    size_t cursor;
    bool has_generation;
    portENTER_CRITICAL(&s_history_lock);
    if (!s_history_mailbox.enabled || !s_history_mailbox.pending || s_history_mailbox.processing) {
        portEXIT_CRITICAL(&s_history_lock);
        return;
    }
    s_history_mailbox.processing = true;
    id = s_history_mailbox.id;
    generation = s_history_mailbox.generation;
    cursor = s_history_mailbox.cursor;
    has_generation = s_history_mailbox.has_generation;
    portEXIT_CRITICAL(&s_history_lock);
    rtc_clock_history_summary_t summary = {0};
    rtc_history_page_t page = {0};
    unsigned result = 423;
    uint32_t reason = 0;
    if (unlocked) {
        summary = rtc_clock_history_owner_summary();
        result = !summary.ready ? 503 : has_generation && generation != summary.generation ? 409 : 200;
        if (result == 200 && !rtc_clock_history_owner_page(summary.generation, cursor, &page)) result = 409;
        if (result == 200 && (page.count > RTC_HISTORY_PAGE_SIZE || page.total > RTC_HISTORY_CAPACITY ||
            page.cursor != cursor || page.cursor > page.total || page.count > page.total - page.cursor ||
            page.generation != summary.generation)) result = 503;
        portENTER_CRITICAL(&s_snapshot_lock);
        reason = s_snapshot.clock.learning_reason;
        portEXIT_CRITICAL(&s_snapshot_lock);
    }
    portENTER_CRITICAL(&s_history_lock);
    if (s_history_mailbox.enabled && s_history_mailbox.pending && s_history_mailbox.id == id) {
        s_history_mailbox.summary = summary;
        s_history_mailbox.page = page;
        s_history_mailbox.result = result;
        s_history_mailbox.learning_reason = reason;
        s_history_mailbox.complete = true;
    }
    portEXIT_CRITICAL(&s_history_lock);
}

static bool history_decimal(const char *text, size_t length, uint64_t *value)
{
    if (!length || length > 20) return false;
    uint64_t n = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        unsigned digit = (unsigned)(text[i] - '0');
        if (n > (UINT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = n;
    return true;
}

static bool history_query(httpd_req_t *req, uint64_t *generation, size_t *cursor, bool *has_generation)
{
    char query[80];
    size_t length = httpd_req_get_url_query_len(req);
    *cursor = 0;
    *generation = 0;
    *has_generation = false;
    bool has_cursor = false;
    if (!length) return true;
    if (length >= sizeof(query) || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char *part = query;
    while (*part) {
        char *end = strchr(part, '&');
        if (!end) end = part + strlen(part);
        char *equals = memchr(part, '=', (size_t)(end - part));
        uint64_t value;
        if (!equals || !history_decimal(equals + 1, (size_t)(end - equals - 1), &value)) return false;
        if (equals - part == 6 && !memcmp(part, "cursor", 6) && !has_cursor) {
            if (value > RTC_HISTORY_CAPACITY) return false;
            *cursor = (size_t)value;
            has_cursor = true;
        } else if (equals - part == 10 && !memcmp(part, "generation", 10) && !*has_generation) {
            *generation = value;
            *has_generation = true;
        } else return false;
        if (!*end) break;
        part = end + 1;
        if (!*part) return false;
    }
    return !*cursor || *has_generation;
}

static esp_err_t history_send(httpd_req_t *req, const rtc_clock_history_summary_t *s,
    const rtc_history_page_t *page, uint32_t reason)
{
    char buf[768];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    int n = snprintf(buf, sizeof(buf),
        "{\"generation\":\"%llu\",\"cursor\":%u,\"total\":%u,\"next_cursor\":%u,\"summary\":{"
        "\"ready\":%s,\"accepted_sequence\":\"%llu\",\"durable_sequence\":\"%llu\",\"dropped\":\"%llu\",\"save_budget_us\":\"%llu\","
        "\"durable_count\":%lu,\"pending_count\":%lu,\"flags\":%lu,\"store_status\":%u,\"save_result\":%u,"
        "\"active_ppm\":%ld,\"active_qualified\":%s,\"legacy_candidate_ppm\":%ld,\"legacy_candidate_qualified\":%s,\"learning_reason\":%lu,",
        (unsigned long long)s->generation, (unsigned)page->cursor, (unsigned)page->total, (unsigned)(page->cursor + page->count),
        s->ready ? "true" : "false", (unsigned long long)s->accepted_sequence, (unsigned long long)s->durable_sequence,
        (unsigned long long)s->dropped, (unsigned long long)s->save_budget_us, (unsigned long)s->durable_count,
        (unsigned long)s->pending_count, (unsigned long)s->flags, (unsigned)s->store_status, (unsigned)s->save_result,
        (long)s->active_ppm, s->active_qualified ? "true" : "false", (long)s->legacy_candidate_ppm,
        s->legacy_candidate_qualified ? "true" : "false", (unsigned long)reason);
    if (n < 0 || (size_t)n >= sizeof(buf) || httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
    const rtc_history_estimate_t *e = &s->shadow;
    n = snprintf(buf, sizeof(buf),
        "\"shadow\":{\"version\":%lu,\"source\":%lu,\"result\":%u,\"samples\":%u,\"inliers\":%u,\"excluded\":%u,"
        "\"ppm\":%.6g,\"uncertainty_ppm\":%.6g,\"holdout_baseline_error_us\":%.6g,\"holdout_candidate_error_us\":%.6g,\"holdout_present\":%s,\"holdout_improved\":%s}},\"rows\":[",
        (unsigned long)e->version, (unsigned long)e->source, (unsigned)e->result, (unsigned)e->samples,
        (unsigned)e->inliers, (unsigned)e->excluded, isfinite(e->ppm) ? e->ppm : 0,
        isfinite(e->uncertainty_ppm) ? e->uncertainty_ppm : 0,
        isfinite(e->holdout_baseline_error_us) ? e->holdout_baseline_error_us : 0,
        isfinite(e->holdout_candidate_error_us) ? e->holdout_candidate_error_us : 0,
        e->holdout_present ? "true" : "false", e->holdout_improved ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(buf) || httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
    for (size_t i = 0; i < page->count; ++i) {
        const rtc_history_record_t *r = &page->rows[i];
        n = snprintf(buf, sizeof(buf),
            "%s{\"sequence\":\"%llu\",\"source\":%lu,\"utc_end_us\":\"%llu\",\"raw_us\":\"%llu\",\"raw_fraction_q19\":%lu,"
            "\"awake_delta_us\":\"%llu\",\"utc_delta_us\":\"%llu\",\"combined_uncertainty_us\":\"%llu\",\"flags\":%u,"
            "\"temperature_centi_c\":%d,\"battery_mv\":%u,\"temp_age_s\":%u,\"battery_age_s\":%u}",
            i ? "," : "", (unsigned long long)r->sequence, (unsigned long)r->source, (unsigned long long)r->utc_end_us,
            (unsigned long long)r->raw.us, (unsigned long)r->raw.fraction_q19, (unsigned long long)r->awake_delta_us,
            (unsigned long long)r->utc_delta_us, (unsigned long long)r->combined_uncertainty_us, r->flags,
            r->temperature_centi_c, r->battery_mv, r->temp_age_s, r->battery_age_s);
        if (n < 0 || (size_t)n >= sizeof(buf) || httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t history_handler(httpd_req_t *req)
{
    char token[33];
    if (httpd_req_get_hdr_value_str(req, "X-Passport-Session", token, sizeof(token)) != ESP_OK || strlen(token) != 32)
        return error_response(req, "403 Forbidden", "Refresh the device page");
    unsigned difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= (unsigned char)token[i] ^ (unsigned char)s_session[i];
    if (difference) return error_response(req, "403 Forbidden", "Refresh the device page");
    bool unlocked;
    portENTER_CRITICAL(&s_snapshot_lock);
    unlocked = s_snapshot.unlocked;
    portEXIT_CRITICAL(&s_snapshot_lock);
    if (!unlocked) return error_response(req, "423 Locked", "Unlock the device first");
    uint64_t generation;
    size_t cursor;
    bool has_generation;
    if (!history_query(req, &generation, &cursor, &has_generation))
        return error_response(req, "400 Bad Request", "Invalid history query");
    uint64_t id;
    portENTER_CRITICAL(&s_history_lock);
    if (!s_history_mailbox.enabled || s_history_mailbox.pending) {
        portEXIT_CRITICAL(&s_history_lock);
        return error_response(req, "503 Service Unavailable", "History unavailable or busy");
    }
    id = ++s_history_mailbox.id;
    s_history_mailbox.generation = generation;
    s_history_mailbox.has_generation = has_generation;
    s_history_mailbox.cursor = cursor;
    s_history_mailbox.pending = true;
    s_history_mailbox.complete = false;
    s_history_mailbox.processing = false;
    portEXIT_CRITICAL(&s_history_lock);
    rtc_clock_history_summary_t summary;
    rtc_history_page_t page;
    uint32_t reason = 0;
    unsigned result = 504;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        portENTER_CRITICAL(&s_history_lock);
        bool cancelled = !s_history_mailbox.enabled || s_history_mailbox.id != id;
        bool complete = !cancelled && s_history_mailbox.complete;
        if (complete) {
            summary = s_history_mailbox.summary;
            page = s_history_mailbox.page;
            result = s_history_mailbox.result;
            reason = s_history_mailbox.learning_reason;
        }
        portEXIT_CRITICAL(&s_history_lock);
        if (cancelled) { result = 503; break; }
        if (complete) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    portENTER_CRITICAL(&s_history_lock);
    if (s_history_mailbox.id == id) {
        s_history_mailbox.pending = false;
        ++s_history_mailbox.id;
    }
    portEXIT_CRITICAL(&s_history_lock);
    portENTER_CRITICAL(&s_snapshot_lock);
    unlocked = s_snapshot.unlocked;
    portEXIT_CRITICAL(&s_snapshot_lock);
    if (!unlocked || result == 423) return error_response(req, "423 Locked", "Unlock the device first");
    if (result == 409) return error_response(req, "409 Conflict", "History generation changed; refresh");
    if (result == 504) return error_response(req, "504 Gateway Timeout", "History owner timed out");
    if (result != 200) return error_response(req, "503 Service Unavailable", "History not ready or cancelled");
    return history_send(req, &summary, &page, reason);
}

static esp_err_t error_response(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
    return httpd_resp_send(req, s_web_page_html, s_web_page_html_len);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    uint64_t time;
    uint8_t count, brightness;
    bool sound, unlocked, time_set;
    rtc_clock_diagnostics_t clock_evidence;
    command_result_t results[4];
    portENTER_CRITICAL(&s_snapshot_lock);
    time = s_snapshot.time;
    count = s_snapshot.count;
    brightness = s_snapshot.brightness;
    sound = s_snapshot.sound;
    unlocked = s_snapshot.unlocked;
    time_set = s_snapshot.time_set;
    clock_evidence = s_snapshot.clock;
    memcpy(results, s_snapshot.results, sizeof(results));
    portEXIT_CRITICAL(&s_snapshot_lock);
    const int64_t before = esp_timer_get_time();
    struct timeval wall;
    const bool got_time = gettimeofday(&wall, NULL) == 0;
    const int64_t after = esp_timer_get_time();
    const bool ordered = before >= 0 && after >= before;
    const int64_t window = ordered ? after - before : 0;
    const bool sample_valid = time_set && got_time && ordered && window <= 1000;
    const int64_t time_us = sample_valid ? (int64_t)wall.tv_sec * 1000000 + wall.tv_usec : 0;
    const int64_t sample_hrt = ordered ? before + window / 2 : 0;
    const time_probe_t probe = time_probe_record(sample_hrt, window, ordered);
    char buf[1792];
    int len = snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\",\"ssid\":\"%s\",\"session\":\"%s\",\"time\":%llu,\"accounts\":%u,\"brightness\":%u,\"sound\":%s,\"unlocked\":%s,\"heap\":%u,\"largest_block\":%u,\"results\":[{\"id\":%lu,\"ok\":%s},{\"id\":%lu,\"ok\":%s},{\"id\":%lu,\"ok\":%s},{\"id\":%lu,\"ok\":%s}],"
        "\"time_set\":%s,\"time_us\":%lld,\"time_sample_hrt_us\":%lld,\"time_sample_window_us\":%lld,\"time_sample_valid\":%s,"
        "\"time_probe\":{\"id\":%lu,\"hrt_us\":%lld,\"window_us\":%lld,\"valid\":%s},"
        "\"rtc\":{\"policy\":\"selected_source_ratio\",\"restored\":%s,\"reconciled_elapsed_us\":%llu,\"sdk_sample_valid\":%s,\"sdk_correction_us\":%lld,\"period_q19\":%lu,\"samples\":%lu,\"pre_tail_gap_upper_us\":%lu,\"fault\":%lu,\"maintenance_blocked\":%s,\"model_qualified\":%s,\"model_observations\":%lu,\"model_rms_millippm\":%lu,"
        "\"source\":%lu,\"ppm\":%ld,\"ppm_qualified\":%s,\"corrected_elapsed_us\":%llu,\"raw_chain_us\":%llu,\"corrected_chain_us\":%llu,\"awake_total_us\":%llu,\"maintenance_count\":%lu,\"learning_reason\":%lu,\"reference_status\":%u}}",
        s_identity.device_id, s_identity.ssid, s_session, (unsigned long long)time, count, brightness,
        sound ? "true" : "false", unlocked ? "true" : "false",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)results[0].id, results[0].ok ? "true" : "false",
        (unsigned long)results[1].id, results[1].ok ? "true" : "false",
        (unsigned long)results[2].id, results[2].ok ? "true" : "false",
        (unsigned long)results[3].id, results[3].ok ? "true" : "false",
        time_set ? "true" : "false", (long long)time_us, (long long)sample_hrt, (long long)window,
        sample_valid ? "true" : "false", (unsigned long)probe.id,
        (long long)probe.hrt_us, (long long)probe.window_us, probe.valid ? "true" : "false",
        clock_evidence.utc_restored ? "true" : "false",
        (unsigned long long)clock_evidence.reconciled_elapsed_us,
        clock_evidence.sdk_sample_valid ? "true" : "false", (long long)clock_evidence.sdk_correction_us,
        (unsigned long)clock_evidence.period_q19, (unsigned long)clock_evidence.samples,
        (unsigned long)clock_evidence.pre_tail_gap_upper_us, (unsigned long)clock_evidence.fault,
        clock_evidence.maintenance_blocked ? "true" : "false", clock_evidence.model_qualified ? "true" : "false",
        (unsigned long)clock_evidence.model_observations, (unsigned long)clock_evidence.model_rms_millippm,
        (unsigned long)clock_evidence.source, (long)clock_evidence.ppm,
        clock_evidence.ppm_qualified ? "true" : "false", (unsigned long long)clock_evidence.corrected_elapsed_us,
        (unsigned long long)clock_evidence.raw_chain_us, (unsigned long long)clock_evidence.corrected_chain_us,
        (unsigned long long)clock_evidence.awake_total_us, (unsigned long)clock_evidence.maintenance_count,
        (unsigned long)clock_evidence.learning_reason, (unsigned)clock_evidence.reference_status);
    if (len < 0 || (size_t)len >= sizeof(buf)) return httpd_resp_send_500(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* Handler-entry/response timestamps include transport scheduling uncertainty. */
static esp_err_t time_probe_handler(httpd_req_t *req)
{
    const int64_t received = esp_timer_get_time();
    char token[33];
    if (httpd_req_get_hdr_value_str(req, "X-Passport-Session", token, sizeof(token)) != ESP_OK || strlen(token) != 32)
        return error_response(req, "403 Forbidden", "Refresh the device page");
    unsigned difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= (unsigned char)token[i] ^ (unsigned char)s_session[i];
    if (difference) return error_response(req, "403 Forbidden", "Refresh the device page");
    portENTER_CRITICAL(&s_snapshot_lock);
    const bool unlocked = s_snapshot.unlocked;
    portEXIT_CRITICAL(&s_snapshot_lock);
    if (!unlocked) return error_response(req, "423 Locked", "Unlock the device first");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    const int64_t sent = esp_timer_get_time();
    const auth_time_sync_probe_t p = auth_time_sync_record(&s_precise_time_probes, received, sent);
    char response[192];
    const int n = snprintf(response, sizeof response,
        "{\"version\":2,\"id\":%lu,\"received_us\":%lld,\"sent_us\":%lld,\"valid\":%s}",
        (unsigned long)p.id, (long long)p.received_us, (long long)p.sent_us, p.valid ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof response) return httpd_resp_send_500(req);
    return httpd_resp_send(req, response, n);
}

static size_t json_escape(const char *src, char *dst, size_t capacity)
{
    size_t n = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        size_t need = c < 32 ? 6 : (c == '"' || c == '\\' ? 2 : 1);
        if (need >= capacity - n) return 0;
        if (c < 32) {
            memcpy(dst + n, "\\u00", 4);
            n += 4;
            dst[n++] = hex[c >> 4];
            dst[n++] = hex[c & 15];
        } else {
            if (need == 2) dst[n++] = '\\';
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
    return n;
}

static esp_err_t accounts_handler(httpd_req_t *req)
{
    public_account_t accounts[AUTH_MAX_ACCOUNTS];
    uint8_t count;
    uint32_t revision;
    bool unlocked;
    portENTER_CRITICAL(&s_snapshot_lock);
    count = s_snapshot.count;
    revision = s_snapshot.revision;
    unlocked = s_snapshot.unlocked;
    memcpy(accounts, s_snapshot.accounts, sizeof(accounts));
    portEXIT_CRITICAL(&s_snapshot_lock);
    if (!unlocked) return error_response(req, "423 Locked", "Unlock the device first");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char prefix[64];
    int prefix_len = snprintf(prefix, sizeof(prefix), "{\"revision\":%lu,\"accounts\":[", (unsigned long)revision);
    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(prefix) ||
        httpd_resp_send_chunk(req, prefix, prefix_len) != ESP_OK) return ESP_FAIL;
    for (uint8_t i = 0; i < count; i++) {
        char issuer[AUTH_ISSUER_MAX_LEN * 6], account[AUTH_ACCOUNT_MAX_LEN * 6], item[512];
        if (!json_escape(accounts[i].issuer, issuer, sizeof(issuer)) ||
            !json_escape(accounts[i].account, account, sizeof(account))) return ESP_FAIL;
        int len = snprintf(item, sizeof(item), "%s{\"index\":%u,\"issuer\":\"%s\",\"account\":\"%s\",\"digits\":%u,\"period\":%u,\"algorithm\":%u}",
                           i ? "," : "", i, issuer, account, accounts[i].digits, accounts[i].period, accounts[i].algorithm);
        if (len < 0 || (size_t)len >= sizeof(item) || httpd_resp_send_chunk(req, item, len) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool string_field(const cJSON *json, const char *name, char *out, size_t capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= capacity) return false;
    strcpy(out, item->valuestring);
    return true;
}

static bool number_field(const cJSON *json, const char *name, uint64_t max, uint64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0 && item->valuedouble <= (double)max)) return false;
    uint64_t value = (uint64_t)item->valuedouble;
    if ((double)value != item->valuedouble) return false;
    *out = value;
    return true;
}

static void parse_time_reference(const cJSON *json, fido_parsed_cmd_t *cmd)
{
    uint64_t id, utc_us, rtt_us;
    if (!number_field(json, "reference_probe_id", UINT32_MAX, &id) || !id ||
        !number_field(json, "reference_utc_us", 4102444800000000ULL, &utc_us) || utc_us < 1577836800000000ULL ||
        !number_field(json, "reference_rtt_us", 2000000, &rtt_us) || !rtt_us) return;
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(json, "reference_clock_delta_us");
    if (!cJSON_IsNumber(delta) ||
        !(delta->valuedouble >= (double)rtt_us - 100000 && delta->valuedouble <= (double)rtt_us + 100000) ||
        (double)(int64_t)delta->valuedouble != delta->valuedouble) return;
    const time_probe_t *probe = &s_time_probes[(id - 1) % 4];
    if (!probe->valid || probe->id != id || probe->hrt_us < 0 || probe->window_us < 0 || probe->window_us > 1000) return;
    const int64_t received = esp_timer_get_time();
    if (received < probe->hrt_us) return;
    cmd->time_reference_present = true;
    cmd->time_reference_quality = received - probe->hrt_us <= 5000000;
    cmd->time_reference_id = probe->reference_id;
    cmd->time_reference_utc_us = utc_us;
    cmd->time_reference_hrt_us = probe->hrt_us;
    cmd->time_reference_uncertainty_us = 1000000 + (rtt_us + 1) / 2 + ((uint64_t)probe->window_us + 1) / 2;
}

static bool parse_precise_time_reference(const cJSON *json, fido_parsed_cmd_t *cmd)
{
    uint64_t version, id, utc_us, rtt_us, spread_us;
    if (!number_field(json, "reference_version", 2, &version) || version != 2 ||
        !number_field(json, "reference_probe_id", UINT32_MAX, &id) || !id ||
        !number_field(json, "reference_utc_us", UINT64_C(4102444800000000), &utc_us) ||
        !number_field(json, "reference_rtt_us", 500000, &rtt_us) ||
        !number_field(json, "reference_spread_us", 20000, &spread_us)) return false;
    auth_time_sync_reference_t r;
    if (!auth_time_sync_resolve(&s_precise_time_probes, (uint32_t)id, utc_us, rtt_us,
            spread_us, esp_timer_get_time(), &r)) return false;
    cmd->time_reference_present = true;
    cmd->time_reference_quality = true;
    cmd->time_reference_precise = true;
    cmd->time_reference_id = r.reference_id;
    cmd->time_reference_utc_us = r.utc_us;
    cmd->time_reference_hrt_us = r.hrt_us;
    cmd->time_reference_phone_uncertainty_us = r.phone_uncertainty_us;
    cmd->time_reference_transport_uncertainty_us = r.transport_uncertainty_us;
    cmd->time_reference_uncertainty_us = r.phone_uncertainty_us + r.transport_uncertainty_us;
    return true;
}

static bool parse_command_json(const cJSON *json, fido_parsed_cmd_t *cmd)
{
    char name[32];
    if (!cJSON_IsObject(json) || !string_field(json, "command", name, sizeof(name))) return false;
    memset(cmd, 0, sizeof(*cmd));
    if (!strcmp(name, "EDIT_TOTP") || !strcmp(name, "DEL_TOTP")) {
        uint64_t revision;
        if (!number_field(json, "revision", UINT32_MAX, &revision)) return false;
        cmd->account_revision = (uint32_t)revision;
    }
    if (!strcmp(name, "ADD_TOTP") || !strcmp(name, "EDIT_TOTP")) {
        cmd->type = !strcmp(name, "ADD_TOTP") ? FIDO_CMD_ADD_TOTP : FIDO_CMD_EDIT_TOTP;
        uint64_t algorithm, digits, period;
        if (!string_field(json, "issuer", cmd->param1, AUTH_ISSUER_MAX_LEN) || !cmd->param1[0] ||
            !string_field(json, "account", cmd->param2, AUTH_ACCOUNT_MAX_LEN) || !cmd->param2[0] ||
            !string_field(json, "secret", cmd->param3, sizeof(cmd->param3)) ||
            !number_field(json, "algorithm", TOTP_ALGO_STEAM, &algorithm) ||
            !number_field(json, "digits", 8, &digits) || !number_field(json, "period", 255, &period) || !period) return false;
        if (algorithm == TOTP_ALGO_STEAM ? (digits != 5 || period != 30) : (digits < 6)) return false;
        if (cmd->type == FIDO_CMD_ADD_TOTP && !cmd->param3[0]) return false;
        if (cmd->param3[0]) {
            uint8_t key[80];
            int len = totp_base32_decode(cmd->param3, key, sizeof(key));
            volatile uint8_t *p = key;
            for (size_t i = 0; i < sizeof(key); i++) p[i] = 0;
            if (len <= 0) return false;
        }
        cmd->otp_algorithm = (uint8_t)algorithm;
        cmd->otp_digits = (uint8_t)digits;
        cmd->otp_period = (uint8_t)period;
        return cmd->type != FIDO_CMD_EDIT_TOTP || number_field(json, "index", AUTH_MAX_ACCOUNTS - 1, &cmd->num_param);
    }
    if (!strcmp(name, "DEL_TOTP")) {
        cmd->type = FIDO_CMD_DEL_TOTP;
        return number_field(json, "index", AUTH_MAX_ACCOUNTS - 1, &cmd->num_param);
    }
    if (!strcmp(name, "SYNC_TIME")) {
        cmd->type = FIDO_CMD_SYNC_TIME;
        if (!number_field(json, "value", 4102444800ULL, &cmd->num_param)) return false;
        if (cJSON_GetObjectItemCaseSensitive(json, "reference_version"))
            return parse_precise_time_reference(json, cmd);
        parse_time_reference(json, cmd);
        return true;
    }
    if (!strcmp(name, "SET_BRIGHTNESS")) {
        cmd->type = FIDO_CMD_SET_BRIGHTNESS;
        return number_field(json, "value", 100, &cmd->num_param) && cmd->num_param >= 10;
    }
    if (!strcmp(name, "SET_SOUND")) {
        cmd->type = FIDO_CMD_SET_SOUND;
        return number_field(json, "value", 1, &cmd->num_param);
    }
    if (!strcmp(name, "WIPE_ALL")) {
        cmd->type = FIDO_CMD_RESET;
        return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "confirm"));
    }
    return false;
}

static esp_err_t command_handler(httpd_req_t *req)
{
    char token[33], content_type[48], origin[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/json") != 0) return error_response(req, "415 Unsupported Media Type", "Use application/json");
    size_t origin_len = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_len && (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
                      strcmp(origin, "http://192.168.4.1") != 0)) return error_response(req, "403 Forbidden", "Origin rejected");
    if (httpd_req_get_hdr_value_str(req, "X-Passport-Session", token, sizeof(token)) != ESP_OK ||
        strlen(token) != 32) return error_response(req, "403 Forbidden", "Refresh the device page");
    unsigned difference = 0;
    for (size_t i = 0; i < 32; i++) difference |= (unsigned char)token[i] ^ (unsigned char)s_session[i];
    if (difference) return error_response(req, "403 Forbidden", "Refresh the device page");
    bool unlocked;
    portENTER_CRITICAL(&s_snapshot_lock);
    unlocked = s_snapshot.unlocked;
    portEXIT_CRITICAL(&s_snapshot_lock);
    if (!unlocked) return error_response(req, "423 Locked", "Unlock the device first");
    char body[768];
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) return error_response(req, "413 Content Too Large", "Request is too large");
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return error_response(req, "408 Request Timeout", "Request was incomplete");
        received += (size_t)n;
    }
    body[received] = '\0';
    bool quoted = false, escaped = false, flat = true;
    unsigned depth = 0;
    for (size_t i = 0; i < received; i++) {
        char c = body[i];
        if (!c) flat = false;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{') { if (++depth > 1) flat = false; }
        else if (c == '}') { if (!depth) flat = false; else depth--; }
        else if (c == '[' || c == ']') flat = false;
    }
    const char *parse_end = NULL;
    cJSON *json = flat && !quoted && depth == 0 ?
                  cJSON_ParseWithLengthOpts(body, received + 1, &parse_end, true) : NULL;
    fido_parsed_cmd_t cmd;
    bool valid = json && parse_command_json(json, &cmd);
    cJSON_Delete(json);
    volatile char *p = body;
    for (size_t i = 0; i < sizeof(body); i++) p[i] = 0;
    if (!valid) return error_response(req, "400 Bad Request", "Invalid command or account fields");
    if (++s_request_id == 0) ++s_request_id;
    cmd.request_id = s_request_id;
    if (cmd.type == FIDO_CMD_SYNC_TIME && cmd.time_reference_present) {
        uint32_t probe_id = (uint32_t)cmd.time_reference_id;
        if (cmd.time_reference_precise) (void)auth_time_sync_consume(&s_precise_time_probes, probe_id);
        else s_time_probes[(probe_id - 1) % 4].valid = false;
    }
    bool queued = s_cmd_cb && s_cmd_cb(&cmd);
    uint32_t id = cmd.request_id;
    volatile uint8_t *q = (volatile uint8_t *)&cmd;
    for (size_t i = 0; i < sizeof(cmd); i++) q[i] = 0;
    if (!queued) return error_response(req, "503 Service Unavailable", "Device is busy or locked; refresh before retrying");
    char response[64];
    int len = snprintf(response, sizeof(response), "{\"queued\":true,\"id\":%lu}", (unsigned long)id);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, len);
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    if (!strncmp(req->uri, "/api/", 5)) return error_response(req, "404 Not Found", "Unknown API");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    return redirect_handler(req);
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    s_dns_ok = sock >= 0 && setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
               bind(sock, (struct sockaddr *)&address, sizeof(address)) == 0;
    xSemaphoreGive(s_dns_ready);
    if (s_dns_ok) ESP_LOGI(TAG, "Captive DNS ready");
    while (s_dns_ok && s_dns_run) {
        uint8_t packet[160];
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, packet, 128, 0, (struct sockaddr *)&client, &client_len);
        if (len < 17 || packet[4] != 0 || packet[5] != 1 || (packet[2] & 0x80)) continue;
        size_t end = 12;
        while (end < (size_t)len && packet[end]) {
            unsigned label = packet[end++];
            if (label > 63 || label > (size_t)len - end) { end = (size_t)len; break; }
            end += label;
        }
        if (end + 5 > (size_t)len) continue;
        end++;
        bool answer = packet[end] == 0 && packet[end + 1] == 1 && packet[end + 2] == 0 && packet[end + 3] == 1;
        end += 4;
        packet[2] = 0x81;
        packet[3] = 0x80;
        memset(packet + 6, 0, 6);
        if (answer) {
            static const uint8_t response[] = {0xc0,0x0c,0,1,0,1,0,0,0,30,0,4,192,168,4,1};
            packet[7] = 1;
            memcpy(packet + end, response, sizeof(response));
            end += sizeof(response);
        }
        sendto(sock, packet, end, 0, (struct sockaddr *)&client, client_len);
    }
    if (sock >= 0) close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

bool auth_wifi_ap_stop(void)
{
    history_mailbox_reset(false);
    s_dns_run = false;
    for (unsigned i = 0; i < 50 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_dns_task) return false;
    if (s_server) {
        if (httpd_stop(s_server) != ESP_OK) return false;
        s_server = NULL;
    }
    if (s_wifi_started) {
        if (esp_wifi_stop() != ESP_OK) return false;
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        if (esp_wifi_deinit() != ESP_OK) return false;
        s_wifi_initialized = false;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    memset(s_session, 0, sizeof(s_session));
    memset(s_time_probes, 0, sizeof(s_time_probes));
    auth_time_sync_reset(&s_precise_time_probes, 0);
    s_cmd_cb = NULL;
    return true;
}

bool auth_wifi_ap_start(auth_config_t *cfg, auth_wifi_cmd_cb_t callback)
{
    (void)cfg;
    if (s_wifi_started && s_server && s_dns_task && s_dns_run) return true;
    if (!auth_wifi_ap_stop() || !auth_wifi_init_identity()) return false;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 48 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16 * 1024) return false;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.static_rx_buf_num = 2;
    init.dynamic_rx_buf_num = 4;
    init.tx_buf_type = 1;
    init.dynamic_tx_buf_num = 4;
    init.mgmt_sbuf_num = 6;
    init.csi_enable = 0;
    init.ampdu_rx_enable = 0;
    init.ampdu_tx_enable = 0;
    init.nvs_enable = 0;
    if (esp_wifi_init(&init) != ESP_OK) return false;
    s_wifi_initialized = true;
    esp_netif_config_t netif = ESP_NETIF_DEFAULT_WIFI_AP();
    s_ap_netif = esp_netif_new(&netif);
    if (!s_ap_netif || esp_netif_attach_wifi_ap(s_ap_netif) != ESP_OK || esp_wifi_set_default_wifi_ap_handlers() != ESP_OK) goto fail;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) goto fail;
    wifi_config_t config = {.ap = {.channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK, .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP}};
    memcpy(config.ap.ssid, s_identity.ssid, strlen(s_identity.ssid));
    config.ap.ssid_len = (uint8_t)strlen(s_identity.ssid);
    memcpy(config.ap.password, s_identity.password, strlen(s_identity.password));
    if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK ||
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20) != ESP_OK || esp_wifi_start() != ESP_OK) goto fail;
    s_wifi_started = true;
    if (esp_wifi_set_max_tx_power(44) != ESP_OK) goto fail;
    uint8_t random[16];
    static const char hex[] = "0123456789ABCDEF";
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) {
        s_session[i * 2] = hex[random[i] >> 4];
        s_session[i * 2 + 1] = hex[random[i] & 15];
    }
    time_probe_new_session();
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 6144;
    http.max_open_sockets = 2;
    http.max_uri_handlers = 9;
    http.backlog_conn = 2;
    http.lru_purge_enable = true;
    http.recv_wait_timeout = 3;
    http.send_wait_timeout = 3;
    history_mailbox_reset(true);
    if (httpd_start(&s_server, &http) != ESP_OK) { s_server = NULL; goto fail; }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/index.html", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/time-probe", .method = HTTP_GET, .handler = time_probe_handler},
        {.uri = "/api/rtc-history", .method = HTTP_GET, .handler = history_handler},
        {.uri = "/api/accounts", .method = HTTP_GET, .handler = accounts_handler},
        {.uri = "/api/command", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = redirect_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_server, &routes[i]) != ESP_OK) goto fail;
    }
    if (httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler) != ESP_OK) goto fail;
    if (!s_dns_ready) s_dns_ready = xSemaphoreCreateBinaryStatic(&s_dns_ready_storage);
    if (!s_dns_ready) goto fail;
    while (xSemaphoreTake(s_dns_ready, 0) == pdTRUE) {}
    s_cmd_cb = callback;
    s_dns_ok = false;
    s_dns_run = true;
    if (xTaskCreate(dns_task, "auth_dns", 2560, NULL, 4, (TaskHandle_t *)&s_dns_task) != pdPASS) goto fail;
    if (xSemaphoreTake(s_dns_ready, pdMS_TO_TICKS(1000)) != pdTRUE || !s_dns_ok) goto fail;
    ESP_LOGI(TAG, "AP ready: %s; free=%u largest=%u", s_identity.ssid,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
fail:
    ESP_LOGE(TAG, "AP initialization failed; releasing resources");
    auth_wifi_ap_stop();
    return false;
}

bool auth_wifi_ap_is_active(void)
{
    return s_wifi_initialized || s_ap_netif || s_wifi_started || s_server || s_dns_task;
}

#else
static bool s_mock_wifi_active;
void auth_wifi_history_service(bool unlocked) { (void)unlocked; }
bool auth_wifi_init_identity(void) { return false; }
void auth_wifi_publish_config(const auth_config_t *cfg, bool unlocked) { (void)cfg; (void)unlocked; }
void auth_wifi_command_result(uint32_t id, bool ok) { (void)id; (void)ok; }
bool auth_wifi_ap_start(auth_config_t *cfg, auth_wifi_cmd_cb_t callback)
{
    (void)cfg;
    (void)callback;
    s_mock_wifi_active = true;
    return true;
}
bool auth_wifi_ap_stop(void) { s_mock_wifi_active = false; return true; }
bool auth_wifi_ap_is_active(void) { return s_mock_wifi_active; }
#endif
