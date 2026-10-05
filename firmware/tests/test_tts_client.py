# SPDX-License-Identifier: Apache-2.0
"""Host execution of the local TTS client and fresh-source session patch.

Set CJSON_SOURCE_DIR to the cJSON directory containing cJSON.c/h. ESP-IDF 5.5
ships this at $IDF_PATH/components/json/cJSON; the staged Muse build also has it
in managed_components. Tests use only fakes; they never open sockets or audio.
"""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

FIRMWARE = Path(__file__).resolve().parents[1]
PORT = FIRMWARE / "muse-port"


def load_patch():
    spec = importlib.util.spec_from_file_location("nemossi_session_patch", PORT / "patch_session.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def json_source():
    candidates = []
    if os.environ.get("CJSON_SOURCE_DIR"):
        candidates.append(Path(os.environ["CJSON_SOURCE_DIR"]))
    if os.environ.get("IDF_PATH"):
        candidates.append(Path(os.environ["IDF_PATH"]) / "components/json/cJSON")
    # Authorized isolated tool installation for this workspace. No download.
    workspace = FIRMWARE.parent.parent
    candidates.append(workspace / ".hardware-tooling/esp-idf-v5.5.1/components/json/cJSON")
    candidates.extend((FIRMWARE / "managed_components").glob("*/cJSON"))
    return next((p for p in candidates if (p / "cJSON.c").is_file()), None)


class TTSClient(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = json_source()
        if not source:
            raise unittest.SkipTest("Set CJSON_SOURCE_DIR to the official cJSON source directory")
        cls.temp = tempfile.TemporaryDirectory(prefix="nemossi-tts-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "tts"
        compiler = shlex.split(os.environ.get("CC", "clang"))
        command = [*compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                   "-I", str(source), "-I", str(FIRMWARE / "tests/tts_fakes"),
                   str(FIRMWARE / "tests/test_tts_client.c"), str(source / "cJSON.c"), "-lm", "-o", str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, *args):
        result = subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("assertions passed", result.stdout)
        print(result.stdout.strip())

    def test_fake_http_pcm_json_validation_caps_cancel_and_deadline(self):
        self.run_case()

    def test_queue_allocation_failure_disables_client(self):
        self.run_case("1")

    def test_mutex_allocation_failure_disables_client(self):
        self.run_case("2")

    def test_worker_creation_failure_disables_client(self):
        self.run_case("3")


class SessionPatch(unittest.TestCase):
    def pinned_source(self):
        workspace = FIRMWARE.parent.parent
        sdk = Path(os.environ.get("MUSE_SDK_PATH", workspace / ".hardware-tooling/muse-gadget-sdk"))
        path = sdk / "esp32/components/muse/muse_chat_session.cpp"
        if not path.exists():
            self.skipTest("Set MUSE_SDK_PATH to the pinned SDK checkout")
        return path

    def test_unknown_source_rejected_without_write(self):
        patch = load_patch()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "muse_chat_session.cpp"
            before = b"an unpinned source\n"
            path.write_bytes(before)
            with self.assertRaises(ValueError):
                patch.apply_session_patch(path)
            self.assertEqual(path.read_bytes(), before)

    def test_fresh_pinned_copy_and_second_application_rejected(self):
        patch = load_patch()
        path = self.pinned_source()
        source = path.read_bytes()
        with tempfile.TemporaryDirectory() as temp:
            staged = Path(temp) / "muse_chat_session.cpp"
            staged.write_bytes(source)
            patch.apply_session_patch(staged)
            result = staged.read_bytes()
            self.assertTrue(result.startswith(source[:700]))  # Apache notice retained.
            self.assertIn(b'LOCAL TTS NOT CONFIGURED', result)
            self.assertIn(b'xSemaphoreTake(s_local_commit_lock, 0)', result)
            with self.assertRaises(ValueError):
                patch.apply_session_patch(staged)
            self.assertEqual(staged.read_bytes(), result)
        self.assertEqual(path.read_bytes(), source)

    def test_actual_session_pcm_backpressure_failure_cancel_and_begin_races(self):
        patched = load_patch().patched_source(self.pinned_source().read_bytes()).decode()
        ranges = [
            ("static void start_tts(void)", "static void tts_data("),
            ('extern "C" void muse_hatch_turn_begin(void)', 'extern "C" void muse_hatch_turn_audio('),
            ('extern "C" void muse_hatch_turn_cancel(void)', 'extern "C" void muse_hatch_set_resting('),
            ('extern "C" size_t muse_hatch_turn_read(', '/* Bench test:'),
        ]
        functions = "\n".join(patched[patched.index(start):patched.index(end, patched.index(start))]
                               for start, end in ranges)
        harness = (FIRMWARE / "tests/tts_session_harness.cpp").read_text()
        harness = harness.replace("/* NM_EXTRACTED_SESSION_FUNCTIONS */", functions)
        with tempfile.TemporaryDirectory(prefix="nemossi-session-test-") as temp:
            path = Path(temp) / "session.cpp"
            binary = Path(temp) / "session"
            path.write_text(harness)
            compiler = shlex.split(os.environ.get("CXX", "clang++"))
            command = [*compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                       "-I", str(PORT), str(path), "-o", str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())

    def test_actual_startup_failures_never_report_session_ready(self):
        patched = load_patch().patched_source(self.pinned_source().read_bytes()).decode()
        ranges = [
            ('extern "C" void muse_hatch_start(void)', 'extern "C" void muse_hatch_chat_connect('),
            ('extern "C" bool muse_hatch_ready(void)', 'extern "C" void muse_hatch_turn_begin('),
        ]
        functions = "\n".join(patched[patched.index(start):patched.index(end, patched.index(start))]
                               for start, end in ranges)
        harness = (FIRMWARE / "tests/tts_startup_harness.cpp").read_text().replace(
            "/* NM_EXTRACTED_STARTUP_FUNCTIONS */", functions)
        with tempfile.TemporaryDirectory(prefix="nemossi-startup-test-") as temp:
            path = Path(temp) / "startup.cpp"
            binary = Path(temp) / "startup"
            path.write_text(harness)
            compiler = shlex.split(os.environ.get("CXX", "clang++"))
            compiled = subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                                       str(path), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())


if __name__ == "__main__":
    unittest.main()
