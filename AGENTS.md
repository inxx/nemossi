# Project guidance

The user approved changing Nemossi from the existing-dot USB experiment to
Muse Gadgets plus local Mac TTS on 2026-10-05. Preserve the current Stack-chan
face and six expressions. The device path is push-to-talk -> official Muse
text response -> local Mac TTS -> speaker and PCM mouth motion. This is a Muse
conversation; do not label it as the user's existing dot. The old browser hub
remains an explicitly mock demo, separate from the new device lifecycle.

- Use the Python standard library and plain browser modules for the local demo.
- Run `python3 scripts/verify.py` before publishing changes.
- Hardware GPIO values must cite the Waveshare source commit in `docs/hardware.md`.
- Host C tests do not prove ESP-IDF compilation or physical hardware operation.
- The Muse port uses the pinned public SDK and a separate ESP-IDF 6.0.1 environment.
  Stage with scripts/prepare_muse.py; check the generated config, table and image
  with scripts/check_muse_build.py. Never use stock Muse full flash arguments.
- Keep automatic eFuse/NVS encryption, hardware Secure Boot, flash encryption,
  pairing attestation, OTA, tunnel and bug-report upload disabled in this port.
- Ask for precise user scope before account/token setup, pairing/Wi-Fi changes,
  LAN TTS activation or flashing. Source and credential-free builds are authorized.
- Keep device credentials, recordings, local logs and build artifacts out of Git.
- Do not install an SDK, flash a device, create credentials, call a paid API, or
  register a persistent service without reporting the specific target and scope.
- Keep the demo's mock state distinct from an actual dot connection.
- Keep Mac mini hub coordination separate from the Nemossi device lifecycle.
- Smart glasses belong to a separate project. A future peer may use a phone
  relay; do not assume a direct Mac link or a USB-only final Nemossi design.
