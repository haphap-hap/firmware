/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "stackchan_camera.h"
#include <lvgl.h>
#include <driver/i2c_master.h>
#include <string_view>

namespace hal_bridge {

struct TouchPoint_t {
    int num = 0;
    int x   = -1;
    int y   = -1;
};

struct Data_t {
    TouchPoint_t touchPoint;
    bool isXiaozhiMode = false;
};

void lock();
void unlock();
Data_t& get_data();

void set_touch_point(int num, int x, int y);
TouchPoint_t get_touch_point();

bool is_xiaozhi_mode();
void set_xiaozhi_mode(bool mode);

void disply_lvgl_lock();
void disply_lvgl_unlock();
lv_disp_t* display_get_lvgl_display();
void display_setup_xiaozhi_ui();

void xiaozhi_board_init();
void start_xiaozhi_app();

i2c_master_bus_handle_t board_get_i2c_bus();

#if !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
// StackChanCamera is a global class defined in stackchan_camera.h.
class ::StackChanCamera;
::StackChanCamera* board_get_camera();
#endif

// For ESP32 and ESP32S3, provide a null implementation
inline auto board_get_camera() { return nullptr; }

void app_play_sound(const std::string_view& sound);

}  // namespace hal_bridge
