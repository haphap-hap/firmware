/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <ctime>
#include <cstdio>
#include <stdexcept>

using namespace stackchan;

static const std::string _tag = "HAL-MCP";

void Hal::xiaozhi_mcp_init()
{
    mclog::tagInfo(_tag, "init");

    // https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md
    auto& mcp_server = McpServer::GetInstance();

    mclog::tagInfo(_tag, "add motion.set_angles tool");
    mcp_server.AddTool(
        "self.motion.set_angles",
        "Set the angles of the robot's servos (head movement). Yaw controls left/right (-1280 to "
        "1280), Pitch controls up/down (0 to 900). Note: 10 units equals 1 degree. For natural movement, prefer angles "
        "within +/- 300 (30 degrees) unless instructed otherwise. Speed controls the movement speed (100-1000).",
        PropertyList({Property("yaw", kPropertyTypeInteger, 0, -1280, 1280),
                      Property("pitch", kPropertyTypeInteger, 0, 0, 900),
                      Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int pitch = properties["pitch"].value<int>();
            int yaw   = properties["yaw"].value<int>();
            int speed = properties["speed"].value<int>();

            mclog::tagInfo(_tag, "motion set angles: pitch: {}, yaw: {}, speed: {}", pitch, yaw, speed);

            auto& motion = GetStackChan().motion();
            motion.pitchServo().moveWithSpeed(pitch, speed);
            motion.yawServo().moveWithSpeed(yaw, speed);

            return true;
        });

    // No LED strip hardware on this device, so we do not register LED control tools.

    mclog::tagInfo(_tag, "add reminder tools");
    mcp_server.AddTool(
        "self.reminder.create_after",
        "Create a reminder after delay_seconds. Use this when user asks reminders like 'in 10 minutes'.",
        PropertyList({
            Property("delay_seconds", kPropertyTypeInteger, 1, 1, 86400 * 7),
            Property("message", kPropertyTypeString),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int delay_seconds = properties["delay_seconds"].value<int>();
            auto message      = properties["message"].value<std::string>();
            int id            = GetHAL().createReminder(delay_seconds, message);
            if (id < 0) {
                throw std::runtime_error("Failed to create reminder");
            }
            return std::string("ok:id=") + std::to_string(id);
        });

    mcp_server.AddTool(
        "self.reminder.set_alarm",
        "Set a fixed-time alarm in local clock time. IMPORTANT: prioritize this tool when the user asks alarms like '16:20', '9:00', or 'tonight 9 PM'.",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("message", kPropertyTypeString, std::string("时间到了")),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!GetHAL().isSystemTimeValid()) {
                throw std::runtime_error("System time is not ready, please sync time first");
            }

            auto time_text = properties["time"].value<std::string>();
            auto message   = properties["message"].value<std::string>();

            int hour = -1;
            int minute = -1;
            if (std::sscanf(time_text.c_str(), "%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                throw std::runtime_error("Invalid time format, expected HH:MM (24-hour), e.g. 16:20");
            }

            std::time_t now = std::time(nullptr);
            std::tm local_tm{};
            localtime_r(&now, &local_tm);
            local_tm.tm_hour = hour;
            local_tm.tm_min = minute;
            local_tm.tm_sec = 0;

            std::time_t target = std::mktime(&local_tm);
            if (target <= now) {
                local_tm.tm_mday += 1;
                target = std::mktime(&local_tm);
            }

            int id = GetHAL().createReminderAtEpochSeconds(static_cast<int>(target), message);
            if (id < 0) {
                throw std::runtime_error("Failed to create reminder");
            }

            return std::string("ok:id=") + std::to_string(id) + ",target=" + std::to_string(static_cast<int>(target));
        });

    mcp_server.AddTool(
        "self.reminder.create_at",
        "Create a reminder at absolute unix epoch seconds. Use this when target time has been resolved already.",
        PropertyList({
            Property("epoch_seconds", kPropertyTypeInteger),
            Property("message", kPropertyTypeString),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!GetHAL().isSystemTimeValid()) {
                throw std::runtime_error("System time is not ready, please sync time first");
            }

            int epoch_seconds = properties["epoch_seconds"].value<int>();
            std::time_t now   = std::time(nullptr);
            if (epoch_seconds <= now) {
                throw std::runtime_error("epoch_seconds must be in the future");
            }

            auto message = properties["message"].value<std::string>();
            int id       = GetHAL().createReminderAtEpochSeconds(epoch_seconds, message);
            if (id < 0) {
                throw std::runtime_error("Failed to create reminder");
            }
            return std::string("ok:id=") + std::to_string(id);
        });

    mcp_server.AddTool(
        "self.reminder.cancel",
        "Cancel a reminder by ID.",
        PropertyList({
            Property("id", kPropertyTypeInteger),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int id = properties["id"].value<int>();
            GetHAL().stopReminder(id);
            return true;
        });
}
