#ifndef MAX9814_DRIVER_H_
#define MAX9814_DRIVER_H_

#include <stdint.h>
#include "esp_err.h"

esp_err_t max9814_init(void);
void max9814_task(void *arg);

#endif // MAX9814_DRIVER_H_