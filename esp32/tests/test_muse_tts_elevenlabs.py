# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Run the real adapter with deterministic HTTP failures and chunked bodies."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ElevenLabsAdapter(unittest.TestCase):
    def test_http_eof_retries_cancellation(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'freertos').mkdir()
            (p / 'fake.h').write_text(r'''
#pragma once
#include <cstdlib>
#include <cstring>
#include <cstdint>
#define ESP_OK 0
#define ESP_ERR_HTTP_EAGAIN 11
#define HTTP_METHOD_POST 1
#define HTTP_ADDR_TYPE_INET 1
#define CONFIG_MUSE_TTS_MODEL "test-model"
#define CONFIG_MUSE_TTS_VOICE_ID "test-voice"
#define CONFIG_MUSE_TTS_API_KEY "test-key"
#define pdMS_TO_TICKS(x) (x)
inline int mode, attempts, reads;
inline int64_t ticks;
inline bool cancel;
inline int64_t esp_timer_get_time() { return ticks+=1000000; }
inline void vTaskDelay(int) {}
inline void heap_caps_free(void *p) { free(p); }
struct esp_http_client_config_t { const char *url; int method,timeout_ms,buffer_size,buffer_size_tx; const char *cert_pem; int addr_type; };
using esp_http_client_handle_t = void *;
inline void *esp_http_client_init(const esp_http_client_config_t *c) {
 if(!strstr(c->url,"/stream?output_format=mp3_44100_128")) abort();
 attempts++; reads=0; return (void *)1;
}
inline int esp_http_client_set_header(void *,const char *,const char *) { return 0; }
inline int esp_http_client_open(void *,int) { return mode==1 && attempts<3 ? -1 : 0; }
inline int esp_http_client_write(void *,const char *,int n) { return n; }
inline int64_t esp_http_client_fetch_headers(void *) { return 0; }
inline int esp_http_client_get_status_code(void *) { return mode==2 ? 401 : 200; }
inline int esp_http_client_read(void *,char *out,int) {
 reads++;
 if(mode==4) { cancel=true; return -1; }
 if(reads==1) { memcpy(out,"abc",3); return 3; }
 if(mode==3) return -1;
 return 0;
}
inline bool esp_http_client_is_complete_data_received(void *) { return mode!=3 && reads>1; }
inline int esp_http_client_cleanup(void *) { return 0; }
''')
            for name in ['sdkconfig.h', 'esp_http_client.h', 'esp_timer.h', 'esp_heap_caps.h',
                         'freertos/FreeRTOS.h', 'freertos/task.h']:
                (p / name).write_text('#include "fake.h"\n')
            (p / 'test.cpp').write_text(r'''
#include "fake.h"
#include "muse_tts_provider.h"
#include <cassert>
#include <string>
static std::string audio;
static bool accept=true;
static bool format(void *,muse_tts_format f) { return accept && f==muse_tts_format::mp3; }
static bool write(void *,const uint8_t *p,size_t n) { audio.append((const char *)p,n); return !cancel; }
static bool stopped(void *) { return cancel; }
int main() {
 muse_tts_sink sink{nullptr,format,write,stopped};
 for(int m=0;m<5;m++) {
  mode=m; attempts=reads=0; ticks=0; cancel=false; audio.clear();
  auto r=muse_tts_elevenlabs("hello",sink);
  if(m<2) { assert(r==muse_tts_result::ok && audio=="abc"); assert(attempts==(m==1?3:1)); }
  if(m==2) assert(r==muse_tts_result::failed && audio.empty() && attempts==3);
  if(m==3) assert(r==muse_tts_result::failed && audio=="abc" && attempts==1);
  if(m==4) assert(r==muse_tts_result::cancelled && attempts==1);
 }
 cancel=false; accept=false; attempts=0;
 assert(muse_tts_elevenlabs("hello",sink)==muse_tts_result::failed && attempts==0);
}
''')
            json_dir = Path(os.environ.get('CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'))
            subprocess.run([os.environ.get('CC', 'cc'), '-c', str(json_dir / 'cJSON.c'),
                            '-o', str(p / 'json.o')], check=True)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-I' + str(p),
                            '-I' + str(ROOT / 'components/muse'), '-I' + str(json_dir),
                            str(ROOT / 'components/muse/muse_tts_elevenlabs.cpp'),
                            str(p / 'test.cpp'), str(p / 'json.o'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
