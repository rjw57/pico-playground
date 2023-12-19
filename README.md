# Raspberry Pi Pico playground

## Building

```console
$ mkdir build
$ pushd build
$ PICO_SDK_PATH=$HOME/projects/pico/pico-sdk cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 ..
$ popd
$ cmake --build build --parallel
```

## Hardware

Connect the output of SYNC and VIDEO to the output by means of two resistors. The ideal resistor for
SYNC is 575 ohms and for VIDEO is 246 ohms. Using 560 and 220 should be OK. The output should be
connected to ground via a 75 ohm resistor.

## Picoprobe

Configuration for udev allowing members of the `dialout` group to connect to a picoprobe is provided
in the [udev](./udev/) directory. To enable:

```console
$ sudo cp udev/99-picoprobe.rules /etc/udev/rules.d/
$ sudo udevadm control --reload-rules &&  sudo udevadm trigger
```

One should then be able to start `openocd` as an ordinary user after building and installing it as
per the pico getting started guide:

```sh
openocd -f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2040.cfg -s tcl
```

If using a RP2040-GEEK, use the appropriate connector cable to connect the SWD port of the
RP2040-GEEK to the SWD pins of the pico. Connect the UART cable according to the following table:

| RP2040-GEEK | Pico pin |
|-|-|
| GP5 | 1 (GP0) |
| GND | 3 |
| GP4 | 2 (GP1) 

The serial console can be connected to via, e.g. `minicom`:

```sh
minicom -b 115200 -o -D /dev/ttyACM0
```

Start the debugger via:

```sh
gdb -ix gdbinit ./build/playground/playground.elf
```

The `bld` and `mri` commands are provided in the `gdbinit` file to rapidly re-build and load and/or
reset the pico.
