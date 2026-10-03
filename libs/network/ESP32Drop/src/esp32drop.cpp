/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements include/esp32drop.h by wrapping the upstream AWDL/AirDrop port layer in a C
 * surface and owning the start/stop order. Adapted for CrossMux from the read_pico_firmware
 * port of ESP32Drop (receive-only, upstream 0BSD -- see LICENSE and README.md).
 *
 * Frozen: start AWDL before the listener and stop in reverse; a failed stop frees nothing.
 */
#include <string.h>

#include <Arduino.h>          /* millis() */

#include "esp32drop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ESP32AWDL.h"
#include "airdrop/port/ad_port_esp32.h"

static const char* TAG = "esp32drop";

static bool s_running;
static uint32_t s_stat_ms;
static esp32drop_file_cb_t s_cb;
static void* s_cb_ctx;

// Map the upstream AdFile to the public struct; the type enum values correspond one to one.
static bool on_file(void* ctx, const struct AdFile* f) {
    (void)ctx;
    if (!s_cb) return false;
    esp32drop_file_t file = {
        .data = f->data, .len = f->len, .name = f->name,
        .type = (esp32drop_file_type_t)f->type, .index = f->index, .count = f->count,
    };
    return s_cb(s_cb_ctx, &file);
}

// The library never prints; it stages lines. Forward them to ESP_LOG on the caller's task.
static void drain_logs(void) {
    char line[200];
    for (int i = 0; i < 16; ++i) {
        size_t n = ad_diag_read_line(line, sizeof(line));
        if (!n) break;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        ESP_LOGI(TAG, "%s", line);
    }
}

extern "C" esp_err_t esp32drop_start(const char* name, uint32_t max_receive, esp32drop_file_cb_t cb, void* ctx) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_cb = cb;
    s_cb_ctx = ctx;
    // The driver warns on every frame from a non-associated station; printing them slows the frame path.
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_err_t err = awdl_begin();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "awdl_begin: %s", esp_err_to_name(err));
        awdl_end();
        s_cb = nullptr;
        s_cb_ctx = nullptr;
        return err;
    }
    ad_on_file(on_file, nullptr);
    int rc = ad_begin(name, max_receive);
    if (rc != 0) {
        ESP_LOGW(TAG, "ad_begin: %d", rc);
        ad_end();
        awdl_end();
        esp_log_level_set("wifi", ESP_LOG_INFO);
        s_cb = nullptr;
        s_cb_ctx = nullptr;
        return rc == -2 ? ESP_ERR_INVALID_ARG : ESP_ERR_NO_MEM;
    }
    s_running = true;
    s_stat_ms = millis();
    ESP_LOGI(TAG, "listening as \"%s\", internal heap %u B", name,
             (unsigned)esp_get_free_internal_heap_size());
    return ESP_OK;
}

extern "C" esp_err_t esp32drop_stop(void) {
    if (!s_running) return ESP_OK;
    if (ad_end() != 0) {
        ESP_LOGE(TAG, "listener did not stop");
        return ESP_ERR_TIMEOUT;
    }
    drain_logs();
    esp_err_t err = awdl_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "awdl_end: %s", esp_err_to_name(err));
        return err;
    }
    s_running = false;
    esp_log_level_set("wifi", ESP_LOG_INFO);
    ad_on_file(nullptr, nullptr);
    s_cb = nullptr;
    s_cb_ctx = nullptr;
    return ESP_OK;
}

// One link-counter line every 2 s, for frames the driver refuses or the queue drops.
static void log_stat(void) {
    uint32_t now = millis();
    if (now - s_stat_ms < 2000) return;
    s_stat_ms = now;
    static struct AwdlDiag d;
    awdl_diag_read(&d, false);
    struct AwdlStatus aw;
    awdl_status_read(&aw);
    struct AdStatus st;
    memset(&st, 0, sizeof(st));
    ad_stats_read(&st);
    ESP_LOGI(TAG, "STAT lock=%d win=%lu/%lu ntx=%lu sent=%lu qdrop=%lu terr=%lu last=0x%lx ucast=%lu mcast=%lu "
             "qmax=%u mif=%lu/%lu mlast=0x%lx rx=%d/%lu heap=%u",
             aw.locked, (unsigned long)aw.win_served, (unsigned long)aw.win_total,
             (unsigned long)d.netif_tx, (unsigned long)d.netif_tx_sent, (unsigned long)d.netif_txq_drop,
             (unsigned long)d.netif_tx_err, (unsigned long)d.netif_tx_last_err,
             (unsigned long)d.netif_tx_err_ucast, (unsigned long)d.netif_tx_err_mcast,
             (unsigned)d.txq_depth_max, (unsigned long)d.mif_tx, (unsigned long)d.mif_tx_err,
             (unsigned long)d.mif_last_err, st.rx_active, (unsigned long)st.rx_bytes,
             (unsigned)esp_get_free_internal_heap_size());
}

extern "C" void esp32drop_poll(void) {
    if (!s_running) return;
    ad_poll();
    drain_logs();
    log_stat();
}

extern "C" void esp32drop_get_status(esp32drop_status_t* out) {
    memset(out, 0, sizeof(*out));
    out->running = s_running;
    if (!s_running) return;
    struct AdStatus st;
    memset(&st, 0, sizeof(st));
    ad_stats_read(&st);
    out->rx_active = st.rx_active;
    out->rx_bytes = st.rx_bytes;
    out->rx_max = st.rx_max;
    out->discover_ok = st.discover_ok;
    out->ask_ok = st.ask_ok;
    out->upload_ok = st.upload_ok;
    out->files_ok = st.files_ok;
    out->file_err = st.file_err;
    strlcpy(out->errmsg, st.file_errmsg, sizeof(out->errmsg));
    struct AwdlStatus aw;
    memset(&aw, 0, sizeof(aw));
    awdl_status_read(&aw);
    out->awdl_locked = aw.locked;
}
