#include "local_web.h"
#include <esp_http_server.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_matter_controller_client.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include <cJSON.h>
#include <esp_matter.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_cluster_command.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <esp_system.h>
#include <esp_random.h>
#include <platform/CHIPDeviceLayer.h>
#include "web_page.h"

using namespace esp_matter::controller;
static httpd_handle_t server;
static std::atomic<bool> ready{false};
static bool pairing = false;
static bool pairing_mode = false;
static bool rebooting = false;
static unsigned in_flight = 0;
static unsigned confirmed = 0;
static unsigned failed = 0;
static bool pase_connected = false;
static int64_t pairing_started = 0;
static char message[160] = "Controller starting";
struct Bulb { uint32_t id; uint16_t endpoint; char name[40]; };
static Bulb bulbs[16]{};
static size_t bulb_count = 0;
static Bulb pending{};
// Separate, versioned storage keeps the existing paired-bulb layout unchanged.
struct PresetEntry { uint32_t bulb; char color[8]; uint16_t kelvin; uint8_t brightness; uint8_t white; uint16_t speed; uint8_t range; uint8_t flicker; };
struct Preset { char name[32]; uint8_t count; PresetEntry entries[16]; };
static Preset presets[16]{};
static cJSON *preset_list() {
    cJSON *a = cJSON_CreateArray();
    for (size_t i = 0; i < 16; ++i) {
        const auto &p = presets[i];
        if (!p.name[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", i + 1);
        cJSON_AddStringToObject(o, "name", p.name);
        cJSON *entries = cJSON_AddArrayToObject(o, "lights");
        for (unsigned j = 0; j < p.count; ++j) {
            const auto &e = p.entries[j];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", e.bulb);
            cJSON_AddStringToObject(item, "color", e.color);
            cJSON_AddStringToObject(item, "mode", e.white ? "white" : "color");
            cJSON_AddNumberToObject(item, "kelvin", e.kelvin);
            cJSON_AddNumberToObject(item, "brightness", e.brightness);
            cJSON_AddBoolToObject(item, "flicker", e.flicker);
            cJSON_AddNumberToObject(item, "flicker_speed", e.speed);
            cJSON_AddNumberToObject(item, "flicker_range", e.range);
            cJSON_AddItemToArray(entries, item);
        }
        cJSON_AddItemToArray(a, o);
    }
    return a;
}
struct Flicker { bool enabled; int brightness = 70; int kelvin = 2700; int speed = 45; int range = 3; int64_t next = 0; };
static Flicker effects[16];
static esp_err_t send_light(Bulb &bulb, uint32_t cluster, uint32_t command, const char *payload);
static void flicker_tick(chip::System::Layer *, void *);

// Shared state is accessed under the Matter stack lock, including HTTP handlers.
static esp_err_t save_bulbs() {
    nvs_handle_t n;
    esp_err_t err = nvs_open("local_lights", NVS_READWRITE, &n);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(n, "bulbs", bulbs, bulb_count * sizeof(Bulb));
    if (err == ESP_OK) err = nvs_commit(n);
    nvs_close(n);
    return err;
}
static void pair_ok(chip::ScopedNodeId peer) {
    if (pairing && peer.GetNodeId() == pending.id && bulb_count < 16) {
        bulbs[bulb_count++] = pending; save_bulbs();
    }
    pairing = false;
    snprintf(message, sizeof(message), "Bulb paired. Ready to control.");
}
static void pair_fail(chip::ScopedNodeId, CHIP_ERROR error, chip::Controller::CommissioningStage,
                      std::optional<chip::Credentials::AttestationVerificationResult>) {
    pairing = false;
    snprintf(message, sizeof(message), "Pairing failed: %s", chip::ErrorStr(error));
}
static void pase(CHIP_ERROR error) {
    if (error == CHIP_NO_ERROR) pase_connected = true;
    if (error != CHIP_NO_ERROR) {
        pairing = false;
        snprintf(message, sizeof(message), "Pairing connection failed: %s", chip::ErrorStr(error));
    }
}
static esp_err_t json_reply(httpd_req_t *req, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(req, s); cJSON_free(s); return result;
}
static esp_err_t busy_reply(httpd_req_t *req, const char *message) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message);
}
static cJSON *body(httpd_req_t *req, int limit = 512) {
    char marker[16];
    if (httpd_req_get_hdr_value_str(req, "X-Local-Control", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "1") || req->content_len <= 0 || req->content_len > limit) return nullptr;
    char *buf = static_cast<char *>(malloc(req->content_len + 1));
    if (!buf) return nullptr;
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n <= 0) { free(buf); return nullptr; }
        total += n;
    }
    buf[total] = 0; cJSON *result = cJSON_Parse(buf); free(buf); return result;
}
static int number(cJSON *o, const char *key, int fallback) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}
static esp_err_t preset_settings(httpd_req_t *req) {
    cJSON *o = body(req, 4096);
    if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    cJSON *action = cJSON_GetObjectItemCaseSensitive(o, "action");
    bool remove = cJSON_IsString(action) && !strcmp(action->valuestring, "delete");
    bool save = cJSON_IsString(action) && !strcmp(action->valuestring, "save");
    int slot = number(o, "id", 0) - 1;
    bool updating = save && cJSON_GetObjectItemCaseSensitive(o, "id");
    Preset next{};
    bool valid = remove && slot >= 0 && slot < 16;
    if (save) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
        cJSON *lights = cJSON_GetObjectItemCaseSensitive(o, "lights");
        int count = cJSON_GetArraySize(lights);
        valid = cJSON_IsString(name) && name->valuestring[0] && strlen(name->valuestring) < sizeof(next.name) &&
            cJSON_IsArray(lights) && count >= 1 && count <= 16;
        if (valid) {
            snprintf(next.name, sizeof(next.name), "%s", name->valuestring);
            next.count = count;
            for (int i = 0; i < count && valid; ++i) {
                cJSON *item = cJSON_GetArrayItem(lights, i);
                cJSON *color = cJSON_GetObjectItemCaseSensitive(item, "color");
                cJSON *mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
                int id = number(item, "id", -1), kelvin = number(item, "kelvin", -1), brightness = number(item, "brightness", -1);
                int speed = number(item, "flicker_speed", 45), range = number(item, "flicker_range", 3);
                cJSON *flicker = cJSON_GetObjectItemCaseSensitive(item, "flicker");
                valid = (!flicker || cJSON_IsBool(flicker)) && speed >= 1 && speed <= 120 && range >= 1 && range <= 20 &&
                    id > 0 && cJSON_IsString(color) && strlen(color->valuestring) == 7 && color->valuestring[0] == '#' &&
                    strspn(color->valuestring + 1, "0123456789abcdefABCDEF") == 6 &&
                    cJSON_IsString(mode) && (!strcmp(mode->valuestring, "white") || !strcmp(mode->valuestring, "color")) &&
                    kelvin >= 2000 && kelvin <= 6500 && brightness >= 1 && brightness <= 100;
                for (int j = 0; j < i; ++j) if (next.entries[j].bulb == (uint32_t)id) valid = false;
                if (!valid) break;
                auto &e = next.entries[i];
                e.speed = speed; e.range = range; e.flicker = cJSON_IsTrue(flicker);
                e.bulb = id; e.kelvin = kelvin; e.brightness = brightness; e.white = !strcmp(mode->valuestring, "white");
                if (e.flicker) e.white = 1;
                snprintf(e.color, sizeof(e.color), "%s", color->valuestring);
            }
        }
    }
    if (updating && (slot < 0 || slot >= 16)) valid = false;
    cJSON_Delete(o);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter a short preset name and valid light settings");
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    const char *error = nullptr;
    if (rebooting || pairing) error = "Wait until setup is finished";
    if (!error && updating && !presets[slot].name[0]) error = "Preset no longer exists";
    if (!error && save) {
        for (unsigned j = 0; j < next.count; ++j) {
            bool found = false;
            for (size_t i = 0; i < bulb_count; ++i) if (bulbs[i].id == next.entries[j].bulb) found = true;
            if (!found) error = "One of the selected lights no longer exists";
        }
    }
    if (!error && save) {
        // Bound JSON response allocations as well as persistent storage.
        unsigned total = next.count;
        for (const auto &preset : presets) total += preset.count;
        if (updating) total -= presets[slot].count;
        if (total > 64) error = "Preset storage is full; delete an unused preset first";
    }
    if (!error && save) {
        if (!updating) slot = -1;
        for (int i = 0; i < 16; ++i) {
            if (i != (updating ? slot : -1) && !strcmp(presets[i].name, next.name)) { error = "A preset already has that name"; break; }
            if (!updating && !presets[i].name[0] && slot < 0) slot = i;
        }
        if (!error && slot < 0) error = "All 16 preset slots are full; delete one first";
    }
    if (!error && remove && !presets[slot].name[0]) error = "Preset no longer exists";
    if (!error) {
        Preset previous = presets[slot]; presets[slot] = next;
        nvs_handle_t n;
        esp_err_t err = nvs_open("local_lights", NVS_READWRITE, &n);
        if (err == ESP_OK) {
            err = nvs_set_blob(n, "presets_v3", presets, sizeof(presets));
            if (err == ESP_OK) err = nvs_commit(n);
            nvs_close(n);
        }
        if (err != ESP_OK) { presets[slot] = previous; error = "Unable to save preset"; }
    }
    cJSON *reply = nullptr;
    if (!error) { reply = cJSON_CreateObject(); cJSON_AddItemToObject(reply, "presets", preset_list()); }
    esp_matter::lock::chip_stack_unlock();
    if (error) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
    return json_reply(req, reply);
}
static esp_err_t status(httpd_req_t *req) {
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (pairing && esp_timer_get_time() - pairing_started > (pase_connected ? 180000000LL : 65000000LL)) {
        pairing = false;
        matter_controller_client::get_instance().get_commissioner()->StopPairing(pending.id);
        snprintf(message, sizeof(message), "Pairing timed out. Check that the bulb is nearby and in pairing mode, then retry.");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ready", ready);
    cJSON_AddBoolToObject(o, "pairing", pairing);
    cJSON_AddBoolToObject(o, "pairing_mode", pairing_mode);
    cJSON_AddBoolToObject(o, "rebooting", rebooting);
    cJSON_AddNumberToObject(o, "confirmed", confirmed);
    cJSON_AddNumberToObject(o, "failed", failed);
    cJSON_AddStringToObject(o, "message", message);
    cJSON_AddNumberToObject(o, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "minimum_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(o, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(o, "in_flight", in_flight);
    cJSON_AddItemToObject(o, "presets", preset_list());
    cJSON *a = cJSON_AddArrayToObject(o, "bulbs");
    for (size_t i = 0; i < bulb_count; ++i) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "id", bulbs[i].id);
        cJSON_AddNumberToObject(b, "endpoint", bulbs[i].endpoint);
        cJSON_AddBoolToObject(b, "flicker", effects[i].enabled);
        cJSON_AddNumberToObject(b, "flicker_speed", effects[i].speed);
        cJSON_AddNumberToObject(b, "flicker_range", effects[i].range);
        cJSON_AddNumberToObject(b, "flicker_brightness", effects[i].brightness);
        cJSON_AddNumberToObject(b, "flicker_kelvin", effects[i].kelvin);
        cJSON_AddStringToObject(b, "name", bulbs[i].name); cJSON_AddItemToArray(a, b);
    }
    esp_matter::lock::chip_stack_unlock();
    return json_reply(req, o);
}
static esp_err_t pair(httpd_req_t *req) {
    cJSON *o = body(req);
    if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(o, "code");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
    if (!cJSON_IsString(code) || strlen(code->valuestring) > 128 || !cJSON_IsString(name) ||
        !name->valuestring[0] || strlen(name->valuestring) >= sizeof(pending.name)) {
        cJSON_Delete(o); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter a bulb name and Matter code");
    }
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (!ready || !pairing_mode || rebooting || pairing || in_flight || bulb_count >= 16) {
        esp_matter::lock::chip_stack_unlock(); cJSON_Delete(o);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Controller busy or bulb limit reached");
    }
    wifi_config_t wifi{};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &wifi);
    pending = {}; pending.id = bulb_count + 1; pending.endpoint = 1;
    snprintf(pending.name, sizeof(pending.name), "%s", name->valuestring);
    if (err == ESP_OK) {
        pairing = true; pase_connected = false; pairing_started = esp_timer_get_time(); snprintf(message, sizeof(message), "Pairing bulb; this can take a minute...");
        err = pairing_code_wifi(pending.id, (char *)wifi.sta.ssid, (char *)wifi.sta.password, code->valuestring);
    }
    if (err != ESP_OK) { pairing = false; snprintf(message, sizeof(message), "Could not start pairing: %s", esp_err_to_name(err)); }
    esp_matter::lock::chip_stack_unlock(); cJSON_Delete(o);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Could not start pairing");
    httpd_resp_set_status(req, "202 Accepted"); return httpd_resp_sendstr(req, "Pairing started");
}
static esp_err_t send_light(Bulb &bulb, uint32_t cluster, uint32_t command, const char *payload) {
        auto *cmd = chip::Platform::New<cluster_command>(bulb.id, bulb.endpoint, cluster, command, payload,
            chip::NullOptional,
            [](void *, const chip::app::ConcreteCommandPath &, const chip::app::StatusIB &status, chip::TLV::TLVReader *) {
                if (in_flight) --in_flight;
                if (status.IsSuccess()) {
                    ++confirmed; snprintf(message, sizeof(message), "Light updated");
                } else {
                    ++failed; snprintf(message, sizeof(message), "Light did not accept this setting");
                }
            },
            [](void *, CHIP_ERROR error) {
                if (in_flight) --in_flight;
                ++failed;
                snprintf(message, sizeof(message), "Light did not respond: %s", chip::ErrorStr(error));
            });
        if (cmd) {
            ++in_flight;
            esp_err_t err = cmd->send_command();
            if (err != ESP_OK && in_flight) --in_flight;
            return err;
        }
        return ESP_ERR_NO_MEM;
}
static void flicker_tick(chip::System::Layer *, void *) {
    static size_t cursor = 0;
    static unsigned seen_failed = 0;
    static int64_t retry_after = 0;
    int64_t now = esp_timer_get_time();
    if (failed != seen_failed) { seen_failed = failed; retry_after = now + 30000000; }
    if (ready && !pairing && !rebooting && !pairing_mode && !in_flight && now >= retry_after) {
        for (size_t n = 0; n < bulb_count; ++n) {
            size_t i = (cursor + n) % bulb_count;
            auto &f = effects[i];
            if (!f.enabled || now < f.next) continue;
            // Brightness and warmth move together around the saved baseline.
            int swing = int(esp_random() % (2 * f.range + 1)) - f.range;
            int level = f.brightness + swing;
            if (level < 1) level = 1;
            if (level > 100) level = 100;
            int kelvin = f.kelvin + swing * 25;
            if (kelvin < 2000) kelvin = 2000;
            if (kelvin > 6500) kelvin = 6500;
            unsigned transition = f.speed * 10; // Matter uses tenths of a second.
            char payload[120];
            snprintf(payload, sizeof(payload), "{\"0:U16\":%d,\"1:U16\":%u,\"2:U8\":0,\"3:U8\":0}", 1000000 / kelvin, transition);
            send_light(bulbs[i], 0x300, 10, payload);
            snprintf(payload, sizeof(payload), "{\"0:U8\":%d,\"1:U16\":%u,\"2:U8\":0,\"3:U8\":0}", level * 254 / 100, transition);
            send_light(bulbs[i], 8, 4, payload);
            f.next = now + transition * 100000LL;
            cursor = (i + 1) % bulb_count;
            break;
        }
    }
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(250), flicker_tick, nullptr);
}
static esp_err_t settings(httpd_req_t *req) {
    cJSON *o = body(req);
    if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    int id = number(o, "id", -1);
    cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
    cJSON *flicker = cJSON_GetObjectItemCaseSensitive(o, "flicker");
    int brightness = number(o, "brightness", 70), kelvin = number(o, "kelvin", 2700);
    int speed = number(o, "flicker_speed", 45), range = number(o, "flicker_range", 3);
    bool rename = cJSON_IsString(name) && name->valuestring[0] && strlen(name->valuestring) < sizeof(pending.name);
    bool effect = speed >= 1 && speed <= 120 && range >= 1 && range <= 20 && cJSON_IsBool(flicker) && brightness >= 1 && brightness <= 100 && kelvin >= 2000 && kelvin <= 6500;
    if ((!rename && !effect) || (name && !rename) || (flicker && !effect)) {
        cJSON_Delete(o); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use a name of 1–39 bytes and valid light settings");
    }
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    Bulb *bulb = nullptr;
    for (size_t i = 0; i < bulb_count; ++i) if (bulbs[i].id == (uint32_t)id) bulb = &bulbs[i];
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (bulb && ready && !rebooting && !pairing && (!effect || !pairing_mode || !cJSON_IsTrue(flicker))) {
        err = ESP_OK;
        if (rename) {
            char previous[40]; memcpy(previous, bulb->name, sizeof(previous));
            snprintf(bulb->name, sizeof(bulb->name), "%s", name->valuestring);
            err = save_bulbs();
            if (err != ESP_OK) memcpy(bulb->name, previous, sizeof(previous));
        }
        if (err == ESP_OK && effect) {
            auto &f = effects[bulb - bulbs];
            f.enabled = cJSON_IsTrue(flicker); f.brightness = brightness; f.kelvin = kelvin; f.speed = speed; f.range = range; f.next = 0;
        }
    }
    esp_matter::lock::chip_stack_unlock(); cJSON_Delete(o);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unable to save settings; finish setup first if adding lights");
    return httpd_resp_sendstr(req, "Settings saved");
}
static esp_err_t control(httpd_req_t *req) {
    cJSON *o = body(req);
    if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    int id = number(o, "id", -1), value = number(o, "value", -1);
    cJSON *act = cJSON_GetObjectItemCaseSensitive(o, "action");
    uint32_t cluster = 0, command = 0; char payload[160] = "{}";
    bool valid = cJSON_IsString(act);
    if (valid && !strcmp(act->valuestring, "power") && (value == 0 || value == 1)) {
        cluster = 6; command = value;
    } else if (valid && !strcmp(act->valuestring, "brightness") && value >= 1 && value <= 100) {
        cluster = 8; command = 4;
        snprintf(payload, sizeof(payload), "{\"0:U8\":%d,\"1:U16\":3,\"2:U8\":0,\"3:U8\":0}", value * 254 / 100);
    } else if (valid && !strcmp(act->valuestring, "temperature") && value >= 2000 && value <= 6500) {
        cluster = 0x300; command = 10;
        snprintf(payload, sizeof(payload), "{\"0:U16\":%d,\"1:U16\":3,\"2:U8\":0,\"3:U8\":0}", 1000000 / value);
    } else if (valid && !strcmp(act->valuestring, "hue") && value >= 0 && value <= 360) {
        cluster = 0x300; command = 6;
        snprintf(payload, sizeof(payload), "{\"0:U8\":%d,\"1:U8\":254,\"2:U16\":3,\"3:U8\":0,\"4:U8\":0}", value * 254 / 360);
    } else if (valid && !strcmp(act->valuestring, "color")) {
        int hue = number(o, "hue", -1), saturation = number(o, "saturation", -1);
        if (hue < 0 || hue > 360 || saturation < 0 || saturation > 100) valid = false;
        else {
            cluster = 0x300; command = 6;
            snprintf(payload, sizeof(payload), "{\"0:U8\":%d,\"1:U8\":%d,\"2:U16\":3,\"3:U8\":0,\"4:U8\":0}", hue * 254 / 360, saturation * 254 / 100);
        }
    } else valid = false;
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    Bulb *bulb = nullptr;
    for (size_t i = 0; i < bulb_count; ++i) if (bulbs[i].id == (uint32_t)id) bulb = &bulbs[i];
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid && bulb && ready && !pairing && !rebooting) {
        if (in_flight >= 2) {
            esp_matter::lock::chip_stack_unlock(); cJSON_Delete(o);
            return busy_reply(req, "Please wait for the previous change");
        }
        effects[bulb - bulbs].enabled = false;
        err = send_light(*bulb, cluster, command, payload);
    }
    esp_matter::lock::chip_stack_unlock(); cJSON_Delete(o);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command could not be queued");
    httpd_resp_set_status(req, "202 Accepted"); return httpd_resp_sendstr(req, "Command queued; check the bulb");
}
static void restart_timer(chip::System::Layer *, void *) { esp_restart(); }
static esp_err_t mode(httpd_req_t *req) {
    cJSON *o = body(req);
    cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, "pairing") : nullptr;
    if (!cJSON_IsBool(v)) { cJSON_Delete(o); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode"); }
    bool next = cJSON_IsTrue(v); cJSON_Delete(o);
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (pairing || in_flight || rebooting) {
        esp_matter::lock::chip_stack_unlock();
        return busy_reply(req, "Please wait for the current operation");
    }
    nvs_handle_t n;
    esp_err_t err = nvs_open("local_lights", NVS_READWRITE, &n);
    if (err == ESP_OK) {
        err = nvs_set_u8(n, "pair_boot", next ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(n);
        nvs_close(n);
    }
    if (err == ESP_OK) {
        auto timer_err = chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(1200), restart_timer, nullptr);
        if (timer_err == CHIP_NO_ERROR) rebooting = true; else err = ESP_FAIL;
    }
    esp_matter::lock::chip_stack_unlock();
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to change mode");
    return httpd_resp_sendstr(req, "Restarting; this page will reconnect automatically");
}
bool local_web_pairing_mode() { return pairing_mode; }
static esp_err_t index(httpd_req_t *r) { httpd_resp_set_type(r,"text/html; charset=utf-8");return httpd_resp_send(r,web_page,sizeof(web_page)-1); }
void local_web_ready() {
    nvs_handle_t n;
    if (nvs_open("local_lights", NVS_READWRITE, &n) == ESP_OK) {
        size_t size = sizeof(bulbs);
        if (nvs_get_blob(n,"bulbs",bulbs,&size) == ESP_OK && size % sizeof(Bulb) == 0) bulb_count=size/sizeof(Bulb);
        size_t preset_size = sizeof(presets);
        if (nvs_get_blob(n, "presets_v3", presets, &preset_size) != ESP_OK || preset_size != sizeof(presets)) {
            memset(presets, 0, sizeof(presets));
            struct EntryV2 { uint32_t bulb; char color[8]; uint16_t kelvin; uint8_t brightness; uint8_t white; };
            struct PresetV2 { char name[32]; uint8_t count; EntryV2 entries[16]; };
            auto *v2 = static_cast<PresetV2 *>(calloc(16, sizeof(PresetV2)));
            size_t v2_size = sizeof(PresetV2) * 16;
            bool loaded_v2 = v2 && nvs_get_blob(n, "presets_v2", v2, &v2_size) == ESP_OK && v2_size == sizeof(PresetV2) * 16;
            if (loaded_v2) for (int i = 0; i < 16; ++i) {
                memcpy(presets[i].name, v2[i].name, sizeof(presets[i].name));
                presets[i].count = v2[i].count <= 16 ? v2[i].count : 0;
                for (unsigned j = 0; j < presets[i].count; ++j) {
                    auto &e = presets[i].entries[j]; const auto &old = v2[i].entries[j];
                    e.bulb = old.bulb; memcpy(e.color, old.color, sizeof(e.color));
                    e.kelvin = old.kelvin; e.brightness = old.brightness; e.white = old.white;
                    e.speed = 45; e.range = 3; e.flicker = 0;
                }
            }
            free(v2);
            struct Legacy { char name[32]; char color[8]; uint16_t kelvin; uint8_t brightness; uint8_t white; };
            Legacy old[16]{}; size_t old_size = sizeof(old);
            if (!loaded_v2 && nvs_get_blob(n, "presets_v1", old, &old_size) == ESP_OK && old_size == sizeof(old)) {
                for (int i = 0; i < 16; ++i) if (old[i].name[0]) {
                    memcpy(presets[i].name, old[i].name, sizeof(old[i].name));
                    presets[i].count = bulb_count;
                    for (size_t j = 0; j < bulb_count; ++j) {
                        auto &e = presets[i].entries[j]; e.bulb = bulbs[j].id;
                        memcpy(e.color, old[i].color, sizeof(e.color));
                        e.speed = 45; e.range = 3; e.flicker = 0;
                        e.kelvin = old[i].kelvin; e.brightness = old[i].brightness; e.white = old[i].white;
                    }
                }
            }
        }
        uint8_t flag = 0;
        nvs_get_u8(n, "pair_boot", &flag); pairing_mode = flag == 1;
        nvs_set_u8(n, "pair_boot", 0); nvs_commit(n);
        nvs_close(n);
    }
    if (bulb_count == 0) pairing_mode = true;
    pairing_command::get_instance().set_callbacks({pase,pair_ok,pair_fail});
    snprintf(message,sizeof(message),"Ready to pair and control bulbs"); ready=true;
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(250), flicker_tick, nullptr);
}
void local_web_start() {
    if (server) return;
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG(); cfg.stack_size=6144;cfg.max_open_sockets=3;cfg.max_uri_handlers=7;cfg.lru_purge_enable=true;
    if (httpd_start(&server,&cfg)!=ESP_OK) return;
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=index,.user_ctx=nullptr},
        {.uri="/api/status",.method=HTTP_GET,.handler=status,.user_ctx=nullptr},
        {.uri="/api/pair",.method=HTTP_POST,.handler=pair,.user_ctx=nullptr},
        {.uri="/api/control",.method=HTTP_POST,.handler=control,.user_ctx=nullptr},
        {.uri="/api/mode",.method=HTTP_POST,.handler=mode,.user_ctx=nullptr},
        {.uri="/api/settings",.method=HTTP_POST,.handler=settings,.user_ctx=nullptr},
        {.uri="/api/presets",.method=HTTP_POST,.handler=preset_settings,.user_ctx=nullptr}};
    for(auto &r:routes) httpd_register_uri_handler(server,&r);
}
