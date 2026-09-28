# Joystick Penguin

Linux virtual joystick remapper inspired by Joystick Gremlin and Joyful.
The current prototype maps physical joystick buttons, hats, and absolute axes
to virtual joysticks, including held modifiers, persistent modes, and tap/hold
buttons.

## Build

Requires Linux, a C++23 compiler, CMake 3.20+, pkg-config, and libevdev
development headers (Fedora: `libevdev-devel`). CMake downloads yaml-cpp on
first configure and finds CLI11 (Fedora: `cli11-devel`). Text-to-speech
additionally needs espeak-ng development headers (Fedora: `espeak-ng-devel`);
configure with `-DJP_ENABLE_TTS=OFF` to build without that dependency.

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

## Reload a profile

Save changes to the same profile file, then press **r** in the remapper's
terminal (no Enter needed) or send `kill -HUP <remapper-pid>` from another
terminal. Single-key input is enabled automatically when stdin is a terminal;
its settings are restored when the remapper exits. The console reports a
successful reload or explains why it was rejected. An invalid profile leaves
the running mappings active.

Reload keeps the existing virtual joysticks and their event nodes alive. You
may change bindings, modes, modifiers, timers, and physical controllers. New or
repointed physical controllers that are absent will be retried. Changes to
virtual devices' names, identity, advertised controls, or axis ranges require
a restart. Successful reload starts in the profile's initial mode, releases
held virtual buttons and hats, cancels pending taps, and resumes axes at their
current positions. Release and press any physically held button again to
activate its new mapping.

Use `./build/joystick-penguin --check <profile.yaml>` to validate a profile
without opening devices. A mode action is `{type: mode, mode: alternate}`.
Button bindings may specify a positive `threshold_ms` and `tap` and/or `hold`
branches, each containing an `action` or `actions` list. A short release pulses
tap outputs, which stay asserted for `tap_ms` milliseconds (default 50) before
releasing so consumers can register the tap; reaching the threshold activates
hold outputs, which release with the physical button. Mode selections persist
until another mode action changes them. Captured buttons and hats stay held
across mode changes; axes reroute live.

## Mode announcements

When built with espeak-ng support, the remapper speaks the name of the new mode
when a mode action changes it. Speech runs on its own thread, so synthesis
never blocks input handling. The voice and audio parameters can be set on the
command line:

```sh
./build/joystick-penguin --voice en --speed 130 --pitch 20 --range 0 profile.yaml
```

The defaults are espeak-ng's default English voice at speed 130, pitch 20, and
range 0 (monotone). `--speed` accepts 80-450 words per minute; `--pitch` and
`--range` accept 0-100. Use `--no-tts` to disable announcements, or configure
with `-DJP_ENABLE_TTS=OFF` to drop the dependency entirely.

See [NOTES.md](NOTES.md) for implementation details and current limitations,
[the specification](AGENTS/SPEC.md) for planned features, and [LICENSE](LICENSE)
for the GPL-3.0-or-later license.
