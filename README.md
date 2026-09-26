# Joystick Penguin

Linux virtual joystick remapper inspired by Joystick Gremlin and Joyful.
See [the project specification](AGENTS/SPEC.md) and
[coding standards](AGENTS/CODING_STANDARDS.md) for implementation guidance.

## Build and test

Requires a C++23 compiler and CMake 3.20+.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/joystick-penguin examples/basic.yaml
```

The executable currently **validates** a profile and exits. It does not grab
physical controllers or run mappings yet.

## Profile conventions

Profiles are versioned YAML (`version: 1`). Devices are named and bindings
reference those names, not creation order. `evdev` devices need a stable path
(prefer `/dev/input/by-id/` or `/dev/input/by-path/`); `grab` defaults to true.
`uinput` devices currently declare `preset: joystick`. Modes, modifiers, and
bindings use the layout in [examples/basic.yaml](examples/basic.yaml).

**Numeric `button` and `axis` values are Linux input event codes**, not logical
indices: for example `BTN_SOUTH` is 304 and `ABS_X` is 0. Only button output
actions are accepted by the skeleton loader; the hardware and gesture engine
milestones will add runtime capability checks and additional action types.
Invalid references and ambiguous equally specific bindings fail loading.

The input backend, frame-based output sink, and injectable monotonic clock are
declared in `include/joystick_penguin/io.hpp`. The engine will use these typed
interfaces in a serialized event loop.
