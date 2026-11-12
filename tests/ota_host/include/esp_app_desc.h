#pragma once
#include <stdint.h>
typedef struct { uint32_t magic_word, secure_version, reserved[2]; char version[32]; uint8_t tail[208]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
