#pragma once
#include "driver/i2c_master.h"

// The one I2C bus of the board (PIN_I2C_SDA / PIN_I2C_SCL), created on first use.
// Shared by the accelerometer and, where present, the audio codec.
// Returns NULL if the bus could not be created.
i2c_master_bus_handle_t board_i2c_bus(void);
