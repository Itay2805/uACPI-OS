#pragma once

#include <lib/defs.h>

#define PS2_OUTPUT_BUFFER_FULL                      BIT0
#define PS2_INPUT_BUFFER_FULL                       BIT1
#define PS2_TIMEOUT_ERROR                           BIT6
#define PS2_PARITY_ERROR                            BIT7

#define PS2_CMD_READ_CONFIG                         0x20
#define PS2_CMD_WRITE_CONFIG                        0x60

#define PS2_CONFIG_FIRST_PS2_INTERRUPT_ENABLE       BIT0
#define PS2_CONFIG_SECOND_PS2_INTERRUPT_ENABLE      BIT1
#define PS2_CONFIG_SYSTEM_FLAG                      BIT2
#define PS2_CONFIG_FIRST_PS2_CLOCK_DISABLE          BIT4
#define PS2_CONFIG_SECOND_PS2_CLOCK_DISABLE         BIT5
#define PS2_CONFIG_FIRST_PS2_PORT_TRANSLATION       BIT6
