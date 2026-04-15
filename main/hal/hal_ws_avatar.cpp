/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include "board/hal_bridge.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <board.h>
#include <web_socket.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <jpg/image_to_jpeg.h>
#include <wifi_station.h>
#include <ArduinoJson.hpp>
#include <settings.h>
#include <mutex>
#include <queue>
#include <vector>
#include <esp_heap_caps.h>
#include <display.h>
#include <lvgl_image.h>
#include "utils/jpeg_to_image/jpeg_decoder.h"
#include "audio/audio_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

static std::string _tag = "WS-Avatar";

static const std::string _setting_ns              = "stackchan";
static const std::string _setting_device_name_key = "device_name";

// 【终极绝招】：改为 true！连上 Mac 局域网后自动触发推流，无需 App，无需多余函数！
static bool _start_xiaozhi_stream_on_connect = true;

// 队列传输结构体
struct JpegData {
    uint8_t* data;
    size_t len;
};


class WebSocketAvatar {
public:
    enum class DataType : uint8_t {
        Jpeg              = 0x02,
        ControlAvatar     = 0x03,
        ControlMotion     = 0x04,
        StartCameraStream = 0x05,
        StopCameraStream  = 0x06,
        TextMessage       = 0x07,
        RequestCall       = 0x09,
        DeclineCall       = 0x0A,
        AcceptCall        = 0x0B,
        EndCall           = 0x0C,
        SetDeviceName     = 0x0D,
        GetDeviceName     = 0x0E,
        HeartbeatPing     = 0x10,
        HeartbeatPong     = 0x11,
        VideoModeOn       = 0x12,
        VideoModeOff      = 0x13,
        DanceSequence     = 0x14
    };

    struct ReceivedMessage {
        bool binary;
        std::vector<uint8_t> data;
    };

    void init() {
        _url = "ws://172.20.10.3:8080/ws";  //改为自己的热点IP地址
        connect();
    }

    void connect() {
        _websocket.reset();
        auto& board  = Board::GetInstance();
        auto network = board.GetNetwork();
        _websocket = network->CreateWebSocket(1);
        if (!_websocket) {
            ESP_LOGE(_tag.c_str(), "Failed to create websocket");
            return;
        }
        _websocket->OnConnected([this]() {
            ESP_LOGI(_tag.c_str(), "Connected to server!");
            _last_heartbeat_time = GetHAL().millis();
            _websocket->Send("{\"type\":\"hello\", \"msg\":\"Hello from StackChan!\"}");
        });
        _websocket->OnDisconnected([this]() {
            ESP_LOGE(_tag.c_str(), "Server disconnected!");
        });
        _websocket->OnData([this](const char* data, size_t len, bool binary) {
            std::lock_guard<std::mutex> lock(_mutex);
            _msg_queue.push({binary, std::vector<uint8_t>(data, data + len)});
        });
        if (!_websocket->Connect(_url.c_str())) {
            ESP_LOGE(_tag.c_str(), "Failed to connect");
        }
        _last_reconnect_attempt = GetHAL().millis();
    }

    void update() {
        if (!_websocket) return;
        if (!_websocket->IsConnected()) {
            if (GetHAL().millis() - _last_reconnect_attempt > 5000) {
                connect();
            }
        } else {
            processMessages();
            if (GetHAL().millis() - _last_heartbeat_time > 10000) {
                ESP_LOGE(_tag.c_str(), "Heartbeat timeout!");
                _last_heartbeat_time = GetHAL().millis();
                return;
            }
        }
    }

    void processMessages() {
        std::vector<ReceivedMessage> messages;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            while (!_msg_queue.empty()) {
                messages.push_back(std::move(_msg_queue.front()));
                _msg_queue.pop();
            }
        }
        for (const auto& msg : messages) {
            handleMessage(msg);
        }
    }

    void handleMessage(const ReceivedMessage& msg) {
        if (msg.binary) {
            if (msg.data.size() < 1) return;
            DataType type = static_cast<DataType>(msg.data[0]);
            switch (type) {
                case DataType::StartCameraStream:
                    setStreamingEnabled(true);
                    _websocket->Send("camera stream started");
                    break;
                case DataType::StopCameraStream:
                    setStreamingEnabled(false);
                    _websocket->Send("camera stream stopped");
                    break;
                // ...其它控制消息保持不变...
                default:
                    break;
            }
        }
    }

    bool isConnected() {
        return _websocket && _websocket->IsConnected();
    }

    void setStreamingEnabled(bool enabled) {
        if (enabled == _is_streaming) return;
        _is_streaming = enabled;
        if (enabled) {
            ESP_LOGI(_tag.c_str(), "Starting camera and websocket tasks");
            _jpeg_queue = xQueueCreate(2, sizeof(struct JpegData));
            if (_jpeg_queue == nullptr) {
                ESP_LOGE(_tag.c_str(), "Failed to create JPEG queue");
                _is_streaming = false;
                return;
            }
            // 1. 降低优先级：将视频采集和发送任务优先级降到极低（2），绝不抢占WiFi/AEC
            xTaskCreatePinnedToCore(cameraCaptureTask, "cam_cap_task", 4096, this, 2, &_camera_task_handle, 1);
            xTaskCreatePinnedToCore(websocketSendTask, "ws_send_task", 4096, this, 2, &_ws_send_task_handle, 1);
        } else {
            ESP_LOGI(_tag.c_str(), "Stopping camera and websocket tasks");
            if (_camera_task_handle != nullptr) {
                vTaskDelete(_camera_task_handle);
                _camera_task_handle = nullptr;
            }
            if (_ws_send_task_handle != nullptr) {
                vTaskDelete(_ws_send_task_handle);
                _ws_send_task_handle = nullptr;
            }
            if (_jpeg_queue != nullptr) {
                struct JpegData jpeg_item;
                while (xQueueReceive(_jpeg_queue, &jpeg_item, 0) == pdPASS) {
                    free(jpeg_item.data);
                }
                vQueueDelete(_jpeg_queue);
                _jpeg_queue = nullptr;
            }
        }
    }


    void sendPacket(DataType type, const uint8_t* data, size_t len) {
        if (!_websocket || !_websocket->IsConnected()) return;
        std::vector<uint8_t> packet;
        packet.reserve(1 + 4 + len);
        packet.push_back(static_cast<uint8_t>(type));
        uint32_t net_len = htonl((uint32_t)len);
        const uint8_t* len_ptr = (const uint8_t*)&net_len;
        packet.push_back(len_ptr[0]);
        packet.push_back(len_ptr[1]);
        packet.push_back(len_ptr[2]);
        packet.push_back(len_ptr[3]);
        if (len > 0) {
            packet.insert(packet.end(), data, data + len);
        }
        _websocket->Send(packet.data(), packet.size(), true);
    }


    static void cameraCaptureTask(void* pvParameters) {
        WebSocketAvatar* self = static_cast<WebSocketAvatar*>(pvParameters);
        ESP_LOGI(_tag.c_str(), "cameraCaptureTask started, waiting 3 seconds...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        const int jpeg_quality = 10; // 进一步降低画质，减小单帧体积
        while (true) {
            if (self->isConnected()) {
                auto camera = hal_bridge::board_get_camera();
                if (camera && camera->StreamCaptures()) {
                    const uint8_t* frameData = camera->GetFrameData();
                    size_t frameSize = camera->GetFrameSize();
                    int width = camera->GetFrameWidth();
                    int height = camera->GetFrameHeight();
                    int format = camera->GetFrameFormat();
                    uint8_t* jpeg_data = nullptr;
                    size_t jpeg_len = 0;
                    if (image_to_jpeg((uint8_t*)frameData, frameSize, width, height, (v4l2_pix_fmt_t)format, jpeg_quality, &jpeg_data, &jpeg_len)) {
                        if (jpeg_data) {
                            struct JpegData jpeg_item = {jpeg_data, jpeg_len};
                            if (xQueueSend(self->_jpeg_queue, &jpeg_item, 0) != pdPASS) {
                                ESP_LOGW(_tag.c_str(), "JPEG queue full, dropping frame");
                                free(jpeg_data);
                            }
                        }
                    }
                }
            }
            // 2. 帧率硬节流：每帧间隔 350ms，极限保护 WiFi 和 CPU 0，防止TX雪崩
            vTaskDelay(pdMS_TO_TICKS(350));
        }
    }


    static void websocketSendTask(void* pvParameters) {
        WebSocketAvatar* self = static_cast<WebSocketAvatar*>(pvParameters);
        struct JpegData jpeg_item;
        while (true) {
            if (xQueueReceive(self->_jpeg_queue, &jpeg_item, portMAX_DELAY) == pdPASS) {
                if (self->isConnected()) {
                    self->sendPacket(DataType::Jpeg, jpeg_item.data, jpeg_item.len);
                    // 3. LwIP 喘息机制：每发完一帧，强制让出 20ms，给 WiFi 协议栈和 AEC 留出喘息窗口
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                free(jpeg_item.data);
            }
        }
    }

private:
    std::unique_ptr<WebSocket> _websocket;
    std::string _url;
    uint32_t _last_reconnect_attempt = 0;
    uint32_t _last_heartbeat_time    = 0;
    bool _is_streaming               = false;
    bool _is_video_mode              = false;
    std::mutex _mutex;
    std::queue<ReceivedMessage> _msg_queue;
    QueueHandle_t _jpeg_queue = nullptr;
    TaskHandle_t _camera_task_handle = nullptr;
    TaskHandle_t _ws_send_task_handle = nullptr;
};

// =========================================================================
// 恢复新版 Mooncake 框架生命周期 (BasicAbility)
// =========================================================================
class WebsocketAvatarWorker : public mooncake::BasicAbility {
public:
    WebsocketAvatarWorker()
    {
        _service = std::make_unique<WebSocketAvatar>();
        _service->init();
    }

    void onCreate() override {}

    void onRunning() override
    {
        if (GetHAL().millis() - _last_tick < 20) {
            return;
        }
        _last_tick = GetHAL().millis();
        
        if (_service) {
            _service->update();

            if (_start_xiaozhi_stream_on_connect && _service->isConnected()) {
                ESP_LOGI("WS-Avatar", "Connected! Auto-starting streaming to Mac...");
                _service->setStreamingEnabled(true);
                _start_xiaozhi_stream_on_connect = false; 
            }
        }
    }

    void onDestroy() override
    {
        _service.reset();
    }

private:
    std::unique_ptr<WebSocketAvatar> _service;
    uint32_t _last_tick = 0;
};

// =========================================================================
// HAL 全局接口暴露区
// 【终极修复】：只保留 hal.h 中真正存在的唯一接口，删掉其它所有私造函数！
// =========================================================================
void Hal::startWebSocketAvatar()
{
    ESP_LOGI("WS-Avatar", "Legacy startWebSocketAvatar called");
    mooncake::GetMooncake().extensionManager()->createAbility(std::make_unique<WebsocketAvatarWorker>());
}

// =========================================================================
// =========================================================================
// 终极钩子：完全脱离 UI 线程的独立后台推流任务
// =========================================================================
static bool _mac_stream_running = false;
static WebSocketAvatar* _global_ws_avatar = nullptr;



static void mac_stream_task(void* param)
{
    ESP_LOGI("MAC-STREAM", "Background task started, waiting for WiFi...");
    while (!WifiStation::GetInstance().IsConnected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI("MAC-STREAM", "WiFi ready! Initializing WebSocket connection...");
    _global_ws_avatar = new WebSocketAvatar();
    _global_ws_avatar->init();
    _global_ws_avatar->setStreamingEnabled(true);
    while (true) {
        _global_ws_avatar->update();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}



void start_mac_video_stream()
{
    if (_mac_stream_running) return;
    _mac_stream_running = true;
    ESP_LOGI("MAC-STREAM", "Hook triggered! Spawning independent FreeRTOS task...");
    xTaskCreatePinnedToCore(mac_stream_task, "mac_stream", 8192, nullptr, 5, nullptr, 1);
}
