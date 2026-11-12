#pragma once
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
int gpio_get_level(int gpio);
int gpio_set_direction(int gpio, gpio_mode_t mode);
