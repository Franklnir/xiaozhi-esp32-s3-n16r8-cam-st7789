#include "wifi_configuration_ap.h"
#include "ota_url.h"
#include <cstdio>
#include <cmath>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cJSON.h>
#if !CONFIG_IDF_TARGET_ESP32P4
#include <esp_smartconfig.h>
#endif
#include "ssid_manager.h"
#include "sdkconfig.h"

#define TAG "WifiConfigurationAp"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

extern const char index_html_start[] asm("_binary_wifi_configuration_html_start");
extern const char done_html_start[] asm("_binary_wifi_configuration_done_html_start");

WifiConfigurationAp::WifiConfigurationAp()
{
    event_group_ = xEventGroupCreate();
    language_ = "en";
    nvs_handle_t nvs_sys_init;
    if (nvs_open("system", NVS_READONLY, &nvs_sys_init) == ESP_OK) {
        char lbuf[32] = {0};
        size_t llen = sizeof(lbuf);
        if (nvs_get_str(nvs_sys_init, "language", lbuf, &llen) == ESP_OK && llen > 0) {
            language_ = lbuf;
        }
        nvs_close(nvs_sys_init);
    }
    sleep_mode_ = false;
    instance_any_id_ = nullptr;
    instance_got_ip_ = nullptr;
    max_tx_power_ = 0;
    remember_bssid_ = false;
}

std::vector<wifi_ap_record_t> WifiConfigurationAp::GetAccessPoints()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ap_records_;
}   

WifiConfigurationAp::~WifiConfigurationAp()
{
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiConfigurationAp::SetLanguage(const std::string &&language)
{
    language_ = language;
}

void WifiConfigurationAp::SetLanguage(const std::string &language)
{
    language_ = language;
}

void WifiConfigurationAp::SetSsidPrefix(const std::string &&ssid_prefix)
{
    ssid_prefix_ = ssid_prefix;
}

void WifiConfigurationAp::SetSsidPrefix(const std::string &ssid_prefix)
{
    ssid_prefix_ = ssid_prefix;
}

void WifiConfigurationAp::Start()
{
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiConfigurationAp::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiConfigurationAp::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));

    StartAccessPoint();
    StartWebServer();
    
    // Start scan immediately
    esp_wifi_scan_start(nullptr, false);
    // Setup periodic WiFi scan timer.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise the AP-mode scan list would stop refreshing
    // whenever the user paused interacting with the config web UI. See
    // esp_timer_get_next_alarm_for_wake_up in components/esp_timer/src/esp_timer.c.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* self = static_cast<WifiConfigurationAp*>(arg);
            if (!self->is_connecting_) {
                esp_wifi_scan_start(nullptr, false);
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_scan_timer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &scan_timer_));
    esp_timer_start_once(scan_timer_, 3 * 1000000);
}

std::string WifiConfigurationAp::GetSsid()
{
    // Get MAC and use it to generate a unique SSID
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_AP, mac);
#else
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
#endif
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", ssid_prefix_.c_str(), mac[4], mac[5]);
    return std::string(ssid);
}

std::string WifiConfigurationAp::GetWebServerUrl()
{
    // http://192.168.4.1
    return "http://192.168.4.1";
}

void WifiConfigurationAp::StartAccessPoint()
{
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    // Create the default WiFi AP interface
    ap_netif_ = esp_netif_create_default_wifi_ap();

    // Set the router IP address to 192.168.4.1
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_set_ip_info(ap_netif_, &ip_info);
    esp_netif_dhcps_start(ap_netif_);

    // Start the DNS server
    dns_server_ = std::make_unique<DnsServer>();
    dns_server_->Start(ip_info.gw);

    // Get the SSID
    std::string ssid = GetSsid();

    // Set the WiFi configuration
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, ssid.c_str());
    wifi_config.ap.ssid_len = ssid.length();
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    // Start the WiFi Access Point
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
#else
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY));
#endif

    ESP_LOGI(TAG, "Access Point started with SSID %s", ssid.c_str());

    // 加载高级配置
    ota_url_.clear();
    sleep_dim_minutes_ = -1;
    sleep_off_minutes_ = -1;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        // 读取OTA URL
        char ota_url[wifi_config::kMaxOtaUrlLength + 1] = {0};
        size_t ota_url_size = sizeof(ota_url);
        err = nvs_get_str(nvs, "ota_url", ota_url, &ota_url_size);
        if (err == ESP_OK && wifi_config::IsValidOtaUrl(ota_url)) {
            ota_url_ = ota_url;
        } else if (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH) {
            ESP_LOGW(TAG, "Ignoring invalid saved OTA URL; using the application default");
        }

        // 读取WiFi功率
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi max tx power from NVS: %d", max_tx_power_);
            ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
        } else {
            esp_wifi_get_max_tx_power(&max_tx_power_);
        }

        // 读取BSSID记忆设置
        uint8_t remember_bssid = 0;
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid);
        if (err == ESP_OK) {
            remember_bssid_ = remember_bssid != 0;
        } else {
            remember_bssid_ = false; // 默认值
        }

        // 读取睡眠模式设置
        uint8_t sleep_mode = 0;
        err = nvs_get_u8(nvs, "sleep_mode", &sleep_mode);
        if (err == ESP_OK) {
            sleep_mode_ = sleep_mode != 0;
        } else {
            sleep_mode_ = true; // 默认值
        }

        // 读取睡眠定时器设置（分钟）
        int32_t sleep_dim_minutes = 0;
        if (nvs_get_i32(nvs, "sleep_dim_min", &sleep_dim_minutes) == ESP_OK &&
            sleep_dim_minutes >= 0 && sleep_dim_minutes <= 1440) {
            sleep_dim_minutes_ = sleep_dim_minutes;
        }
        int32_t sleep_off_minutes = 0;
        if (nvs_get_i32(nvs, "sleep_off_min", &sleep_off_minutes) == ESP_OK &&
            sleep_off_minutes >= 0 && sleep_off_minutes <= 1440) {
            sleep_off_minutes_ = sleep_off_minutes;
        }

        nvs_close(nvs);
    }
}

void WifiConfigurationAp::StartWebServer()
{
    // Start the web server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // 5G Network takes longer to connect
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    ESP_ERROR_CHECK(httpd_start(&server_, &config));

    // Register the index.html file
    httpd_uri_t index_html = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, index_html_start, strlen(index_html_start));
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index_html));

    // Register the /saved/list URI
    httpd_uri_t saved_list = {
        .uri = "/saved/list",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto ssid_list = SsidManager::GetInstance().GetSsidList();
            std::string json_str = "[";
            for (const auto& ssid : ssid_list) {
                json_str += "\"" + ssid.ssid + "\",";
            }
            if (json_str.length() > 1) {
                json_str.pop_back(); // Remove the last comma
            }
            json_str += "]";
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, json_str.c_str(), HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_list));

    // Register the /saved/set_default URI
    httpd_uri_t saved_set_default = {
        .uri = "/saved/set_default",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            std::string uri = req->uri;
            auto pos = uri.find("?index=");
            if (pos != std::string::npos) {
                int index = -1;
                sscanf(&req->uri[pos+7], "%d", &index);
                ESP_LOGI(TAG, "Set default item %d", index);
                SsidManager::GetInstance().SetDefaultSsid(index);
            }
            // send {}
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_set_default));

    // Register the /saved/delete URI
    httpd_uri_t saved_delete = {
        .uri = "/saved/delete",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            std::string uri = req->uri;
            auto pos = uri.find("?index=");
            if (pos != std::string::npos) {
                int index = -1;
                sscanf(&req->uri[pos+7], "%d", &index);
                ESP_LOGI(TAG, "Delete saved list item %d", index);
                SsidManager::GetInstance().RemoveSsid(index);
            }
            // send {}
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_delete));

    // Register the /scan URI
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            std::lock_guard<std::mutex> lock(this_->mutex_);

            if (this_->ap_records_.empty() && !this_->is_connecting_) {
                esp_wifi_scan_start(nullptr, false);
            }

            // Check if 5G is supported
            bool support_5g = false;
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            support_5g = true;
#endif

            // Send the scan results as JSON
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_sendstr_chunk(req, "{\"support_5g\":");
            httpd_resp_sendstr_chunk(req, support_5g ? "true" : "false");
            httpd_resp_sendstr_chunk(req, ",\"aps\":[");
            for (int i = 0; i < this_->ap_records_.size(); i++) {
                ESP_LOGI(TAG, "SSID: %s, RSSI: %d, Authmode: %d",
                    (char *)this_->ap_records_[i].ssid, this_->ap_records_[i].rssi, this_->ap_records_[i].authmode);
                char buf[128];
                snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"rssi\":%d,\"authmode\":%d}",
                    (char *)this_->ap_records_[i].ssid, this_->ap_records_[i].rssi, this_->ap_records_[i].authmode);
                httpd_resp_sendstr_chunk(req, buf);
                if (i < this_->ap_records_.size() - 1) {
                    httpd_resp_sendstr_chunk(req, ",");
                }
            }
            httpd_resp_sendstr_chunk(req, "]}");
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &scan));

    // Register the form submission
    httpd_uri_t form_submit = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            char *buf;
            size_t buf_len = req->content_len;
            if (buf_len > 1024) { // 限制最大请求体大小
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
                return ESP_FAIL;
            }

            buf = (char *)malloc(buf_len + 1);
            if (!buf) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
                return ESP_FAIL;
            }

            int ret = httpd_req_recv(req, buf, buf_len);
            if (ret <= 0) {
                free(buf);
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    httpd_resp_send_408(req);
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive request");
                }
                return ESP_FAIL;
            }
            buf[ret] = '\0';

            // 解析 JSON 数据
            cJSON *json = cJSON_Parse(buf);
            free(buf);
            if (!json) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
                return ESP_FAIL;
            }

            cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
            cJSON *password_item = cJSON_GetObjectItemCaseSensitive(json, "password");

            if (!cJSON_IsString(ssid_item) || (ssid_item->valuestring == NULL) || (strlen(ssid_item->valuestring) >= 33)) {
                cJSON_Delete(json);
                httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid SSID\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            std::string ssid_str = ssid_item->valuestring;
            std::string password_str = "";
            if (cJSON_IsString(password_item) && (password_item->valuestring != NULL) && (strlen(password_item->valuestring) < 65)) {
                password_str = password_item->valuestring;
            }

            // Multi-SSID backup failover
            std::string ssid1_str = "";
            std::string password1_str = "";
            cJSON *ssid1_item = cJSON_GetObjectItemCaseSensitive(json, "ssid1");
            cJSON *pass1_item = cJSON_GetObjectItemCaseSensitive(json, "password1");
            if (cJSON_IsString(ssid1_item) && ssid1_item->valuestring && strlen(ssid1_item->valuestring) > 0 && strlen(ssid1_item->valuestring) < 33) {
                ssid1_str = ssid1_item->valuestring;
                if (cJSON_IsString(pass1_item) && pass1_item->valuestring && strlen(pass1_item->valuestring) < 65) {
                    password1_str = pass1_item->valuestring;
                }
            }

            std::string ssid2_str = "";
            std::string password2_str = "";
            cJSON *ssid2_item = cJSON_GetObjectItemCaseSensitive(json, "ssid2");
            cJSON *pass2_item = cJSON_GetObjectItemCaseSensitive(json, "password2");
            if (cJSON_IsString(ssid2_item) && ssid2_item->valuestring && strlen(ssid2_item->valuestring) > 0 && strlen(ssid2_item->valuestring) < 33) {
                ssid2_str = ssid2_item->valuestring;
                if (cJSON_IsString(pass2_item) && pass2_item->valuestring && strlen(pass2_item->valuestring) < 65) {
                    password2_str = pass2_item->valuestring;
                }
            }

            // 获取当前对象
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            bool connected = this_->ConnectToWifi(ssid_str, password_str);
            if (!connected && !ssid1_str.empty()) {
                ESP_LOGI(TAG, "Primary SSID %s not reachable, attempting backup SSID1 %s", ssid_str.c_str(), ssid1_str.c_str());
                connected = this_->ConnectToWifi(ssid1_str, password1_str);
            }
            if (!connected && !ssid2_str.empty()) {
                ESP_LOGI(TAG, "Backup SSID1 %s not reachable, attempting backup SSID2 %s", ssid1_str.c_str(), ssid2_str.c_str());
                connected = this_->ConnectToWifi(ssid2_str, password2_str);
            }

            if (!connected) {
                cJSON_Delete(json);
                httpd_resp_send(req, "{\"success\":false,\"error\":\"Gagal terhubung ke WiFi. Periksa SSID dan Password.\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            // Save backups first so Primary SSID remains at top priority index 0 in ssid_list_
            if (!ssid2_str.empty()) {
                SsidManager::GetInstance().AddSsid(ssid2_str, password2_str);
            }
            if (!ssid1_str.empty()) {
                SsidManager::GetInstance().AddSsid(ssid1_str, password1_str);
            }
            this_->Save(ssid_str, password_str);
            cJSON_Delete(json);
            // 设置成功响应
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &form_submit));

    // Register the done.html page
    httpd_uri_t done_html = {
        .uri = "/done.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, done_html_start, strlen(done_html_start));
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &done_html));

    // Register the exit endpoint - exits config mode without rebooting
    httpd_uri_t exit_config = {
        .uri = "/exit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto* this_ = static_cast<WifiConfigurationAp*>(req->user_ctx);
            
            // 设置响应头，防止浏览器缓存
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            // 发送响应
            httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
            
            // 延迟调用回调，确保HTTP响应完全发送
            ESP_LOGI(TAG, "Exiting config mode...");
            xTaskCreate([](void *ctx) {
                // 等待200ms确保HTTP响应完全发送
                vTaskDelay(pdMS_TO_TICKS(200));
                
                auto* self = static_cast<WifiConfigurationAp*>(ctx);
                // 通知回调退出配网模式
                if (self->on_exit_requested_) {
                    self->on_exit_requested_();
                }
                vTaskDelete(NULL);
            }, "exit_config_task", 4096, this_, 5, NULL);
            
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &exit_config));

    auto captive_portal_handler = [](httpd_req_t *req) -> esp_err_t {
        auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
        std::string url = this_->GetWebServerUrl() + "/?lang=" + this_->language_ + "&_=" + std::to_string(esp_timer_get_time());
        // Set content type to prevent browser warnings
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url.c_str());
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    };

    // Register all common captive portal detection endpoints
    const char* captive_portal_urls[] = {
        "/hotspot-detect.html",    // Apple
        "/generate_204*",           // Android
        "/mobile/status.php",      // Android
        "/check_network_status.txt", // Windows
        "/ncsi.txt",              // Windows
        "/fwlink/",               // Microsoft
        "/connectivity-check.html", // Firefox
        "/success.txt",           // Various
        "/portal.html",           // Various
        "/library/test/success.html" // Apple
    };

    for (const auto& url : captive_portal_urls) {
        httpd_uri_t redirect_uri = {
            .uri = url,
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = this
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &redirect_uri));
    }

    // Register the /advanced/config URI
    httpd_uri_t advanced_config = {
        .uri = "/advanced/config",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            // 获取当前对象
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            
            // 创建JSON对象
            cJSON *json = cJSON_CreateObject();
            if (!json) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON");
                return ESP_FAIL;
            }

            // Wi-Fi Saved Networks (Primary & Failover)
            auto ssid_list = SsidManager::GetInstance().GetSsidList();
            if (!ssid_list.empty()) {
                cJSON_AddStringToObject(json, "ssid", ssid_list[0].ssid.c_str());
                cJSON_AddStringToObject(json, "password", ssid_list[0].password.c_str());
                if (ssid_list.size() > 1) {
                    cJSON_AddStringToObject(json, "backup_ssid", ssid_list[1].ssid.c_str());
                    cJSON_AddStringToObject(json, "backup_password", ssid_list[1].password.c_str());
                }
                if (ssid_list.size() > 2) {
                    cJSON_AddStringToObject(json, "backup_ssid2", ssid_list[2].ssid.c_str());
                    cJSON_AddStringToObject(json, "backup_password2", ssid_list[2].password.c_str());
                }
            }

            // 添加配置项到JSON
            if (!this_->ota_url_.empty()) {
                cJSON_AddStringToObject(json, "ota_url", this_->ota_url_.c_str());
            }
            cJSON_AddNumberToObject(json, "max_tx_power", this_->max_tx_power_);
            cJSON_AddBoolToObject(json, "remember_bssid", this_->remember_bssid_);
            cJSON_AddBoolToObject(json, "sleep_mode", this_->sleep_mode_);
            if (this_->sleep_dim_minutes_ >= 0) {
                cJSON_AddNumberToObject(json, "sleep_dim_minutes", this_->sleep_dim_minutes_);
            } else {
                cJSON_AddNullToObject(json, "sleep_dim_minutes");
            }
            if (this_->sleep_off_minutes_ >= 0) {
                cJSON_AddNumberToObject(json, "sleep_off_minutes", this_->sleep_off_minutes_);
            } else {
                cJSON_AddNullToObject(json, "sleep_off_minutes");
            }
            cJSON_AddBoolToObject(json, "show_ota_config", this_->show_ota_config_);
            cJSON_AddBoolToObject(json, "show_sleep_config", this_->show_sleep_config_);

            // Operating mode (0 = Xiaozhi AI, 1 = Chronchi Companion)
            int32_t dev_mode = 0;
            nvs_handle_t nvs_ch_r;
            if (nvs_open("chronchi", NVS_READONLY, &nvs_ch_r) == ESP_OK) {
                nvs_get_i32(nvs_ch_r, "mode", &dev_mode);
                nvs_close(nvs_ch_r);
            }
            cJSON_AddNumberToObject(json, "device_mode", dev_mode);

            // Server domain
            char s_domain[64] = {0};
            size_t s_len = sizeof(s_domain);
            nvs_handle_t nvs_wifi_r;
            if (nvs_open("wifi", NVS_READONLY, &nvs_wifi_r) == ESP_OK) {
                if (nvs_get_str(nvs_wifi_r, "server_domain", s_domain, &s_len) == ESP_OK) {
                    cJSON_AddStringToObject(json, "server_domain", s_domain);
                } else {
                    cJSON_AddStringToObject(json, "server_domain", "xiaozhiscig.biz.id");
                }
                nvs_close(nvs_wifi_r);
            } else {
                cJSON_AddStringToObject(json, "server_domain", "xiaozhiscig.biz.id");
            }

            // Display settings
            nvs_handle_t nvs_d_r;
            if (nvs_open("display", NVS_READONLY, &nvs_d_r) == ESP_OK) {
                char d_preset[32] = {0};
                size_t d_len = sizeof(d_preset);
                if (nvs_get_str(nvs_d_r, "preset", d_preset, &d_len) == ESP_OK && strlen(d_preset) > 0) {
                    cJSON_AddStringToObject(json, "display_preset", d_preset);
                } else {
                    cJSON_AddStringToObject(json, "display_preset", "st7789_240x240_8p");
                }
                int32_t rot = 0;
                nvs_get_i32(nvs_d_r, "rotation", &rot);
                cJSON_AddNumberToObject(json, "display_rotation", rot);
                const char* pin_keys[] = {"scl_pin", "sda_pin", "dc_pin", "rst_pin", "cs_pin", "bl_pin", "oled_sda", "oled_scl"};
                for (auto key : pin_keys) {
                    int32_t pval = -1;
                    if (nvs_get_i32(nvs_d_r, key, &pval) == ESP_OK) {
                        cJSON_AddNumberToObject(json, key, pval);
                    }
                }
                nvs_close(nvs_d_r);
            } else {
                cJSON_AddStringToObject(json, "display_preset", "st7789_240x240_8p");
                cJSON_AddNumberToObject(json, "display_rotation", 0);
            }

            // Camera settings
            nvs_handle_t nvs_c_r;
            if (nvs_open("camera", NVS_READONLY, &nvs_c_r) == ESP_OK) {
                char c_preset[32] = {0};
                size_t c_len = sizeof(c_preset);
                if (nvs_get_str(nvs_c_r, "preset", c_preset, &c_len) == ESP_OK) {
                    cJSON_AddStringToObject(json, "camera_preset", c_preset);
                } else {
                    cJSON_AddStringToObject(json, "camera_preset", "");
                }
                nvs_close(nvs_c_r);
            } else {
                cJSON_AddStringToObject(json, "camera_preset", "");
            }

            // Audio settings
            nvs_handle_t nvs_a_r;
            if (nvs_open("audio", NVS_READONLY, &nvs_a_r) == ESP_OK) {
                char a_profile[32] = {0};
                size_t a_len = sizeof(a_profile);
                if (nvs_get_str(nvs_a_r, "profile", a_profile, &a_len) == ESP_OK) {
                    cJSON_AddStringToObject(json, "audio_profile", a_profile);
                } else {
                    cJSON_AddStringToObject(json, "audio_profile", "");
                }
                const char* audio_pin_keys[] = {"mic_ws", "mic_sck", "mic_din", "spk_bclk", "spk_ws", "spk_dout"};
                for (auto key : audio_pin_keys) {
                    int32_t pval = -1;
                    if (nvs_get_i32(nvs_a_r, key, &pval) == ESP_OK) {
                        cJSON_AddNumberToObject(json, key, pval);
                    }
                }
                nvs_close(nvs_a_r);
            } else {
                cJSON_AddStringToObject(json, "audio_profile", "");
            }

            cJSON_AddStringToObject(json, "language", this_->language_.c_str());
            nvs_handle_t nvs_sys_cfg;
            if (nvs_open("system", NVS_READONLY, &nvs_sys_cfg) == ESP_OK) {
                char vlang[32] = {0};
                size_t vlen = sizeof(vlang);
                if (nvs_get_str(nvs_sys_cfg, "voice_lang", vlang, &vlen) == ESP_OK && vlen > 0) {
                    cJSON_AddStringToObject(json, "system_voice_lang", vlang);
                } else {
                    cJSON_AddStringToObject(json, "system_voice_lang", "en-US");
                }
                nvs_close(nvs_sys_cfg);
            } else {
                cJSON_AddStringToObject(json, "system_voice_lang", "en-US");
            }

            // 发送JSON响应
            char *json_str = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (!json_str) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to print JSON");
                return ESP_FAIL;
            }

            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, json_str, strlen(json_str));
            free(json_str);
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &advanced_config));

    // Register the /advanced/submit URI
    httpd_uri_t advanced_submit = {
        .uri = "/advanced/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            char *buf;
            size_t buf_len = req->content_len;
            if (buf_len > 4096) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
                return ESP_FAIL;
            }

            buf = (char *)malloc(buf_len + 1);
            if (!buf) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
                return ESP_FAIL;
            }

            int ret = httpd_req_recv(req, buf, buf_len);
            if (ret <= 0) {
                free(buf);
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    httpd_resp_send_408(req);
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive request");
                }
                return ESP_FAIL;
            }
            buf[ret] = '\0';

            // 解析JSON数据
            cJSON *json = cJSON_Parse(buf);
            free(buf);
            if (!cJSON_IsObject(json)) {
                cJSON_Delete(json);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
                return ESP_FAIL;
            }

            // 获取当前对象
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);

            // Reject invalid URLs before opening NVS or changing any settings.
            cJSON* ota_url = cJSON_GetObjectItemCaseSensitive(json, "ota_url");
            if (ota_url && (!cJSON_IsString(ota_url) || !ota_url->valuestring ||
                            !wifi_config::IsValidOtaUrl(ota_url->valuestring))) {
                cJSON_Delete(json);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req,
                                "{\"success\":false,\"error\":\"OTA URL must be empty or a valid "
                                "HTTP(S) URL (max 255 bytes)\"}",
                                HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            // Missing fields preserve their overrides; null restores the board default.
            cJSON* sleep_dim = cJSON_GetObjectItemCaseSensitive(json, "sleep_dim_minutes");
            cJSON* sleep_off = cJSON_GetObjectItemCaseSensitive(json, "sleep_off_minutes");
            int dim = this_->sleep_dim_minutes_;
            int off = this_->sleep_off_minutes_;
            auto read_minutes = [](const cJSON* item, int& minutes) {
                if (!item) {
                    return true;
                }
                if (cJSON_IsNull(item)) {
                    minutes = -1;
                    return true;
                }
                if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
                    item->valuedouble < 0 || item->valuedouble > 1440 ||
                    std::floor(item->valuedouble) != item->valuedouble) {
                    return false;
                }
                minutes = static_cast<int>(item->valuedouble);
                return true;
            };
            if (!read_minutes(sleep_dim, dim) || !read_minutes(sleep_off, off) ||
                ((sleep_dim || sleep_off) && dim > 0 && off > 0 && off <= dim)) {
                cJSON_Delete(json);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid sleep timer values\"}",
                                HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            // Check if reboot requested (default true if not specified)
            cJSON *reboot_item = cJSON_GetObjectItem(json, "reboot");
            bool should_reboot = (reboot_item == NULL || cJSON_IsTrue(reboot_item));

            // 1. Wi-Fi Multi-SSID parsing and saving
            cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
            cJSON *password_item = cJSON_GetObjectItem(json, "password");
            cJSON *b_ssid_item = cJSON_GetObjectItem(json, "backup_ssid");
            cJSON *b_pass_item = cJSON_GetObjectItem(json, "backup_password");
            cJSON *b_ssid2_item = cJSON_GetObjectItem(json, "backup_ssid2");
            cJSON *b_pass2_item = cJSON_GetObjectItem(json, "backup_password2");
            if (!b_ssid_item) b_ssid_item = cJSON_GetObjectItem(json, "ssid1");
            if (!b_pass_item) b_pass_item = cJSON_GetObjectItem(json, "password1");
            if (!b_ssid2_item) b_ssid2_item = cJSON_GetObjectItem(json, "ssid2");
            if (!b_pass2_item) b_pass2_item = cJSON_GetObjectItem(json, "password2");

            if (should_reboot && !cJSON_IsBool(reboot_item) && !ota_url && !sleep_dim && !sleep_off) {
                // Keep compatibility with programmatic callers omitting ssid when only updating advanced params
            } else if (should_reboot && cJSON_IsTrue(reboot_item)) {
                if (!cJSON_IsString(ssid_item) || !ssid_item->valuestring || strlen(ssid_item->valuestring) == 0) {
                    cJSON_Delete(json);
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_hdr(req, "Connection", "close");
                    httpd_resp_send(req, "{\"success\":false,\"error\":\"Primary SSID is required to connect and restart\"}", HTTPD_RESP_USE_STRLEN);
                    return ESP_OK;
                }
            }

            if (cJSON_IsString(ssid_item) && ssid_item->valuestring && strlen(ssid_item->valuestring) > 0) {
                std::string ssid_str = ssid_item->valuestring;
                std::string pass_str = (cJSON_IsString(password_item) && password_item->valuestring) ? password_item->valuestring : "";
                std::string b_ssid_str = (cJSON_IsString(b_ssid_item) && b_ssid_item->valuestring) ? b_ssid_item->valuestring : "";
                std::string b_pass_str = (cJSON_IsString(b_pass_item) && b_pass_item->valuestring) ? b_pass_item->valuestring : "";
                std::string b_ssid2_str = (cJSON_IsString(b_ssid2_item) && b_ssid2_item->valuestring) ? b_ssid2_item->valuestring : "";
                std::string b_pass2_str = (cJSON_IsString(b_pass2_item) && b_pass2_item->valuestring) ? b_pass2_item->valuestring : "";

                // Save backups first so Primary SSID remains at top priority index 0
                if (!b_ssid2_str.empty()) {
                    SsidManager::GetInstance().AddSsid(b_ssid2_str, b_pass2_str);
                }
                if (!b_ssid_str.empty()) {
                    SsidManager::GetInstance().AddSsid(b_ssid_str, b_pass_str);
                }
                this_->Save(ssid_str, pass_str);
                ESP_LOGI(TAG, "Saved Wi-Fi: Primary=%s, Backup1=%s, Backup2=%s",
                         ssid_str.c_str(), b_ssid_str.c_str(), b_ssid2_str.c_str());
            }

            // 打开NVS wifi
            nvs_handle_t nvs;
            esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);
            if (err != ESP_OK) {
                cJSON_Delete(json);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open NVS");
                return ESP_FAIL;
            }

            // 保存OTA URL
            if (ota_url) {
                err = nvs_set_str(nvs, "ota_url", ota_url->valuestring);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save OTA URL: %d", err);
                    nvs_close(nvs);
                    cJSON_Delete(json);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                        "Failed to save OTA URL");
                    return ESP_FAIL;
                }
            }

            // 保存WiFi功率
            cJSON *max_tx_power = cJSON_GetObjectItem(json, "max_tx_power");
            if (cJSON_IsNumber(max_tx_power)) {
                this_->max_tx_power_ = max_tx_power->valueint;
                esp_wifi_set_max_tx_power(this_->max_tx_power_);
                nvs_set_i8(nvs, "max_tx_power", this_->max_tx_power_);
            }

            // 保存BSSID记忆设置
            cJSON *remember_bssid = cJSON_GetObjectItem(json, "remember_bssid");
            if (cJSON_IsBool(remember_bssid)) {
                this_->remember_bssid_ = cJSON_IsTrue(remember_bssid);
                nvs_set_u8(nvs, "remember_bssid", this_->remember_bssid_ ? 1 : 0);
            }

            // 保存睡眠模式设置
            cJSON *sleep_mode = cJSON_GetObjectItem(json, "sleep_mode");
            if (cJSON_IsBool(sleep_mode)) {
                this_->sleep_mode_ = cJSON_IsTrue(sleep_mode);
                nvs_set_u8(nvs, "sleep_mode", this_->sleep_mode_ ? 1 : 0);
            }

            // Save Server Domain
            cJSON *server_domain = cJSON_GetObjectItem(json, "server_domain");
            if (cJSON_IsString(server_domain) && server_domain->valuestring && strlen(server_domain->valuestring) > 0) {
                nvs_set_str(nvs, "server_domain", server_domain->valuestring);
            }

            // Save only explicitly supplied overrides. NVS keys are limited to 15 characters.
            auto save_minutes = [nvs](const char* key, int minutes) {
                if (minutes < 0) {
                    esp_err_t result = nvs_erase_key(nvs, key);
                    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
                }
                return nvs_set_i32(nvs, key, minutes);
            };
            if (sleep_dim || sleep_off) {
                esp_err_t timer_err = ESP_OK;
                if (sleep_dim) {
                    timer_err = save_minutes("sleep_dim_min", dim);
                }
                if (timer_err == ESP_OK && sleep_off) {
                    timer_err = save_minutes("sleep_off_min", off);
                }
                if (timer_err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save sleep timer values: %d", timer_err);
                    cJSON_Delete(json);
                    nvs_close(nvs);
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to save sleep timer values\"}", HTTPD_RESP_USE_STRLEN);
                    return ESP_FAIL;
                }
            }

            err = nvs_commit(nvs);
            if (err == ESP_OK && ota_url) {
                this_->ota_url_ = ota_url->valuestring;
            }
            if (err == ESP_OK) {
                this_->sleep_dim_minutes_ = dim;
                this_->sleep_off_minutes_ = off;
            }
            nvs_close(nvs);

            // Save display settings & pins
            cJSON *display_preset = cJSON_GetObjectItem(json, "display_preset");
            cJSON *display_rotation = cJSON_GetObjectItem(json, "display_rotation");
            nvs_handle_t nvs_d;
            if (nvs_open("display", NVS_READWRITE, &nvs_d) == ESP_OK) {
                if (cJSON_IsString(display_preset) && display_preset->valuestring && strlen(display_preset->valuestring) > 0) {
                    nvs_set_str(nvs_d, "preset", display_preset->valuestring);
                    if (strcmp(display_preset->valuestring, "st7789_240x240_7p") == 0) {
                        nvs_set_i32(nvs_d, "offset_x", 0);
                        nvs_set_i32(nvs_d, "offset_y", 0);
                        nvs_set_i32(nvs_d, "cs_pin", -1);
                    } else if (strcmp(display_preset->valuestring, "st7789_240x240_8p") == 0) {
                        nvs_set_i32(nvs_d, "offset_x", 0);
                        nvs_set_i32(nvs_d, "offset_y", 0);
                        nvs_set_i32(nvs_d, "cs_pin", 45);
                    } else if (strcmp(display_preset->valuestring, "st7789_240x280_8p") == 0) {
                        nvs_set_i32(nvs_d, "offset_x", 0);
                        nvs_set_i32(nvs_d, "offset_y", 20);
                        nvs_set_i32(nvs_d, "cs_pin", 45);
                    } else if (strcmp(display_preset->valuestring, "st7789_240x320_8p") == 0) {
                        nvs_set_i32(nvs_d, "offset_x", 0);
                        nvs_set_i32(nvs_d, "offset_y", 0);
                        nvs_set_i32(nvs_d, "cs_pin", 45);
                    }
                }
                if (cJSON_IsNumber(display_rotation)) {
                    nvs_set_i32(nvs_d, "rotation", display_rotation->valueint);
                }
                const char* pin_keys[] = {"scl_pin", "sda_pin", "dc_pin", "rst_pin", "cs_pin", "bl_pin", "oled_sda", "oled_scl"};
                for (auto key : pin_keys) {
                    cJSON *item = cJSON_GetObjectItem(json, key);
                    if (cJSON_IsNumber(item)) {
                        nvs_set_i32(nvs_d, key, item->valueint);
                    }
                }
                nvs_commit(nvs_d);
                nvs_close(nvs_d);
            }

            // Save camera settings
            cJSON *camera_preset = cJSON_GetObjectItem(json, "camera_preset");
            if (camera_preset != nullptr) {
                nvs_handle_t nvs_c;
                if (nvs_open("camera", NVS_READWRITE, &nvs_c) == ESP_OK) {
                    if (cJSON_IsString(camera_preset) && camera_preset->valuestring && strlen(camera_preset->valuestring) > 0) {
                        nvs_set_str(nvs_c, "preset", camera_preset->valuestring);
                    }
                    nvs_commit(nvs_c);
                    nvs_close(nvs_c);
                }
            }

            // Save operating mode (Xiaozhi vs Chronchi)
            cJSON *device_mode = cJSON_GetObjectItem(json, "device_mode");
            if (cJSON_IsNumber(device_mode)) {
                nvs_handle_t nvs_ch;
                if (nvs_open("chronchi", NVS_READWRITE, &nvs_ch) == ESP_OK) {
                    nvs_set_i32(nvs_ch, "mode", device_mode->valueint);
                    nvs_commit(nvs_ch);
                    nvs_close(nvs_ch);
                }
            }

            // Save language setting
            cJSON *lang_item = cJSON_GetObjectItem(json, "language");
            if (cJSON_IsString(lang_item) && lang_item->valuestring && strlen(lang_item->valuestring) > 0) {
                this_->language_ = lang_item->valuestring;
                nvs_handle_t nvs_sys;
                if (nvs_open("system", NVS_READWRITE, &nvs_sys) == ESP_OK) {
                    nvs_set_str(nvs_sys, "language", this_->language_.c_str());
                    nvs_commit(nvs_sys);
                    nvs_close(nvs_sys);
                }
            }

            // Save voice language setting
            cJSON *voice_item = cJSON_GetObjectItem(json, "system_voice_lang");
            if (!voice_item) {
                voice_item = cJSON_GetObjectItem(json, "voice_lang");
            }
            if (cJSON_IsString(voice_item) && voice_item->valuestring && strlen(voice_item->valuestring) > 0) {
                nvs_handle_t nvs_sys;
                if (nvs_open("system", NVS_READWRITE, &nvs_sys) == ESP_OK) {
                    nvs_set_str(nvs_sys, "voice_lang", voice_item->valuestring);
                    nvs_commit(nvs_sys);
                    nvs_close(nvs_sys);
                }
            }

            // Save audio settings
            cJSON *audio_profile = cJSON_GetObjectItem(json, "audio_profile");
            nvs_handle_t nvs_a;
            if (nvs_open("audio", NVS_READWRITE, &nvs_a) == ESP_OK) {
                if (cJSON_IsString(audio_profile) && audio_profile->valuestring && strlen(audio_profile->valuestring) > 0) {
                    nvs_set_str(nvs_a, "profile", audio_profile->valuestring);
                }
                const char* audio_pin_keys[] = {"mic_ws", "mic_sck", "mic_din", "spk_bclk", "spk_ws", "spk_dout"};
                for (auto key : audio_pin_keys) {
                    cJSON *item = cJSON_GetObjectItem(json, key);
                    if (cJSON_IsNumber(item)) {
                        nvs_set_i32(nvs_a, key, item->valueint);
                    }
                }
                nvs_commit(nvs_a);
                nvs_close(nvs_a);
            }

            cJSON_Delete(json);

            if (err != ESP_OK) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
                return ESP_FAIL;
            }

            // 发送成功响应
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");

            ESP_LOGI(TAG, "Saved settings: ota_url=%s, max_tx_power=%d, remember_bssid=%d, sleep_mode=%d, sleep_dim=%d, sleep_off=%d",
                this_->ota_url_.c_str(), this_->max_tx_power_, this_->remember_bssid_, this_->sleep_mode_,
                this_->sleep_dim_minutes_, this_->sleep_off_minutes_);

            if (should_reboot && cJSON_IsBool(reboot_item)) {
                httpd_resp_send(req, "{\"success\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
                ESP_LOGI(TAG, "All configurations saved to NVS. Restarting in 600ms to apply changes...");
                xTaskCreate([](void *ctx) {
                    vTaskDelay(pdMS_TO_TICKS(600));
                    esp_restart();
                }, "save_restart", 2048, NULL, 5, NULL);
            } else {
                httpd_resp_send(req, "{\"success\":true,\"reboot\":false}", HTTPD_RESP_USE_STRLEN);
                ESP_LOGI(TAG, "Configurations saved to NVS.");
            }
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &advanced_submit));

    // Register /reboot endpoint
    httpd_uri_t reboot_endpoint = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
            ESP_LOGI(TAG, "Reboot requested from web portal...");
            xTaskCreate([](void *ctx) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }, "web_reboot", 2048, NULL, 5, NULL);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &reboot_endpoint));

    ESP_LOGI(TAG, "Web server started");
}

bool WifiConfigurationAp::ConnectToWifi(const std::string &ssid, const std::string &password)
{
    if (ssid.empty()) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        return false;
    }
    
    if (ssid.length() > 32) {  // WiFi SSID 最大长度
        ESP_LOGE(TAG, "SSID too long");
        return false;
    }

    if (password.length() > 64) {
        ESP_LOGE(TAG, "Password too long");
        return false;
    }
    
    is_connecting_ = true;
    last_connected_channel_ = 0;

    // Upper-level retry loop with delay between attempts.
    //
    // Background: in APSTA mode the captive-portal session is on the AP
    // beacon channel (typically 1), and the target home AP is on some
    // other channel (e.g. 10). When ConnectToWifi triggers, esp-wifi
    // performs a Channel Switch Announcement to move both AP and STA to
    // the home AP's channel, then immediately issues an association
    // request. The home AP frequently responds with "Association
    // Response status=30 (Refused Temporarily)" + a Comeback Time in
    // TUs (= ~1.1s for Buffalo routers, observed) because its own
    // state hasn't settled yet for the new station.
    //
    // The ESP-IDF wifi driver's failure_retry_cnt issues re-association
    // attempts back-to-back (within a few ms) and does not honor the
    // 802.11 Comeback Time, so every driver-internal retry is refused
    // the same way and the first ConnectToWifi() call returns failure.
    // By the time the user clicks "submit" a second time (~8s later)
    // the AP has fully settled and association succeeds on the first
    // try — which is why users observe "it always fails the first
    // time, then works".
    //
    // Fix: when an attempt fails, wait long enough for the comeback
    // timer + AP state settle (~3s is safe), then retry once. This
    // produces a single user-visible success path instead of forcing
    // the user to resubmit. The driver-internal retries (set below
    // via failure_retry_cnt) are kept as a secondary safety net.
    constexpr int kMaxAttempts = 2;
    constexpr int kRetryDelayMs = 3000;
    bool connected = false;

    for (int attempt = 1; attempt <= kMaxAttempts && !connected; ++attempt) {
        if (attempt > 1) {
            ESP_LOGI(TAG,
                "WiFi attempt %d/%d after %d ms delay "
                "(waiting for AP comeback timer + state settle)",
                attempt, kMaxAttempts, kRetryDelayMs);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }

        xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_scan_stop();

        wifi_config_t wifi_config;
        bzero(&wifi_config, sizeof(wifi_config));
        size_t ssid_len = ssid.size();
        if (ssid_len > sizeof(wifi_config.sta.ssid)) {
            ssid_len = sizeof(wifi_config.sta.ssid);
        }
        memcpy(wifi_config.sta.ssid, ssid.data(), ssid_len);

        size_t password_len = password.size();
        if (password_len > sizeof(wifi_config.sta.password)) {
            password_len = sizeof(wifi_config.sta.password);
        }
        memcpy(wifi_config.sta.password, password.data(), password_len);
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.failure_retry_cnt = 1;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        auto ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                "esp_wifi_connect failed: %d (attempt %d/%d)",
                ret, attempt, kMaxAttempts);
            continue;
        }
        ESP_LOGI(TAG, "Connecting to WiFi %s (attempt %d/%d)",
            ssid.c_str(), attempt, kMaxAttempts);

        // Wait for the connection to complete for 10 or 25 seconds.
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE,
            pdFALSE,
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            pdMS_TO_TICKS(25000)
#else
            pdMS_TO_TICKS(10000)
#endif
        );

        if (bits & WIFI_CONNECTED_BIT) {
            connected = true;
        } else {
            const bool timed_out = (bits == 0);
            if (timed_out) {
                // Timeout — neither WIFI_CONNECTED_BIT nor WIFI_FAIL_BIT
                // was set, so WIFI_EVENT_STA_DISCONNECTED has not fired
                // and the driver may still be in `connecting` state.
                // Cancel the in-flight attempt explicitly before the
                // retry delay; without this the next esp_wifi_connect()
                // can return ESP_ERR_WIFI_STATE on a connecting-state
                // driver (per esp_wifi.h attention 3), making the retry
                // a no-op on slow / event-dropping APs.
                esp_wifi_disconnect();
            }
            ESP_LOGW(TAG,
                "Attempt %d/%d %s%s",
                attempt, kMaxAttempts,
                timed_out ? "timed out (driver may still be connecting)" : "failed",
                attempt < kMaxAttempts ? " — will retry" : "");
        }
    }
    is_connecting_ = false;

    if (connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            last_connected_channel_ = ap_info.primary;
        }
        ESP_LOGI(TAG, "Connected to WiFi %s, channel %u", ssid.c_str(), last_connected_channel_);
        esp_wifi_disconnect();
        return true;
    } else {
        ESP_LOGE(TAG,
            "Failed to connect to WiFi %s after %d attempts",
            ssid.c_str(), kMaxAttempts);
        return false;
    }
}

void WifiConfigurationAp::Save(const std::string &ssid, const std::string &password)
{
    uint8_t channel = last_connected_channel_;
    if (channel == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& rec : ap_records_) {
            if (ssid == reinterpret_cast<const char*>(rec.ssid)) {
                channel = rec.primary;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "Save SSID %s channel %u", ssid.c_str(), channel);
    SsidManager::GetInstance().AddSsid(ssid, password, channel);
}

void WifiConfigurationAp::OnExitRequested(std::function<void()> callback)
{
    on_exit_requested_ = callback;
}

void WifiConfigurationAp::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    WifiConfigurationAp* self = static_cast<WifiConfigurationAp*>(arg);
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_STA_START) {
        if (!self->is_connecting_) {
            esp_wifi_scan_start(nullptr, false);
        }
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(self->event_group_, WIFI_CONNECTED_BIT);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(self->event_group_, WIFI_FAIL_BIT);
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);

        self->ap_records_.resize(ap_num);
        esp_wifi_scan_get_ap_records(&ap_num, self->ap_records_.data());

        // 扫描完成，等待10秒后再次扫描
        if (self->scan_timer_) {
            esp_timer_stop(self->scan_timer_);
            esp_timer_start_once(self->scan_timer_, 10 * 1000000);
        }
    }
}

void WifiConfigurationAp::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    WifiConfigurationAp* self = static_cast<WifiConfigurationAp*>(arg);
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(self->event_group_, WIFI_CONNECTED_BIT);
    }
}

#if !CONFIG_IDF_TARGET_ESP32P4
void WifiConfigurationAp::StartSmartConfig()
{
    // 注册SmartConfig事件处理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                                        &WifiConfigurationAp::SmartConfigEventHandler, this, &sc_event_instance_));

    // 初始化SmartConfig配置
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    // cfg.esp_touch_v2_enable_crypt = true;
    // cfg.esp_touch_v2_key = "1234567890123456"; // 设置16字节加密密钥

    // 启动SmartConfig服务
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    ESP_LOGI(TAG, "SmartConfig started");
}

void WifiConfigurationAp::SmartConfigEventHandler(void *arg, esp_event_base_t event_base,
                                                  int32_t event_id, void *event_data)
{
    WifiConfigurationAp *self = static_cast<WifiConfigurationAp *>(arg);

    if (event_base == SC_EVENT){
        switch (event_id){
        case SC_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "SmartConfig scan done");
            break;
        case SC_EVENT_FOUND_CHANNEL:
            ESP_LOGI(TAG, "Found SmartConfig channel");
            break;
        case SC_EVENT_GOT_SSID_PSWD:{
            ESP_LOGI(TAG, "Got SmartConfig credentials");
            smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;

            char ssid[33] = {0};
            char password[65] = {0};
            memcpy(ssid, evt->ssid, sizeof(evt->ssid));
            memcpy(password, evt->password, sizeof(evt->password));
            ESP_LOGI(TAG, "SmartConfig SSID: %s, Password: %s", ssid, password);
            // 尝试连接WiFi会失败，故不连接
            self->Save(ssid, password);
            // 延迟退出配网模式
            xTaskCreate([](void *ctx){
                ESP_LOGI(TAG, "Exiting config mode in 1 second");
                vTaskDelay(pdMS_TO_TICKS(1000));
                auto* self = static_cast<WifiConfigurationAp*>(ctx);
                if (self->on_exit_requested_) {
                    self->on_exit_requested_();
                }
                vTaskDelete(NULL);
            }, "exit_config_task", 4096, self, 5, NULL);
            break;
        }
        case SC_EVENT_SEND_ACK_DONE:
            ESP_LOGI(TAG, "SmartConfig ACK sent");
            esp_smartconfig_stop();
            break;
        }
    }
}
#endif // !CONFIG_IDF_TARGET_ESP32P4

void WifiConfigurationAp::Stop() {
#if !CONFIG_IDF_TARGET_ESP32P4
    // 停止SmartConfig服务
    if (sc_event_instance_) {
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_instance_);
        sc_event_instance_ = nullptr;
    }
    esp_smartconfig_stop();
#endif

    // 停止定时器
    if (scan_timer_) {
        esp_timer_stop(scan_timer_);
        esp_timer_delete(scan_timer_);
        scan_timer_ = nullptr;
    }

    // 停止Web服务器
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }

    // 停止DNS服务器
    if (dns_server_) {
        dns_server_->Stop();
        dns_server_.reset();
    }

    // 注销事件处理器
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // 停止WiFi（但不 deinit，WiFi 驱动由 WifiManager 管理）
    esp_wifi_stop();
    
    // 销毁网络接口
    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }

    ESP_LOGI(TAG, "Wifi configuration AP stopped");
}
