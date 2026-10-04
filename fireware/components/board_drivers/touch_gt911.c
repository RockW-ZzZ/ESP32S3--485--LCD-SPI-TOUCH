#include "touch_gt911.h"
#include "board_display.h"
#include "board_pins.h"
#include "touch_mapping.h"
#include "touch_gate.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t device;
static SemaphoreHandle_t lock;
static uint16_t x_resolution, y_resolution;
static bool ready;
static touch_gate_t gate = {.enabled = true};

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static esp_err_t read_reg(uint16_t reg, void *data, size_t length)
{
    uint8_t address[] = {reg >> 8, reg};
    return i2c_master_transmit_receive(device, address, 2, data, length, 50);
}
static esp_err_t acknowledge(void)
{
    const uint8_t data[] = {0x81, 0x4E, 0x00};
    return i2c_master_transmit(device, data, sizeof(data), 50);
}

esp_err_t touch_init(void)
{
    if (ready) return ESP_OK;
    esp_err_t err = board_display_prepare();
    if (err != ESP_OK) return err;
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = BOARD_TOUCH_SDA,
        .scl_io_num = BOARD_TOUCH_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) goto fail;
    uint8_t address = BOARD_TOUCH_ADDR;
    err = i2c_master_probe(bus, address, 50);
    if (err != ESP_OK) { address = 0x14; err = i2c_master_probe(bus, address, 50); }
    if (err != ESP_OK) goto fail;
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address,
        .scl_speed_hz = 100000, /* Module has 4.7k pull-ups. */
    };
    err = i2c_master_bus_add_device(bus, &dev, &device);
    if (err != ESP_OK) goto fail;
    uint8_t info[10] = {0};
    err = read_reg(0x8140, info, sizeof(info));
    if (err != ESP_OK) goto fail;
    if (memcmp(info, "911", 3) != 0) { err = ESP_ERR_NOT_SUPPORTED; goto fail; }
    x_resolution = le16(info + 6);
    y_resolution = le16(info + 8);
    if (x_resolution < 2 || y_resolution < 2) { err = ESP_ERR_INVALID_RESPONSE; goto fail; }
    err = acknowledge();
    if (err != ESP_OK) goto fail;
    ready = true;
    ESP_LOGI("gt911", "ID=%.4s address=0x%02x firmware=0x%04x resolution=%ux%u",
             (char *)info, address, le16(info + 4), x_resolution, y_resolution);
    return ESP_OK;
fail:
    if (device) { i2c_master_bus_rm_device(device); device = NULL; }
    if (bus) { i2c_del_master_bus(bus); bus = NULL; }
    vSemaphoreDelete(lock); lock = NULL;
    return err;
}

esp_err_t touch_set_enabled(bool enabled)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (enabled && !gate.enabled) {
        /* Discard a frame that arrived since the last disabled poll, so it
         * cannot become a fresh click on re-enable. No coordinates are needed. */
        uint8_t status;
        err = read_reg(0x814E, &status, 1);
        if (err == ESP_OK && (status & 0x80)) {
            if ((status & 0x0F) > TOUCH_MAX_POINTS) err = ESP_ERR_INVALID_RESPONSE;
            else {
                err = acknowledge();
                if (err == ESP_OK) gate.physical_down = (status & 0x0F) != 0;
            }
        }
    }
    if (err == ESP_OK) touch_gate_set(&gate, enabled);
    xSemaphoreGive(lock);
    return err;
}

bool touch_is_enabled(void)
{
    if (!ready) return true;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool enabled = gate.enabled;
    xSemaphoreGive(lock);
    return enabled;
}

esp_err_t touch_read(touch_frame_t *frame)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    memset(frame, 0, sizeof(*frame));
    if (!ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    uint8_t status = 0, data[TOUCH_MAX_POINTS * 8];
    esp_err_t err = read_reg(0x814E, &status, 1);
    if (err != ESP_OK || !(status & 0x80)) goto done;
    unsigned count = status & 0x0F;
    if (count > TOUCH_MAX_POINTS) {
        (void)acknowledge(); err = ESP_ERR_INVALID_RESPONSE; goto done;
    }
    if (count) {
        err = read_reg(0x814F, data, count * 8);
        if (err != ESP_OK) goto done; /* Do not acknowledge incomplete I2C reads. */
    }
    err = acknowledge(); /* Clear ready only AFTER all point data was read. */
    if (err != ESP_OK) goto done;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *p = data + i * 8;
        bool swap_xy = false, mirror_x = false, mirror_y = false;
#ifdef CONFIG_BOARD_TOUCH_SWAP_XY
        swap_xy = true;
#endif
#ifdef CONFIG_BOARD_TOUCH_MIRROR_X
        mirror_x = true;
#endif
#ifdef CONFIG_BOARD_TOUCH_MIRROR_Y
        mirror_y = true;
#endif
        uint16_t x, y;
        if (!touch_map_landscape(le16(p + 1), le16(p + 3), x_resolution, y_resolution,
                                 swap_xy, mirror_x, mirror_y, &x, &y)) {
            err = ESP_ERR_INVALID_RESPONSE; goto done;
        }
        frame->points[i] = (touch_point_t){.id = p[0], .x = x, .y = y, .size = le16(p + 5)};
    }
    frame->count = count;
    frame->updated = true;
done:
    if (touch_gate_filter(&gate, err == ESP_OK && frame->updated, frame->count, &frame->input_reset)) {
        frame->updated = true;
        frame->count = 0;
        memset(frame->points, 0, sizeof(frame->points));
    }
    xSemaphoreGive(lock);
    return err;
}
