/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * C surface over the ESP32Drop AirDrop receiver: start/stop AWDL and the TLS listener,
 * hand over received files and report status. No file system or display access; the
 * caller decides where files go.
 *
 * Adapted for CrossMux from the read_pico_firmware port of ESP32Drop (receive-only,
 * upstream 0BSD -- see LICENSE and README.md).
 *
 * Frozen: takes the radio exclusively and cannot run beside ordinary WiFi; receive-only,
 * "Everyone" mode. The library code underneath is upstream 0BSD.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// File type, sniffed from content -- never from the name.
typedef enum {
    ESP32DROP_FILE_OTHER = 0,   ///< Anything else
    ESP32DROP_FILE_JPEG,        ///< JPEG
    ESP32DROP_FILE_PNG,         ///< PNG
    ESP32DROP_FILE_GIF,         ///< GIF
    ESP32DROP_FILE_HEIC,        ///< HEIC
    ESP32DROP_FILE_PDF,         ///< PDF
    ESP32DROP_FILE_ZIP,         ///< ZIP container, EPUB included
    ESP32DROP_FILE_APPLEDOUBLE, ///< macOS metadata sidecar
} esp32drop_file_type_t;

/// One received file. Valid only for the duration of the callback: copy what you keep.
typedef struct {
    const uint8_t* data;        ///< File bytes, in PSRAM
    uint32_t len;
    const char* name;           ///< UTF-8 basename
    esp32drop_file_type_t type;
    uint32_t index;             ///< Position and count within this transfer
    uint32_t count;
} esp32drop_file_t;

/// Return false if the file could not be used; that is counted in file_err.
typedef bool (*esp32drop_file_cb_t)(void* ctx, const esp32drop_file_t* file);

typedef struct {
    bool running;               ///< Started and listening
    bool rx_active;             ///< A transfer is arriving
    uint32_t rx_bytes;          ///< Compressed bytes received so far
    uint32_t rx_max;            ///< Receive ceiling in force; 0 before the first transfer
    /// Cumulative: discovered, asked, uploaded, delivered, refused.
    uint32_t discover_ok, ask_ok, upload_ok, files_ok, file_err;
    bool awdl_locked;           ///< AWDL is locked to the mesh timing
    char errmsg[40];            ///< Description of the latest failure
} esp32drop_status_t;

/// Start AWDL and the AirDrop listener; `name` is the UTF-8 label on the sender's share sheet.
/// `max_receive` bounds the decoded archive in bytes, 0 for the 6 MiB library default.
/// The callback runs only on the task that calls esp32drop_poll().
esp_err_t esp32drop_start(const char* name, uint32_t max_receive, esp32drop_file_cb_t cb, void* ctx);
/// Stop listening and release the radio; returns at once when not started.
/// Must be called from the same task as esp32drop_start().
esp_err_t esp32drop_stop(void);
/// Deliver finished files and forward library logs to ESP_LOG; cheap when idle.
void esp32drop_poll(void);
/// Read a status snapshot.
void esp32drop_get_status(esp32drop_status_t* out);

#ifdef __cplusplus
}
#endif
