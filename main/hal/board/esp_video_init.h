/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <driver/i2c_master.h>

/**
 * @brief DVP camera pin data width
 */
typedef enum {
    CAM_CTLR_DATA_WIDTH_8 = 0,
    CAM_CTLR_DATA_WIDTH_16,
} cam_ctlr_data_width_t;

/**
 * @brief DVP camera controller pin configuration
 */
typedef struct {
    cam_ctlr_data_width_t data_width;
    uint32_t data_io[16];
    uint32_t vsync_io;
    uint32_t de_io;
    uint32_t pclk_io;
    uint32_t xclk_io;
} esp_cam_ctlr_dvp_pin_config_t;

/**
 * @brief I2C camera SCCB configuration
 */
typedef struct {
    bool init_sccb;
    union {
        i2c_master_bus_handle_t i2c_handle;
        struct {
            uint16_t port;
            uint32_t scl_pin;
            uint32_t sda_pin;
        } i2c_config;
    };
    uint32_t freq;
} esp_video_init_sccb_config_t;

/**
 * @brief DVP video initialization configuration
 */
typedef struct {
    esp_video_init_sccb_config_t sccb_config;
    uint32_t reset_pin;
    uint32_t pwdn_pin;
    esp_cam_ctlr_dvp_pin_config_t dvp_pin;
    uint32_t xclk_freq;
} esp_video_init_dvp_config_t;

/**
 * @brief MIPI CSI video initialization configuration
 */
typedef struct {
    esp_video_init_sccb_config_t sccb_config;
    uint32_t reset_pin;
    uint32_t pwdn_pin;
    uint32_t xclk_freq;
} esp_video_init_csi_config_t;

/**
 * @brief JPEG video initialization configuration
 */
typedef struct {
    esp_video_init_sccb_config_t sccb_config;
    uint32_t reset_pin;
    uint32_t pwdn_pin;
    esp_cam_ctlr_dvp_pin_config_t dvp_pin;
    uint32_t xclk_freq;
} esp_video_init_jpeg_config_t;

/**
 * @brief USB UVC video initialization configuration
 */
typedef struct {
    struct {
        uint32_t uvc_dev_num;
        uint32_t task_stack;
        uint32_t task_priority;
        int32_t task_affinity;
    } uvc;
    struct {
        bool init_usb_host_lib;
        uint32_t task_stack;
        uint32_t task_priority;
        int32_t task_affinity;
    } usb;
} esp_video_init_usb_uvc_config_t;

/**
 * @brief Video device names
 */
#define ESP_VIDEO_DVP_DEVICE_NAME           "/dev/video0"
#define ESP_VIDEO_MIPI_CSI_DEVICE_NAME      "/dev/video1"
#define ESP_VIDEO_JPEG_DEVICE_NAME          "/dev/video2"
#define ESP_VIDEO_USB_UVC_DEVICE_NAME       "/dev/video3"

/**
 * @brief Main video initialization configuration
 */
typedef struct {
    esp_video_init_dvp_config_t *dvp;
    esp_video_init_csi_config_t *csi;
    esp_video_init_jpeg_config_t *jpeg;
    esp_video_init_usb_uvc_config_t *usb_uvc;
} esp_video_init_config_t;

/**
 * @brief Initialize ESP video framework
 *
 * @param config Pointer to video initialization configuration
 * @return ESP_OK on success, otherwise an error code
 */
int esp_video_init(const esp_video_init_config_t *config);

#ifdef __cplusplus
}
#endif
