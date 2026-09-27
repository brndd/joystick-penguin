# Joystick Penguin

Linux virtual joystick remapper inspired by Joystick Gremlin and Joyful.
The current prototype maps physical joystick buttons, hats, and absolute axes
to virtual joysticks, including held modifiers, persistent modes, and tap/hold
buttons.

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
your joystick's `/dev/input/by-id/` or `/dev/input/by-path/` link. `button: 1`
means its first advertised joystick button. Buttons are **one-based indices**
in profiles; use `button_code: 704` for a literal EV_KEY code if needed.
[examples/gestures.yaml](examples/gestures.yaml) demonstrates modifier capture
and multiple outputs. [examples/controls.yaml](examples/controls.yaml) maps
axes and diagonal hats across two controllers. [examples/modes.yaml](examples/modes.yaml)
demonstrates mode changes and tap/hold buttons. Edit their physical paths to
try them. [examples/star_citizen.yaml](examples/star_citizen.yaml) ports the
two-stick profile from Joyful using one-based physical and virtual button
numbers. The joystick preset advertises all 79 buttons and four hats on each
virtual device. Check the stable paths against your devices.

```sh
./build/joystick-penguin examples/hardware.yaml
```

The program prints the virtual `/dev/input/eventN` path. Use
`evtest <virtual-event-node>` in another terminal to see mapped presses and
releases. Stop the remapper with Ctrl+C. You need read access to the physical
device and virtual event node, and write access to `/dev/uinput`. If a grab
fails, close any other remapper using that physical device.

Use `./build/joystick-penguin --check <profile.yaml>` to validate a profile
without opening devices. A mode action is `{type: mode, mode: alternate}`.
Button bindings may specify a positive `threshold_ms` and `tap` and/or `hold`
branches, each containing an `action` or `actions` list. A short release pulses
tap outputs; reaching the threshold activates hold outputs, which release with
the physical button. Mode selections persist until another mode action changes
them. Captured buttons and hats stay held across mode changes; axes reroute live.

See [NOTES.md](NOTES.md) for implementation details and current limitations,
[the specification](AGENTS/SPEC.md) for planned features, and [LICENSE](LICENSE)
for the GPL-3.0-or-later license.
