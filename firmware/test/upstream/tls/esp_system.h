#pragma once
/* host stand-in: what IDF's own esp_system.h (and the sdkconfig.h it brings)
 * gives esp_crt_bundle.c */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_err.h"

#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS 200 /* the board's sdkconfig */
