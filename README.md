## Introduction

Helium Meteo is a small battery-powered device for measuring temperature, humidity and atmospheric pressure. Data samples are transmitted via Helium LoRaWan.

## BOM

 - Olimex [BB-STM32WL-ANT](https://www.olimex.com/Products/IoT/LoRa/BB-STM32WL/).
 - Olimex [MOD-BME280](https://www.olimex.com/Products/Modules/Sensors/MOD-BME280/open-source-hardware).

## Getting Started

Before getting started, make sure you have a proper Zephyr development
environment. You can follow the official
[Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/getting_started/index.html).

### Initialization

The first step is to initialize the workspace folder where the
`helium_meteo` and needed Zephyr modules will be cloned. You can do
that by running:

```shell
# initialize workspace for the helium_meteo (main branch)
west init -m https://github.com/dinuxbg/helium_meteo --mr main helium_meteo_project
# update Zephyr modules
cd helium_meteo_project/helium_meteo
west update
```

### Build & Run

The application can be built by running:

```shell
west build -d build -b olimex_lora_stm32wl_devkit@D -s helium_meteo/app  --pristine
```

Once you have built the application you can flash it by running:

```shell
west flash
```

### Serial terminal

Hook a serial UART to LPUSART1. You would get access to a shell running on the device, where you can setup the Helium/LoRaWan keys and tweak other settings.

```shell
screen /dev/ttyACM0 9600
```

## Acknowledgements

This project is heavily based on https://github.com/retfie/helium_mapper .

## TODO
 - Design a custom PCB.
 - Create a custom board definition for Zephyr instead of inheriting stm32wl_devkit.
 - Add more documentation (e.g. how to setup Helium/LoRaWan keys).
 - Add battery voltage measurement to the Lora packet.
