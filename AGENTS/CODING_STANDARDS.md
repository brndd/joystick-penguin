# Coding standards

- Target C++23. Follow the existing project layout and naming conventions, and
  keep platform-specific evdev/uinput details behind input/output interfaces.
- Keep input decoding separate from mapping decisions. Process device events,
  timers, mode changes, and output updates in one serialized engine loop; do not
  mutate mapping state from reader or timer threads.
- Use `std::expected` (or an explicit result type) for recoverable operational
  failures such as device loss, failed grabs, and reconnect attempts. Handle
  those failures explicitly without stopping unrelated controllers.
- Exceptions are appropriate for unexpected failures and at boundaries with
  libraries that throw. yaml-cpp throws during parsing; translate its errors at
  the configuration boundary. Do not impose a project-wide no-exceptions rule.
- Validate YAML before starting the engine and pass typed configuration into
  runtime code. Make unsupported profile features and ambiguous bindings errors
  rather than silently ignoring them.
- Add deterministic tests for mapping semantics and cleanup. Use an injectable
  clock for timers; do not make tests depend on wall-clock sleeps or real input
  hardware. Run the CMake build and CTest after relevant changes.

See [SPEC.md](SPEC.md) for the required behavior and delivery sequence.
