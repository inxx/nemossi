"""Compile actual prepared Muse C bodies against host-only timing fakes.

Set NEMOSSI_MUSE_PROJECT to the freshly staged esp32 project. An absent
environment variable skips these tests explicitly; no SDK/device is needed.
"""
import os
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def function_body(source, name):
    """Retain an exact C function, ignoring braces in strings/comments."""
    match = re.search(r"(?m)^static\s+(?:bool|size_t)\s+" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError("Actual staged voice function missing: " + name)
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    opening = source.index("{", match.start())
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():token.end()]
    raise ValueError("Unclosed actual C function: " + name)


def without_includes(source):
    return re.sub(r"(?m)^\s*#include[^\n]*\n", "", source)


class ActualMusePlayerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        project = os.environ.get("NEMOSSI_MUSE_PROJECT")
        if not project:
            raise unittest.SkipTest("NEMOSSI_MUSE_PROJECT absent: actual staged Muse C checks were not run")
        cls.project = Path(project)
        compiler = shutil.which(os.environ.get("CC", "clang")) or shutil.which("cc")
        if not compiler:
            raise RuntimeError("A host C compiler is required for actual Muse C checks")
        cls.temporary = tempfile.TemporaryDirectory(prefix="nemossi-muse-player-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        player = (cls.project / "main/voice_player.c").read_text()
        voice = (cls.project / "main/voice.c").read_text()
        print("Actual staged player SHA-256: " + hashlib.sha256(player.encode()).hexdigest(), flush=True)
        print("Actual staged voice SHA-256: " + hashlib.sha256(voice.encode()).hexdigest(), flush=True)
        common = '#include "muse_player_stubs.h"\n'
        player_tu = common + without_includes(player) + "\n" + (HERE / "muse_player_driver.c").read_text()
        macros = "\n".join(re.findall(r"(?m)^#define[^\n]*", voice))
        event = re.search(r"typedef enum \{ EVT_PRESS, EVT_RELEASE \} voice_evt_t;", voice)
        if not event:
            raise ValueError("Actual staged voice event declaration missing")
        voice_tu = common + macros + "\n" + event.group() + '\nstatic const char *TAG = "host.voice";\nstatic QueueHandle_t s_events;\n'
        voice_tu += "\n".join(function_body(voice, name) for name in ("pressed_again", "record", "fail", "reply", "run_turn"))
        voice_tu += "\n" + (HERE / "muse_voice_driver.c").read_text()
        cls.programs = {}
        for name, source in (("player", player_tu), ("voice", voice_tu)):
            path, binary = directory / (name + ".c"), directory / name
            path.write_text(source)
            result = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                                     "-pedantic", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                                     "-I", str(HERE), str(path), "-o", str(binary)],
                                    capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError("Actual staged " + name + " C compile failed:\n" + result.stderr)
            cls.programs[name] = binary

    def scenario(self, program, name):
        result = subprocess.run([str(self.programs[program]), name], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("passed", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_player_actual_worker_sets_error_and_idle_on_speaker_failure(self):
        self.scenario("player", "speaker_fail")

    def test_player_actual_worker_success_and_signed_sample_extremes(self):
        self.scenario("player", "speaker_ok")

    def test_player_oom_before_worker_creation(self):
        for scenario in ("oom1", "oom2"):
            with self.subTest(scenario=scenario):
                self.scenario("player", scenario)

    def test_voice_final_failure_with_idle_already_set_is_not_success(self):
        self.scenario("voice", "final_failure")

    def test_voice_success_keeps_existing_completion(self):
        self.scenario("voice", "reply_ok")

    def test_voice_waits_for_old_player_before_mic_capture(self):
        self.scenario("voice", "capture_handoff")

    def test_voice_handoff_timeout_does_not_start_mic_or_turn(self):
        self.scenario("voice", "capture_timeout")


if __name__ == "__main__":
    unittest.main(verbosity=2)
