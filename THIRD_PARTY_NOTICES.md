# Stack-chan face attribution

## Muse Gadgets SDK port

The port stages the separately downloaded Meta Platforms Muse Gadgets SDK at
commit `3229892e93c18a768ace42cbe1fe7133f91ca203` under Apache-2.0. Upstream LICENSE
and source headers remain in the generated local tree. The license is reproduced
in [licenses/Muse-Gadgets-Apache-2.0.txt](licenses/Muse-Gadgets-Apache-2.0.txt).
This repository contains
authored board/TTS adapters and explicit patch scripts. Modifications add bounded
PCM TTS, cancellation/OOM guards and the existing face. Security activation, OTA,
tunnel and bug report uploads are disabled for this port.

Upstream minimp3 retains CC0-1.0 and pixel_font.c retains BSD-2-Clause. Jollybot
artwork is excluded from the staged port. The Stack-chan notices below still apply.
macOS speech uses installed OS voices; Apple voice data is not distributed here.
Registry components keep their licenses in ignored local build directories.

Source and license: https://github.com/facebookincubator/muse-gadget-sdk/tree/3229892e93c18a768ace42cbe1fe7133f91ca203

Stack-chan is developed and published by **meganetaaan** (Shinya Ishikawa)
and the Stack-chan community: <https://github.com/stack-chan/stack-chan>.

Nemossi adapts the official default SimpleFace geometry, eyelid masks and motion
formulas from commit `2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d`.
The affected renderer files identify the source and their modifications.
The original Apache License 2.0 is reproduced in
[licenses/Stack-chan-Apache-2.0.txt](licenses/Stack-chan-Apache-2.0.txt).
The rest of Nemossi's device, hub and voice code is unchanged by this adaptation.

Changes include a uniform 3/4 projection of the original 320×240 layout into the
240×240 screen, a centered vertical offset, native browser/C primitive rendering,
and bounded seeded motion scheduling for deterministic cross-renderer checks.
The existing normalized audio-level input controls mouth openness. No original
Stack-chan firmware or device drivers are installed or executed.

The six selectable Nemossi expressions are downstream designs, described in
[docs/expressions.md](docs/expressions.md). Joy, drowsiness and downcast expressions
use upstream HAPPY, SLEEPY and SAD eyelid motifs. Their caps, gaze offsets and
mouth adjustments are Nemossi parameters; the default upstream face is preserved.

The visual comparison credits the official README photograph to Stack-chan /
meganetaaan and links the same repository. The photograph depicts the earlier
three-button M5Stack. Current numeric geometry comes from the pinned SimpleFace
source. Reference pictures are kept as comparison artifacts, not app assets.
See the upstream [character guidelines](https://github.com/stack-chan/stack-chan/blob/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/GUIDELINE.md)
and [license](https://github.com/stack-chan/stack-chan/blob/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/LICENSE).
