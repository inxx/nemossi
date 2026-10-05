#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) Meta Platforms, Inc. and affiliates. (upstream patch excerpts)
# Nemossi modifications: stage the 1.54 port, PCM TTS and fail-closed guards.
"""Stage the pinned public Muse SDK plus the Nemossi port; never flash/install.

Input is a clean local checkout. Only tracked public SDK source is copied.
The output must be a new directory, so generated settings cannot be overwritten.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import sys

MUSE_COMMIT = "3229892e93c18a768ace42cbe1fe7133f91ca203"
ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "firmware/muse-port"


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args])


def replace_once(path, before, after):
    text = path.read_text()
    if text.count(before) != 1:
        raise ValueError("pinned source patch mismatch: " + str(path.name))
    path.write_text(text.replace(before, after))


def stage(source, output):
    source, output = source.resolve(), output.absolute()
    if git(source, "rev-parse", "HEAD").decode().strip() != MUSE_COMMIT:
        raise ValueError("Muse SDK must be at pinned commit " + MUSE_COMMIT)
    subprocess.run(["git", "-C", str(source), "diff", "--quiet", "HEAD"], check=True)
    if output.exists() or output.is_symlink():
        raise ValueError("output must be a new directory; preserve existing build/settings")
    files = git(source, "ls-files", "-z", "esp32", "xplat", "LICENSE").decode().split("\0")
    output.mkdir(parents=True)
    # xplat stays beside esp32, matching the upstream noise_core CMake contract.
    project = output / "esp32"
    hashes = {}
    for name in filter(None, files):
        if name.startswith("esp32/avatar/"):
            continue  # Keep our Stack-chan face; do not copy Jollybot artwork.
        original = source / name
        if not original.is_file() or original.is_symlink():
            raise ValueError("SDK tracked input must be a regular file: " + name)
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
        target.chmod(original.stat().st_mode & 0o777)
        hashes[name] = hashlib.sha256(original.read_bytes()).hexdigest()
    for original in (PORT / "main").iterdir():
        if original.is_file():
            shutil.copyfile(original, project / "main" / original.name)
    for name in ("board_display.c", "board_display.h", "board_input.h", "board_pins.h"):
        shutil.copyfile(ROOT / "firmware/main" / name, project / "main" / name)
    for original in (ROOT / "firmware/model").glob("*.h"):
        shutil.copyfile(original, project / "main" / original.name)
    shutil.copyfile(ROOT / "firmware/model/face.c", project / "main/face.c")
    shutil.copyfile(PORT / "partitions.csv", project / "partitions-nemossi.csv")
    shutil.copyfile(PORT / "sdkconfig.nemossi", project / "sdkconfig.nemossi")
    kconfig = project / "main/Kconfig.projbuild"
    replace_once(kconfig, "        config HOMEHUB_LED_BACKEND_VOICE_RING", """        config HOMEHUB_LED_BACKEND_NEMOSSI
            bool "Nemossi Waveshare ESP32-S3-Touch-LCD-1.54"
            depends on IDF_TARGET_ESP32S3 && SPIRAM

        config HOMEHUB_LED_BACKEND_VOICE_RING""")
    replace_once(kconfig,
                 "(HOMEHUB_LED_BACKEND_VOICE_RING || HOMEHUB_LED_BACKEND_RESPEAKER_LITE) && SPIRAM",
                 "(HOMEHUB_LED_BACKEND_VOICE_RING || HOMEHUB_LED_BACKEND_RESPEAKER_LITE || HOMEHUB_LED_BACKEND_NEMOSSI) && SPIRAM")
    with kconfig.open("a") as stream:
        stream.write('''\nmenu "Nemossi local Mac TTS"
config NEMOSSI_TOUCH_ENABLED
    bool "Touch cycles the existing face expressions"
    default y
config NEMOSSI_BACKLIGHT_PERCENT
    int "Nemossi backlight percent"
    range 1 100
    default 60
config NEMOSSI_TTS_URL
    string "Local Mac TTS URL (private IPv4 only)"
    default ""
config NEMOSSI_TTS_TOKEN
    string "Local TTS shared secret (generated sdkconfig only)"
    default ""
endmenu\n''')
    cmake = project / "main/CMakeLists.txt"
    replace_once(cmake, '    "../avatar/happy_anim.c"\n', '')
    replace_once(cmake, 'else()\n    list(APPEND GADGET_SRCS "led_status.c")', '''elseif(CONFIG_HOMEHUB_LED_BACKEND_NEMOSSI)
    list(APPEND GADGET_SRCS "led_status_nemossi.c" "board_display.c"
         "board_input.c" "board_bus.c" "face.c")
else()
    list(APPEND GADGET_SRCS "led_status.c")''')
    replace_once(cmake, '    if(CONFIG_HOMEHUB_LED_BACKEND_RESPEAKER_LITE)', '''    if(CONFIG_HOMEHUB_LED_BACKEND_NEMOSSI)
        list(APPEND GADGET_SRCS "voice_board_nemossi.c")
    elseif(CONFIG_HOMEHUB_LED_BACKEND_RESPEAKER_LITE)''')
    replace_once(cmake, '    PRIV_INCLUDE_DIRS "../avatar"\n', '')
    replace_once(cmake, '    REQUIRES ${GADGET_REQUIRES}', '    REQUIRES ${GADGET_REQUIRES} esp_codec_dev esp_lcd_touch esp_lcd_touch_cst816s')
    manifest = project / "main/idf_component.yml"
    with manifest.open("a") as stream:
        stream.write('  espressif/esp_codec_dev: "==1.6.2"\n'
                     '  espressif/esp_lcd_touch: "==1.1.2"\n'
                     '  espressif/esp_lcd_touch_cst816s: "==1.0.6"\n')
    # Upstream logs transcripts in the voice task; keep speech out of local logs.
    replace_once(project / "main/voice.c", 'ESP_LOGI(TAG, "heard: %s", text);',
                 'ESP_LOGI(TAG, "transcript received");')
    replace_once(project / "main/voice.c", '#define DEFAULT_VOLUME      60',
                 '#define DEFAULT_VOLUME      20')
    replace_once(project / "main/voice.c", 'static void apply_volume(int volume) {',
                 'static void apply_volume(int volume) {\n    if (volume > 20) volume = 20;\n    if (volume < 0) volume = 0;')
    voice = project / "main/voice.c"
    voice.write_text(voice.read_text().replace('v > 100 ? 100', 'v > 20 ? 20')
                     .replace('volume > 100 ? 100', 'volume > 20 ? 20')
                     .replace('volume->valueint > 100', 'volume->valueint > 20')
                     .replace('volume must be 0-100', 'volume must be 0-20'))
    player = project / "main/voice_player.c"
    text = player.read_text().replace('s[k] << 16', 's[k] * 65536')
    text = text.replace('static volatile bool', 'static atomic_bool')
    player.write_text(text)
    replace_once(player, 'static atomic_bool s_started;', '''static atomic_bool s_started;
static atomic_bool s_failed;
static int16_t *s_input_pcm;
static int32_t *s_output_pcm;''')
    replace_once(player, '''    int16_t *in = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int32_t *out = heap_caps_malloc(CHUNK_SAMPLES * 6 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!in || !out) {
        ESP_LOGE(TAG, "no memory for the player");
        vTaskSuspend(NULL);
        return;
    }''', '''    int16_t *in = s_input_pcm;
    int32_t *out = s_output_pcm;''')
    replace_once(player, 'esp_err_t voice_player_init(void) {', '''esp_err_t voice_player_init(void) {
    s_input_pcm = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_output_pcm = heap_caps_malloc(CHUNK_SAMPLES * 6 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!s_input_pcm || !s_output_pcm) {
        free(s_input_pcm);
        free(s_output_pcm);
        s_input_pcm = NULL;
        s_output_pcm = NULL;
        return ESP_ERR_NO_MEM;
    }''')
    replace_once(player, '            voice_board_speaker_write(out, frames);\n            s_started = true;', '''            if (voice_board_speaker_write(out, frames) != ESP_OK) {
                led_status_set_voice(LED_VOICE_ERROR);
                s_failed = true;
                s_active = false;
                break;
            }
            s_started = true;''')
    # The stock player did not need status calls; our checked speaker error does.
    replace_once(player, '#include "voice_board.h"', '#include "voice_board.h"\n#include "led_status.h"')
    replace_once(player, '#include "esp_heap_caps.h"',
                 '#include "esp_heap_caps.h"\n#include <stdatomic.h>\n#include <stdlib.h>')
    replace_once(player, '    s_started = false;', '    s_started = false;\n    s_failed = false;')
    with player.open("a") as stream:
        stream.write('\nbool voice_player_failed(void) { return s_failed; }\n')
    with (project / "main/voice_player.h").open("a") as stream:
        stream.write('\nbool voice_player_failed(void);\n')
    replace_once(voice, '            voice_player_write(pcm, n);', '''            if (voice_player_write(pcm, n) != ESP_OK) {
                voice_player_stop();
                muse_hatch_turn_cancel();
                return fail("speaker unavailable");
            }''')
    text = voice.read_text()
    before = '        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);'
    if text.count(before) != 2:
        raise ValueError("pinned voice player status patch mismatch")
    voice.write_text(text.replace(before, '''        if (voice_player_failed()) {
            voice_player_stop();
            muse_hatch_turn_cancel();
            return fail("speaker unavailable");
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);'''))
    replace_once(voice, '    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total",', '''    if (voice_player_failed()) {
        voice_player_stop();
        muse_hatch_turn_cancel();
        return fail("speaker unavailable");
    }
    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total",''')
    replace_once(voice, 'static bool run_turn(void) {\n    voice_player_stop();', '''static bool run_turn(void) {
    voice_player_stop();
    if (!voice_player_wait(500)) return fail("speaker stop timeout");''')
    # The user deliberately keeps BOOT for ROM recovery; PLUS is PTT and pairing.
    # Avoid an accidental five-second PTT hold resetting setup when disconnected.
    replace_once(project / "main/app.c", '    if (!button_init(on_button_short_press, on_button_double_press,',
                 '    (void)on_button_long_press;\n    if (!button_init(on_button_short_press, on_button_double_press,')
    replace_once(project / "main/app.c", '                     on_button_long_press)) {',
                 '                     NULL)) {')
    replace_once(project / "main/app.c", '#include "app.h"',
                 '#include "app.h"\n#include "nemossi_build_guard.h"')
    for name in ("tts_client.c", "tts_client.h"):
        shutil.copyfile(PORT / name, project / "components/muse" / name)
    sys.path.insert(0, str(PORT))
    from patch_session import apply_session_patch
    apply_session_patch(project / "components/muse/muse_chat_session.cpp")
    replace_once(project / "components/muse/CMakeLists.txt",
                 '"muse_chat_text.c" "muse_text.c")\n    list(APPEND embed "test_reply.mp3")',
                 '"muse_chat_text.c" "muse_text.c" "tts_client.c")\n    list(APPEND embed "test_reply.mp3")')
    (output / "nemossi-source.json").write_text(json.dumps({
        "muse_commit": MUSE_COMMIT, "upstream_files": hashes,
        "device_actions": [], "credentials_configured": False,
        "jollybot_artwork_copied": False,
    }, indent=2) + "\n")
    return project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / ".muse-build/source")
    args = parser.parse_args()
    try:
        project = stage(args.sdk_source, args.output)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")
    print(project)


if __name__ == "__main__":
    main()
