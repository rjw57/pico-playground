# Raspberry Pi Pico playground

## Building

```console
$ mkdir build
$ pushd build
$ PICO_SDK_PATH=$HOME/projects/pico/pico-sdk cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 ..
$ popd
$ cmake --build build --parallel
```
