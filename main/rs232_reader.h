#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rs232_reader_init(void);
esp_err_t rs232_reader_reconfigure(void);
void rs232_reader_inject_test_frame(void);

#ifdef __cplusplus
}
#endif
