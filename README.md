## Getting Started

Before getting started, make sure you have a proper Zephyr development
environment. You can follow the official
[Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/getting_started/index.html).

### Initialization

The first step is to initialize the workspace folder where the
`helium_mapper` and needed Zephyr modules will be cloned. You can do
that by running:

```shell
# initialize workspace for the helium_mapper (main branch)
west init -m https://github.com/retfie/graywater_box --mr main graywater_box_project
# update Zephyr modules
cd graywater_box_project/graywater_box
west update
```

### Build & Run

The application can be built by running:

```shell
west build -b olimex_lora_stm32wl_devkit -s app
```

Once you have built the application you can flash it by running:

```shell
west flash
```

### Serial terminal

```shell
screen /dev/ttyACM0 115200
```
