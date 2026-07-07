#pragma once

// Internal die-temperature sensor with a persisted single-point calibration.
void  temp_init(void);              // load the saved offset from NVS (call at boot)
float temp_read_c(void);            // calibrated °C (raw + offset)
float temp_raw_c(void);             // uncalibrated raw °C (averaged)
float temp_offset(void);            // the stored calibration offset in °C
void  temp_calibrate(float real_c); // set offset so the reading == real_c now; persist it
