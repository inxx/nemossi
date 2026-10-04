# Project guidance

Nemossi is a device for using the user's existing dot voice call. A new generic
LLM or Realtime API agent is not equivalent to that dot. Keep the external voice
transport separate and fail clearly until an official supported path is verified.

- Use the Python standard library and plain browser modules for the local demo.
- Run `python3 scripts/verify.py` before publishing changes.
- Hardware GPIO values must cite the Waveshare source commit in `docs/hardware.md`.
- Host C tests do not prove ESP-IDF compilation or physical hardware operation.
- Keep device credentials, recordings, local logs and build artifacts out of Git.
- Do not install an SDK, flash a device, create credentials, call a paid API, or
  register a persistent service without reporting the specific target and scope.
- Keep the demo's mock state distinct from an actual dot connection.
- Keep Mac mini hub coordination separate from the Nemossi device lifecycle.
- Smart glasses belong to a separate project. A future peer may use a phone
  relay; do not assume a direct Mac link or a USB-only final Nemossi design.
