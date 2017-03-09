/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * resarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>
#include <errno.h>
#include <assert.h>

#include "defs/error.h"
#include "os/os.h"
#include "sysinit/sysinit.h"
#include "hal/hal_i2c.h"
#include "sensor/sensor.h"
#include "tcs34725/tcs34725.h"
#include "tcs34725_priv.h"
#include "sensor/color.h"

#if MYNEWT_VAL(TCS34725_LOG)
#include "log/log.h"
#endif

#if MYNEWT_VAL(TCS34725_STATS)
#include "stats/stats.h"
#endif

#if MYNEWT_VAL(TCS34725_STATS)
/* Define the stats section and records */
STATS_SECT_START(tcs34725_stat_section)
    STATS_SECT_ENTRY(samples_2_4ms)
    STATS_SECT_ENTRY(samples_24ms)
    STATS_SECT_ENTRY(samples_50ms)
    STATS_SECT_ENTRY(samples_101ms)
    STATS_SECT_ENTRY(samples_154ms)
    STATS_SECT_ENTRY(samples_700ms)
    STATS_SECT_ENTRY(samples_userdef)
    STATS_SECT_ENTRY(errors)
STATS_SECT_END

/* Define stat names for querying */
STATS_NAME_START(tcs34725_stat_section)
    STATS_NAME(tcs34725_stat_section, samples_2_4ms)
    STATS_NAME(tcs34725_stat_section, samples_24ms)
    STATS_NAME(tcs34725_stat_section, samples_50ms)
    STATS_NAME(tcs34725_stat_section, samples_101ms)
    STATS_NAME(tcs34725_stat_section, samples_154ms)
    STATS_NAME(tcs34725_stat_section, samples_700ms)
    STATS_NAME(tcs34725_stat_section, samples_userdef)
    STATS_NAME(tcs34725_stat_section, errors)
STATS_NAME_END(tcs34725_stat_section)

/* Global variable used to hold stats data */
STATS_SECT_DECL(tcs34725_stat_section) g_tcs34725stats;
#endif

#if MYNEWT_VAL(TCS34725_LOG)
#define LOG_MODULE_TCS34725 (307)
#define TCS34725_INFO(...)  LOG_INFO(&_log, LOG_MODULE_TCS34725, __VA_ARGS__)
#define TCS34725_ERR(...)   LOG_ERROR(&_log, LOG_MODULE_TCS34725, __VA_ARGS__)
static struct log _log;
#else
#define TCS34725_INFO(...)
#define TCS34725_ERR(...)
#endif

/* Exports for the sensor interface.
 */
static void *tcs34725_sensor_get_interface(struct sensor *, sensor_type_t);
static int tcs34725_sensor_read(struct sensor *, sensor_type_t,
        sensor_data_func_t, void *, uint32_t);
static int tcs34725_sensor_get_config(struct sensor *, sensor_type_t,
        struct sensor_cfg *);

static const struct sensor_driver g_tcs34725_sensor_driver = {
    tcs34725_sensor_get_interface,
    tcs34725_sensor_read,
    tcs34725_sensor_get_config
};

uint8_t g_tcs34725_gain;
uint8_t g_tcs34725_integration_time;
uint8_t g_tcs34725_enabled;

/**
 * Writes a single byte to the specified register
 *
 * @param The register address to write to
 * @param The value to write
 *
 * @return 0 on success, non-zero error on failure.
 */
int
tcs34725_write8(uint8_t reg, uint32_t value)
{
    int rc;
    uint8_t payload[2] = { reg | TCS34725_COMMAND_BIT, value & 0xFF };

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(TCS34725_I2CADDR),
        .len = 2,
        .buffer = payload
    };

    rc = hal_i2c_master_write(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 1);
    if (rc) {
        TCS34725_ERR("Failed to write to 0x%02X:0x%02X with value 0x%02X\n",
                       addr, reg, value);
#if MYNEWT_VAL(TCS34725_STATS)
        STATS_INC(g_tcs34725stats, errors);
#endif
    }

    return rc;
}

/**
 * Reads a single byte from the specified register
 *
 * @param The register address to read from
 * @param Pointer to where the register value should be written
 *
 * @return 0 on success, non-zero error on failure.
 */
int
tcs34725_read8(uint8_t reg, uint8_t *value)
{
    int rc;
    uint8_t payload;

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(TCS34725_I2CADDR),
        .len = 1,
        .buffer = &payload
    };

    /* Register write */
    payload = reg | TCS34725_COMMAND_BIT;
    rc = hal_i2c_master_write(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 1);
    if (rc) {
        TCS34725_ERR("I2C access failed at address 0x%02X\n", addr);
#if MYNEWT_VAL(TCS34725_STATS)
        STATS_INC(g_tcs34725stats, errors);
#endif
        goto error;
    }

    /* Read one byte back */
    payload = 0;
    rc = hal_i2c_master_read(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                             OS_TICKS_PER_SEC / 10, 1);
    *value = payload;
    if (rc) {
        TCS34725_ERR("Failed to read from 0x%02X:0x%02X\n", addr, reg);
#if MYNEWT_VAL(TCS34725_STATS)
        STATS_INC(g_tcs34725stats, errors);
#endif
    }

error:
    return rc;
}

/**
 * Read data from the sensor of variable length (MAX: 8 bytes)
 *
 * @param Register to read from
 * @param Bufer to read into
 * @param Length of the buffer
 *
 * @return 0 on success and non-zero on failure
 */
int
tcs34725_readlen(uint8_t reg, uint8_t *buffer, uint8_t len)
{
    int rc;
    uint8_t payload[9] = { reg | TCS34725_COMMAND_BIT, 0, 0, 0, 0, 0, 0, 0, 0};

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(TCS34725_I2CADDR),
        .len = 1,
        .buffer = payload
    };

    /* Clear the supplied buffer */
    memset(buffer, 0, len);

    /* Register write */
    rc = hal_i2c_master_write(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 1);
    if (rc) {
        TCS34725_ERR("I2C access failed at address 0x%02X\n", addr);
#if MYNEWT_VAL(TCS34725_STATS)
        STATS_INC(g_tcs34725stats, errors);
#endif
        goto err;
    }

    /* Read len bytes back */
    memset(payload, 0, sizeof(payload));
    data_struct.len = len;
    rc = hal_i2c_master_read(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                             OS_TICKS_PER_SEC / 10, 1);

    if (rc) {
        TCS34725_ERR("Failed to read from 0x%02X:0x%02X\n", addr, reg);
#if MYNEWT_VAL(TCS34725_STATS)
        STATS_INC(g_tcs34725stats, errors);
#endif
        goto err;
    }

    /* Copy the I2C results into the supplied buffer */
    memcpy(buffer, payload, len);

    return 0;
err:
    return rc;
}

/**
 * Writes a multiple bytes to the specified register
 *
 * @param The register address to write to
 * @param The data buffer to write from
 *
 * @return 0 on success, non-zero error on failure.
 */
int
tcs34725_writelen(uint8_t reg, uint8_t *buffer, uint8_t len)
{
    int rc;
    uint8_t payload[9] = { reg, 0, 0, 0, 0, 0, 0, 0, 0};

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(TCS34725_I2CADDR),
        .len = 1,
        .buffer = payload
    };

    memcpy(&payload[1], buffer, len);

    /* Register write */
    rc = hal_i2c_master_write(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 1);
    if (rc) {
        TCS34725_ERR("I2C access failed at address 0x%02X\n", addr);
#if MYNEWT_VAL(TCS34725_STATS)
        STATS_INC(g_tcs34725stats, errors);
#endif
        goto err;
    }

    memset(payload, 0, sizeof(payload));
    data_struct.len = len;
    rc = hal_i2c_master_write(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, len);

    if (rc) {
        TCS34725_ERR("Failed to read from 0x%02X:0x%02X\n", addr, reg);
#if MYNEWT_VAL(TCS34725_STATS)
        STATS_INC(g_tcs34725stats, errors);
#endif
        goto err;
    }

    return 0;
err:
    return rc;
}


#if MYNEWT_VAL(USE_MATH)
/**
 * Float power function
 *
 * @param float base
 * @param float exponent
 */
static float
powf(float base, float exp)
{
    return (float)(pow((double)base, (double)exp));
}

#endif

/**
 *
 * Enables the device
 *
 * @param enable/disable
 * @return 0 on success, non-zero on error
 */
int
tcs34725_enable(uint8_t enable)
{
    int rc;
    uint8_t reg;

    rc = tcs34725_read8(TCS34725_REG_ENABLE, &reg);
    if (rc) {
        goto err;
    }

    os_time_delay((3 * OS_TICKS_PER_SEC)/1000 + 1);

    if (enable) {
        rc = tcs34725_write8(TCS34725_REG_ENABLE, reg | TCS34725_ENABLE_PON | TCS34725_ENABLE_AEN);
        if (rc) {
            goto err;
        }
    } else {
        rc = tcs34725_write8(TCS34725_REG_ENABLE, reg & ~(TCS34725_ENABLE_PON | TCS34725_ENABLE_AEN));
        if (rc) {
            goto err;
        }
    }

    g_tcs34725_enabled = enable;

    return 0;
err:
    return rc;
}

/**
 * Expects to be called back through os_dev_create().
 *
 * @param The device object associated with this accellerometer
 * @param Argument passed to OS device init, unused
 *
 * @return 0 on success, non-zero error on failure.
 */
int
tcs34725_init(struct os_dev *dev, void *arg)
{
    struct tcs34725 *tcs34725;
    struct sensor *sensor;
    int rc;

    tcs34725 = (struct tcs34725 *) dev;

#if MYNEWT_VAL(TCS34725_LOG)
    log_register("tcs34725", &_log, &log_console_handler, NULL, LOG_SYSLEVEL);
#endif

    sensor = &tcs34725->sensor;

#if MYNEWT_VAL(TCS34725_STATS)
    /* Initialise the stats entry */
    rc = stats_init(
        STATS_HDR(g_tcs34725stats),
        STATS_SIZE_INIT_PARMS(g_tcs34725stats, STATS_SIZE_32),
        STATS_NAME_INIT_PARMS(tcs34725_stat_section));
    SYSINIT_PANIC_ASSERT(rc == 0);
    /* Register the entry with the stats registry */
    rc = stats_register("tcs34725", STATS_HDR(g_tcs34725stats));
    SYSINIT_PANIC_ASSERT(rc == 0);
#endif

    rc = sensor_init(sensor, dev);
    if (rc != 0) {
        goto err;
    }

    /* Add the color sensor driver */
    rc = sensor_set_driver(sensor, SENSOR_TYPE_COLOR,
                           (struct sensor_driver *) &g_tcs34725_sensor_driver);
    if (rc != 0) {
        goto err;
    }

    rc = sensor_mgr_register(sensor);
    if (rc != 0) {
        goto err;
    }

    return (0);
err:
    return (rc);
}

/**
 * Indicates whether the sensor is enabled or not
 *
 * @return 1 if enabled, 0 if disabled
 */
uint8_t
tcs34725_get_enable (void)
{
    return g_tcs34725_enabled;
}

/**
 * Sets integration time
 *
 * @param integration time to be set
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_set_integration_time(uint8_t int_time)
{
    int rc;

    rc = tcs34725_write8(TCS34725_REG_ATIME,
                         int_time | g_tcs34725_gain);
    if (rc) {
        goto err;
    }

    g_tcs34725_integration_time = int_time;

err:
    return rc;
}

/**
 * Gets integration time set earlier
 *
 * @return integration time
 */
uint8_t
tcs34725_get_integration_time(void)
{
    return g_tcs34725_integration_time;
}

/**
 * Set gain of the sensor
 *
 * @param gain
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_set_gain(uint8_t gain)
{
    int rc;

    if (gain > TCS34725_GAIN_60X) {
        TCS34725_ERR("Invalid gain value\n");
        rc = SYS_EINVAL;
        goto err;
    }

    rc = tcs34725_write8(TCS34725_REG_CONTROL,
                        g_tcs34725_integration_time | gain);
    if (rc) {
        goto err;
    }

    g_tcs34725_gain = gain;

err:
    return rc;
}

/**
 * Get gain of the sensor
 *
 * @return gain
 */
uint8_t
tcs34725_get_gain(void)
{
    return g_tcs34725_gain;
}

/**
 * Get chip ID from the sensor
 *
 * @param Pointer to the variable to fill up chip ID in
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_get_chip_id(uint8_t *id)
{
    int rc;
    uint8_t idtmp;

    /* Check if we can read the chip address */
    rc = tcs34725_read8(TCS34725_REG_ID, &idtmp);
    if (rc) {
        goto err;
    }

    *id = idtmp;

    return 0;
err:
    return rc;
}

/**
 * Configure the sensor
 *
 * @param ptr to the sensor
 * @param ptr to sensor config
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_config(struct tcs34725 *tcs34725, struct tcs34725_cfg *cfg)
{
    int rc;
    uint8_t id;

    rc = tcs34725_get_chip_id(&id);
    if (id != TCS34725_ID || rc != 0) {
        rc = SYS_EINVAL;
        goto err;
    }

    rc |= tcs34725_enable(1);

    rc |= tcs34725_set_integration_time(cfg->integration_time);

    rc |= tcs34725_set_gain(cfg->gain);
    if (rc) {
        goto err;
    }

    /* Overwrite the configuration data. */
    memcpy(&tcs34725->cfg, cfg, sizeof(*cfg));

err:
    return (rc);
}

/**
 * Reads the raw red, green, blue and clear channel values
 *
 *
 * @param red value to return
 * @param green value to return
 * @param blue value to return
 * @param clear channel value
 * @param driver sturcture containing config
 */
int
tcs34725_get_rawdata(uint16_t *r, uint16_t *g, uint16_t *b, uint16_t *c,
                     struct tcs34725 *tcs34725)
{
    uint8_t payload[8] = {0};
    int rc;
    int delay_ticks;

    /* Set a delay for the integration time */
    switch (tcs34725->cfg.integration_time)
    {
        case TCS34725_INTEGRATIONTIME_2_4MS:
            delay_ticks = (3 * OS_TICKS_PER_SEC)/1000 + 1;
            break;
        case TCS34725_INTEGRATIONTIME_24MS:
            delay_ticks = (24 * OS_TICKS_PER_SEC)/1000 + 1;
            break;
        case TCS34725_INTEGRATIONTIME_50MS:
            delay_ticks = (50 * OS_TICKS_PER_SEC)/1000 + 1;
            break;
        case TCS34725_INTEGRATIONTIME_101MS:
            delay_ticks = (101 * OS_TICKS_PER_SEC)/1000 + 1;
            break;
        case TCS34725_INTEGRATIONTIME_154MS:
            delay_ticks = (154 * OS_TICKS_PER_SEC)/1000 + 1;
            break;
        case TCS34725_INTEGRATIONTIME_700MS:
            delay_ticks = (700 * OS_TICKS_PER_SEC)/1000 + 1;
            break;
        default:
            /*
             * If the integration time specified is not from the config,
             * it will get considered as valid inetgration time in ms
             */
            delay_ticks = (tcs34725->cfg.integration_time * OS_TICKS_PER_SEC)/
                          1000 + 1;
            break;
    }

    os_time_delay(delay_ticks);

    *c = *r = *g = *b = 0;

    rc = tcs34725_readlen(TCS34725_REG_CDATAL, payload, 8);
    if (rc) {
        goto err;
    }

    *c = payload[1] << 8 | payload[0];
    *r = payload[3] << 8 | payload[2];
    *g = payload[5] << 8 | payload[4];
    *b = payload[7] << 8 | payload[6];

#if MYNEWT_VAL(TCS34725_STATS)
    switch (tcs34725->cfg.integration_time) {
        case TCS34725_INTEGRATIONTIME_2_4MS:
            STATS_INC(g_tcs34725stats, samples_2_4ms);
            break;
        case TCS34725_INTEGRATIONTIME_24MS:
            STATS_INC(g_tcs34725stats, samples_24ms);
            break;
        case TCS34725_INTEGRATIONTIME_50MS:
            STATS_INC(g_tcs34725stats, samples_50ms);
            break;
        case TCS34725_INTEGRATIONTIME_101MS:
            STATS_INC(g_tcs34725stats, samples_101ms);
            break;
        case TCS34725_INTEGRATIONTIME_154MS:
            STATS_INC(g_tcs34725stats, samples_154ms);
            break;
        case TCS34725_INTEGRATIONTIME_700MS:
            STATS_INC(g_tcs34725stats, samples_700ms);
        default:
            STATS_INC(g_tcs34725stats, samples_userdef);
        break;
    }

#endif

    return 0;
err:
    return rc;

}

/**
 *
 * Converts raw RGB values to color temp in deg K
 *
 * @param red value
 * @param green value
 * @param blue value
 * @return final CCT value using McCamy's formula
 */
static uint16_t
tcs34725_calculate_color_temp(uint16_t r, uint16_t g, uint16_t b)
{
    float n;
    float cct;

    /**
     * From the designer's notebook by TAOS:
     * Mapping sensor response  RGB values to CIE tristimulus values(XYZ)
     * based on broad enough transformation, the light sources chosen were a
     * high color temperature fluorescent (6500K), a low color temperature
     * fluorescent (3000K), and an incandescent (60W)
     * Note: y = Illuminance or lux
     *
     * For applications requiring more precision,
     * narrower range of light sources should be used and a new correlation
     * matrix could be formulated and CIE tristimulus values should be
     * calculated. Please refer the manual for calculating tristumulus values.
     *
     * x = (-0.14282F * r) + (1.54924F * g) + (-0.95641F * b);
     * y = (-0.32466F * r) + (1.57837F * g) + (-0.73191F * b);
     * z = (-0.68202F * r) + (0.77073F * g) + ( 0.56332F * b);
     *
     *
     * Calculating chromaticity co-ordinates, the light can be plotted on a two
     * dimensional chromaticity diagram
     *
     * xc = x / (x + y + z);
     * yc = y / (x + y + z);
     *
     * Use McCamy's formula to determine the CCT
     * n = (xc - 0.3320F) / (0.1858F - yc);
     */

     /*
      * n can be calculated directly using the following formula for
      * above considerations
      */
    n = ((0.23881)*r + (0.25499)*g + (-0.58291)*b) / ((0.11109)*r + (-0.85406)*g +
         (0.52289)*b);

    /*
     * Calculate the final CCT
     * CCT is only meant to characterize near white lights.
     */

#if MYNEWT_VAL(USE_MATH)
    cct = (449.0F * powf(n, 3)) + (3525.0F * powf(n, 2)) + (6823.3F * n) + 5520.33F;
#else
    cct = (449.0F * n * n * n) + (3525.0F * n * n) + (6823.3F * n) + 5520.33F;
#endif

    /* Return the results in degrees Kelvin */
    return (uint16_t)cct;
}

/**
 *
 * Converts the raw RGB values to lux
 *
 * @param red value
 * @param green value
 * @param blue value
 * @return lux value
 */
static uint16_t
tcs34725_calculate_lux(uint16_t r, uint16_t g, uint16_t b)
{
    float lux;

    lux = (-0.32466F * r) + (1.57837F * g) + (-0.73191F * b);

    return (uint16_t)lux;
}



static int
tcs34725_sensor_read(struct sensor *sensor, sensor_type_t type,
        sensor_data_func_t data_func, void *data_arg, uint32_t timeout)
{
    struct tcs34725 *tcs34725;
    struct sensor_color_data scd;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t c;
    int rc;

    /* If the read isn't looking for accel or mag data, don't do anything. */
    if (!(type & SENSOR_TYPE_COLOR)) {
        rc = SYS_EINVAL;
        goto err;
    }

    tcs34725 = (struct tcs34725 *) SENSOR_GET_DEVICE(sensor);

    /* Get a new accelerometer sample */
    if (type & SENSOR_TYPE_COLOR) {
        r = g = b = c = 0;

        rc = tcs34725_get_rawdata(&r, &g, &b, &c, tcs34725);
        if (rc) {
            goto err;
        }

        scd.scd_r = r;
        scd.scd_g = g;
        scd.scd_b = b;
        scd.scd_c = c;
        scd.scd_lux = tcs34725_calculate_lux(r, g, b);
        scd.scd_colortemp = tcs34725_calculate_color_temp(r, g, b);

        /* Call data function */
        rc = data_func(sensor, data_arg, &scd);
        if (rc != 0) {
            goto err;
        }
    }

    return 0;
err:
    return rc;
}

/**
 * enables/disables interrupts
 *
 * @param enable/disable
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_enable_interrupt(uint8_t enable)
{
    uint8_t reg;
    int rc;

    rc = tcs34725_read8(TCS34725_REG_ENABLE, &reg);
    if (rc) {
        goto err;
    }

    if (enable) {
        reg |= TCS34725_ENABLE_AIEN;
    } else {
        reg &= ~TCS34725_ENABLE_AIEN;
    }

    rc = tcs34725_write8(TCS34725_REG_ENABLE, reg);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Clears the interrupt by writing to the command register
 * as a special function
 * ______________________________________________________
 * |   CMD |     TYPE    |         ADDR/SF              |
 * |    7  |     6:5     |           4:0                |
 * |    1  |      11     |          00110               |
 * |_______|_____________|______________________________|
 *
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_clear_interrupt(void)
{
    int rc;
    uint8_t payload = TCS34725_COMMAND_BIT | TCS34725_CMD_TYPE | TCS34725_CMD_ADDR;

    struct hal_i2c_master_data data_struct = {
        .address = MYNEWT_VAL(TCS34725_I2CADDR),
        .len = 0,
        .buffer = &payload
    };

    rc = hal_i2c_master_write(MYNEWT_VAL(TCS34725_I2CBUS), &data_struct,
                              OS_TICKS_PER_SEC / 10, 1);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Sets threshold limits for interrupts, if the low threshold is set above
 * the high threshold, the high threshold is ignored and only the low
 * threshold is evaluated
 *
 * @param lower threshold
 * @param higher threshold
 *
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_set_int_limits(uint16_t low, uint16_t high)
{
    uint8_t payload[4];
    int rc;

    payload[0] = low & 0xFF;
    payload[1] = low >> 8;
    payload[2] = high & 0xFF;
    payload[3] = high >> 8;

    rc = tcs34725_writelen(TCS34725_REG_AILTL, payload, sizeof(payload));
    if (rc) {
        return rc;
    }
    return 0;
}

static void *
tcs34725_sensor_get_interface(struct sensor *sensor, sensor_type_t type)
{
    return (NULL);
}

/**
 *
 * Gets threshold limits for interrupts, if the low threshold is set above
 * the high threshold, the high threshold is ignored and only the low
 * threshold is evaluated
 *
 * @param ptr to lower threshold
 * @param ptr to higher threshold
 *
 * @return 0 on success, non-zero on failure
 */
int
tcs34725_get_int_limits(uint16_t *low, uint16_t *high)
{
    uint8_t payload[4];
    int rc;

    rc = tcs34725_readlen(TCS34725_REG_AILTL, payload, sizeof(payload));
    if (rc) {
        return rc;
    }

    *low   = payload[0];
    *low  |= payload[1] << 8;
    *high  = payload[2];
    *high |= payload[3] << 8;

    return 0;
}

static int
tcs34725_sensor_get_config(struct sensor *sensor, sensor_type_t type,
                           struct sensor_cfg *cfg)
{
    int rc;

    if ((type != SENSOR_TYPE_COLOR)) {
        rc = SYS_EINVAL;
        goto err;
    }

    cfg->sc_valtype = SENSOR_VALUE_TYPE_INT32;

    return (0);
err:
    return (rc);
}
