#pragma once
typedef struct { int dummy; } gpio_dev_t;
extern gpio_dev_t GPIO;
#define gpio_ll_input_enable(hw, gpio) ((void)(hw), (void)(gpio))
