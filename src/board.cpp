#include "board.h"
#include "config.h"
#include "esp_log.h"

i2c_master_bus_handle_t board_i2c_bus(void) {
    static i2c_master_bus_handle_t s_bus = NULL;
    if (s_bus == NULL) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = (gpio_num_t)PIN_I2C_SDA,
            .scl_io_num = (gpio_num_t)PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .flags = {
                .enable_internal_pullup = true,
            }
        };
        esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE("board", "I2C Bus Init failed: %s", esp_err_to_name(err));
            s_bus = NULL;
        }
    }
    return s_bus;
}
