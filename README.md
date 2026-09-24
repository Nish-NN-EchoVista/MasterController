# MasterController

STM32CubeIDE firmware project for the NUCLEO-H7A3ZI-Q board (STM32H7A3).

## Project contents

- `Core/`: application code, interrupt handlers, and startup code.
- `Drivers/`: bundled STM32 HAL, CMSIS, and board support drivers.
- `MasterController.ioc`: STM32CubeMX peripheral and pin configuration.
- `STM32H7A3ZITXQ_FLASH.ld` and `STM32H7A3ZITXQ_RAM.ld`: linker scripts.
- Eclipse project files and `.settings/`: STM32CubeIDE project configuration.

## Open and build

Import this directory into STM32CubeIDE as an existing project, then build
using the IDE. The configuration records STM32CubeMX 6.17.0 and
STM32Cube H7 firmware package V1.13.0.

The current application initializes GPIO, DMA, the watchdog, RTC, TIM1,
and seven UART/USART interfaces, along with board LEDs, the user button,
and the board COM port. The main application loop is currently empty.

Build output and local IDE workspace state are excluded from Git.
