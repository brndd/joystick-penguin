# Joystick Penguin

Linux virtual joystick remapper inspired by Joystick Gremlin and Joyful.
The current prototype maps physical buttons to virtual joystick buttons,
including held modifiers. Other controls and mapping features are still in
development.

## Build

Requires Linux, a C++23 compiler, CMake 3.20+, pkg-config, and libevdev
development headers (Fedora: `libevdev-devel`). CMake downloads yaml-cpp on
first configure.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Try it

Edit [examples/hardware.yaml](examples/hardware.yaml): set `physical.path` to
your joystick's `/dev/input/by-id/` or `/dev/input/by-path/` link and choose a
button code it supports. `evtest <physical-link>` lists the codes; button
numbers in profiles are **Linux event codes**, not button indices.
[examples/gestures.yaml](examples/gestures.yaml) demonstrates modifier capture
and multiple outputs; edit its physical path the same way to try it.

```sh
./build/joystick-penguin examples/hardware.yaml
```

The program prints the virtual `/dev/input/eventN` path. Use
`evtest <virtual-event-node>` in another terminal to see mapped presses and
releases. Stop the remapper with Ctrl+C. You need read access to the physical
device and virtual event node, and write access to `/dev/uinput`. If a grab
fails, close any other remapper using that physical device.

Use `./build/joystick-penguin --check <profile.yaml>` to validate a profile
without opening devices. [examples/basic.yaml](examples/basic.yaml) illustrates
multiple declared modes and currently works with `--check` only.

See [NOTES.md](NOTES.md) for implementation details and current limitations,
[the specification](AGENTS/SPEC.md) for planned features, and [LICENSE](LICENSE)
for the GPL-3.0-or-later license.
