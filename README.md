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
