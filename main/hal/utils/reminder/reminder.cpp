/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "reminder.h"
#include <esp_log.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <application.h>
#include <board.h>
#include <display.h>
#include <ctime>
#include <stackchan/stackchan.h>
#include <stackchan/modifiers/speaking.h>
#include <assets/lang_config.h>

static const char* TAG = "ReminderManager";

static void PlayDigitSound(Application& app, char digit)
{
    switch (digit) {
        case '0': app.PlaySound(Lang::Sounds::OGG_0); break;
        case '1': app.PlaySound(Lang::Sounds::OGG_1); break;
        case '2': app.PlaySound(Lang::Sounds::OGG_2); break;
        case '3': app.PlaySound(Lang::Sounds::OGG_3); break;
        case '4': app.PlaySound(Lang::Sounds::OGG_4); break;
        case '5': app.PlaySound(Lang::Sounds::OGG_5); break;
        case '6': app.PlaySound(Lang::Sounds::OGG_6); break;
        case '7': app.PlaySound(Lang::Sounds::OGG_7); break;
        case '8': app.PlaySound(Lang::Sounds::OGG_8); break;
        case '9': app.PlaySound(Lang::Sounds::OGG_9); break;
        default: break;
    }
}

ReminderItem::ReminderItem(int duration_s, const std::string& msg) : message_(msg)
{
    target_steady_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
}

ReminderItem::ReminderItem(std::time_t epoch_seconds, const std::string& msg) : message_(msg)
{
    target_system_time_ = std::chrono::system_clock::from_time_t(epoch_seconds);
}

bool ReminderItem::IsDue() const
{
    if (target_steady_time_.has_value()) {
        return std::chrono::steady_clock::now() >= target_steady_time_.value();
    }
    if (target_system_time_.has_value()) {
        return std::chrono::system_clock::now() >= target_system_time_.value();
    }
    return false;
}

ReminderManager& ReminderManager::GetInstance()
{
    static ReminderManager instance;
    return instance;
}

ReminderManager::ReminderManager()
{
    mutex_ = xSemaphoreCreateMutex();
}

ReminderManager::~ReminderManager()
{
    running_ = false;
    // 等待任务结束（简单处理，实际可能需要更复杂的同步）
    if (worker_task_handle_) {
        // vTaskDelete(worker_task_handle_); // 不建议直接删除，最好让任务自己退出
        // 这里我们假设任务会检测 running_ 并退出
        int timeout = 100;
        while (eTaskGetState(worker_task_handle_) != eDeleted && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

void ReminderManager::Start()
{
    if (running_) return;
    running_ = true;
    xTaskCreate(
        [](void* arg) {
            ReminderManager* mgr = (ReminderManager*)arg;
            mgr->WorkerThread();
            vTaskDelete(NULL);
        },
        "reminder_worker", 4096, this, 5, &worker_task_handle_);
    ESP_LOGI(TAG, "ReminderManager started");
}

int ReminderManager::CreateReminder(int duration_s, const std::string& message)
{
    if (duration_s <= 0) {
        ESP_LOGE(TAG, "CreateReminder failed: duration_s must be > 0");
        return -1;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    auto item = std::make_unique<ReminderItem>(duration_s, message);
    int id    = pool_.create(std::move(item));
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "Created reminder ID: %d, Duration: %ds, Msg: %s", id, duration_s, message.c_str());
    return id;
}

int ReminderManager::CreateReminderAtEpochSeconds(int epoch_seconds, const std::string& message)
{
    std::time_t now = std::time(nullptr);
    if (epoch_seconds <= now) {
        ESP_LOGE(TAG, "CreateReminderAtEpochSeconds failed: time must be in the future");
        return -1;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    auto item = std::make_unique<ReminderItem>(static_cast<std::time_t>(epoch_seconds), message);
    int id    = pool_.create(std::move(item));
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "Created reminder ID: %d, At: %d, Msg: %s", id, epoch_seconds, message.c_str());
    return id;
}

void ReminderManager::StopReminder(int id)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);

    // 如果正在响铃的是这个提醒，停止播放
    if (id == ringing_id_) {
        ringing_id_ = -1;
    }

    // 标记销毁
    auto* item = pool_.get(id);
    if (item) {
        item->requestDestroy();
        ESP_LOGI(TAG, "Stopped reminder ID: %d", id);
    }

    // 立即清理（或者等待 WorkerThread 清理也可以，这里立即清理更及时）
    pool_.destroy(id);
    xSemaphoreGive(mutex_);
}

void ReminderManager::WorkerThread()
{
    std::vector<std::pair<int, std::string>> triggered_list;
    while (running_) {
        triggered_list.clear();

        {
            xSemaphoreTake(mutex_, portMAX_DELAY);

            // 1. 检查所有提醒
            pool_.forEach([&](ReminderItem* item, int id) {
                if (!item->IsTriggered() && item->IsDue()) {
                    item->SetTriggered(true);
                    triggered_list.push_back({id, item->GetMessage()});
                }
            });

            // 2. 清理已销毁的对象
            pool_.cleanup();
            xSemaphoreGive(mutex_);
        }

        // 3. 处理触发的提醒（在锁外执行，防止死锁）
        for (const auto& pair : triggered_list) {
            int id                 = pair.first;
            const std::string& msg = pair.second;

            ESP_LOGI(TAG, "Reminder triggered! ID: %d, Msg: %s", id, msg.c_str());

            // 更新响铃 ID
            {
                xSemaphoreTake(mutex_, portMAX_DELAY);
                // 再次检查对象是否存在（可能在触发前一瞬间被删除了）
                if (pool_.get(id) == nullptr) {
                    xSemaphoreGive(mutex_);
                    continue;
                }
                ringing_id_ = id;
                xSemaphoreGive(mutex_);
            }

            auto& app = Application::GetInstance();
            app.Schedule([msg]() {
                auto display = Board::GetInstance().GetDisplay();
                std::string reminder_text = std::string("提醒: ") + msg;
                display->ShowNotification(reminder_text.c_str(), 10000);
                display->SetChatMessage("system", reminder_text.c_str());

                // Add speaking animation so reminder feels like voice notification.
                auto& stackchan = GetStackChan();
                if (stackchan.hasAvatar()) {
                    stackchan.addModifier(std::make_unique<stackchan::SpeakingModifier>(3000, 150, false));
                }

                // Voice-style reminder: play time digits (HHMM) using existing number OGG assets.
                auto& app = Application::GetInstance();
                std::time_t now = std::time(nullptr);
                std::tm local_tm{};
                localtime_r(&now, &local_tm);
                char hhmm[5] = {0};
                std::strftime(hhmm, sizeof(hhmm), "%H%M", &local_tm);

                app.PlaySound(Lang::Sounds::OGG_POPUP);
                for (char c : hhmm) {
                    PlayDigitSound(app, c);
                }
                app.PlaySound(Lang::Sounds::OGG_EXCLAMATION);

                if (!OGG_CAMERA_SHUTTER.empty()) {
                    Application::GetInstance().PlaySound(OGG_CAMERA_SHUTTER);
                }
            });

            // 发出信号
            GetHAL().onReminderTriggered.emit(id, msg);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
