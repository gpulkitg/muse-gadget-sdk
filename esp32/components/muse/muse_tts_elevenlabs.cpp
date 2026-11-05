// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "muse_tts_provider.h"
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
static const char TTS_CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n";


muse_tts_result muse_tts_elevenlabs(const char *text, const muse_tts_sink &sink)
{
    if (!sink.format(sink.context, muse_tts_format::mp3)) return muse_tts_result::failed;
    cJSON *body = cJSON_CreateObject();
    if (!body) return muse_tts_result::failed;
    bool valid = cJSON_AddStringToObject(body, "text", text) &&
                 cJSON_AddStringToObject(body, "model_id", CONFIG_MUSE_TTS_MODEL);
    char *json = valid ? cJSON_PrintUnformatted(body) : nullptr;
    cJSON_Delete(body);
    if (!json) return muse_tts_result::failed;
    char url[256];
    snprintf(url, sizeof(url), "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=mp3_44100_128", CONFIG_MUSE_TTS_VOICE_ID);
    bool committed = false, success = false;
    for (int attempt = 0; attempt < 3 && !sink.cancelled(sink.context); ++attempt) {
        esp_http_client_config_t cfg{};
        cfg.url = url;
        cfg.method = HTTP_METHOD_POST;
        cfg.timeout_ms = 3000;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 4096;
        cfg.cert_pem = TTS_CA_PEM;
        cfg.addr_type = HTTP_ADDR_TYPE_INET;
        auto client = esp_http_client_init(&cfg);
        if (!client) break;
        esp_http_client_set_header(client, "xi-api-key", CONFIG_MUSE_TTS_API_KEY);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Accept", "audio/mpeg");
        bool ok = esp_http_client_open(client, strlen(json)) == ESP_OK;
        size_t off = 0;
        while (ok && off < strlen(json) && !sink.cancelled(sink.context)) {
            int n = esp_http_client_write(client, json + off, strlen(json) - off);
            if (n <= 0) ok = false; else off += n;
        }
        if (ok && !sink.cancelled(sink.context)) {
            int64_t start = esp_timer_get_time();
            int64_t headers;
            do {
                headers = esp_http_client_fetch_headers(client);
                if (headers != -ESP_ERR_HTTP_EAGAIN) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            } while (!sink.cancelled(sink.context) && esp_timer_get_time() - start < 30000000LL);
            ok = headers >= 0 && esp_http_client_get_status_code(client) == 200;
        }
        uint8_t bytes[4096];
        int64_t last = esp_timer_get_time();
        while (ok && !sink.cancelled(sink.context)) {
            int n = esp_http_client_read(client, reinterpret_cast<char *>(bytes), sizeof(bytes));
            if (n > 0) {
                committed = true; // Never retry once any bytes may have reached playback.
                if (!sink.write(sink.context, bytes, n)) { ok = false; break; }
                last = esp_timer_get_time(); // Backpressure time is not network inactivity.
            } else if (esp_http_client_is_complete_data_received(client)) {
                success = committed;
                break;
            } else if ((n < 0 && n != -ESP_ERR_HTTP_EAGAIN) || esp_timer_get_time() - last >= 30000000LL) {
                ok = false;
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        esp_http_client_cleanup(client);
        if (success || committed) break;
        for (int ms = 0; ms < (500 << attempt) && !sink.cancelled(sink.context); ms += 20)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    heap_caps_free(json);
    return sink.cancelled(sink.context) ? muse_tts_result::cancelled
         : success ? muse_tts_result::ok : muse_tts_result::failed;
}
