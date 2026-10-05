# 네모씨 Muse + Mac 로컬 TTS

2026-10-05 사용자가 **PLUS push-to-talk → Muse 텍스트 답 → Mac 로컬 TTS →
보드 스피커와 입 움직임**을 승인했다. 기존 Stack-chan 기본 얼굴과 여섯 표정을
사용한다. ChatGPT 기존 dot 통화와는 별도 Muse 대화다. 실제 계정·보드 연결은 미검증이다.

공식 SDK는 [`3229892e93c18a768ace42cbe1fe7133f91ca203`](https://github.com/facebookincubator/muse-gadget-sdk/tree/3229892e93c18a768ace42cbe1fe7133f91ca203)에 고정했다.
[README](https://github.com/facebookincubator/muse-gadget-sdk/blob/3229892e93c18a768ace42cbe1fe7133f91ca203/esp32/README.md)는
ESP-IDF **6.0.1**을 요구하고 기본 음성 답은 텍스트다. 공식 지원 목록에 없는
1.54 LCD를 별도 backend로 포팅했다. `HOMEHUB_VOICE`는 전체 LVGL UI 없이 공식 세션을 쓴다.

```mermaid
flowchart LR
  P[PLUS와 마이크] -->|최대 15초 음성 메모| M[공식 Muse 세션]
  M -->|텍스트 답| T[Mac 로컬 TTS]
  T -->|16kHz 모노 PCM16 WAV| S[보드 스피커]
  S -->|성공한 PCM 전송 진폭| F[기존 얼굴과 입]
```

오디오는 공통 16kHz·64 BCLK 반이중이다. 원본 player의 48kHz stereo PCM32를
backend에서 16kHz PCM16으로 변환한다. 최대 볼륨은 20%다. I2C 오류·readback
불일치·부분 스피커 전송은 앰프를 끄고 실패한다. 새 녹음은 이전 player의 정지를
확인한 뒤 시작한다. 입 움직임은 전송된 PCM 증거이며 실제 공기 중 소리 측정은 아니다.

터치를 누르면 기본 → 기쁨 → 궁금함 → 생각 중 → 놀람 → 졸림 → 시무룩함을 선택한다.
표정은 대화 상태와 독립적이다. PLUS는 페어링 확인과 PTT에 쓰며 5초 눌러 설정을
지우는 동작은 끈다. BOOT와 PWR은 앱이 조작하지 않고 BOOT는 ROM 복구용으로 남긴다.

## 기기 없는 빌드

기존 데모는 IDF 5.5.1, Muse 포팅은 분리된 IDF 6.0.1을 사용한다. SDK 환경을
준비한 뒤 clean pinned Muse checkout에서 생성한다. 아래 명령은 장치를 열지 않는다.

```sh
git clone https://github.com/facebookincubator/muse-gadget-sdk.git /tmp/muse-gadget-sdk
git -C /tmp/muse-gadget-sdk checkout 3229892e93c18a768ace42cbe1fe7133f91ca203
python3 -B scripts/prepare_muse.py --sdk-source /tmp/muse-gadget-sdk
idf.py -C .muse-build/source/esp32 -B "$PWD/.muse-build/build" \
  -DSDKCONFIG="$PWD/.muse-build/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$PWD/.muse-build/source/esp32/sdkconfig.defaults;$PWD/.muse-build/source/esp32/sdkconfig.nemossi" build
python3 -B scripts/check_muse_build.py --build .muse-build/build --sdkconfig .muse-build/sdkconfig
```

빈 토큰·TTS 설정의 빌드는 컴파일 증거용이다. SDK 토큰 경고가 나오며 TTS 미설정은
`LOCAL TTS NOT CONFIGURED`로 실패한다. 연결이나 무음 응답 성공으로 대체하지 않는다.
`prepare_muse.py`는 기존 output을 덮지 않는다. SDK·generated config·바이너리는
`.muse-build/`의 로컬 산출물이고 Git에서 제외한다.

전체 검사는 `python3 -B scripts/verify.py`다. 실제 staged player 검사에는
`NEMOSSI_MUSE_PROJECT`를 생성한 `esp32` 폴더로 지정한다. TTS client/세션 검사에는
공식 cJSON source와 pinned checkout을 각각 `CJSON_SOURCE_DIR`, `MUSE_SDK_PATH`로
지정할 수 있다. 준비되지 않은 검사는 이유와 함께 skip을 표시한다.

## Mac TTS, 네트워크와 비용

`server/local_tts.py`는 설치된 한국어 **Yuna**를 `/usr/bin/say` stdin으로 합성하고
`/usr/bin/afconvert`로 WAV로 변환한다. macOS 설치 음성을 사용한다.
[Apple 음성 기능](https://support.apple.com/guide/mac-help/mh27448/mac).
중립 한국어 파일 합성은 통과했으며 실제 답 재생은 미검증이다. 새 음성 다운로드나
유료·외부 TTS는 실행하지 않았다. 추가 TTS API 비용은 없다. Muse 계정의 이용 자격·
요금은 사용자 계정에서 확인해야 하며 가입·결제는 수행하지 않았다.

수동 실행은 `NEMOSSI_TTS_TOKEN` 환경 변수와 `python3 -B -m server.local_tts`를 쓴다.
기본 주소는 `127.0.0.1:8766`이다. 기기 연결에는 사용자가 승인한 Mac의 private IPv4
`--host <private IPv4>`를 명시해야 한다. 일반 HTTP이므로 선택한 LAN에서 답 텍스트와
공유 비밀이 전송된다. client는 인터넷 주소·DNS·redirect를 거부한다. 지속 서비스·
방화벽 변경은 수행하지 않았다.

`POST /v1/tts`, `Authorization: Bearer <local secret>`, JSON `{"text":"..."}`를 받는다.
본문 4096 bytes, WAV 전체 960000 bytes·30초 이하, 동시 합성 1회다. 입력 2초·
서버 요청 12초·client 전체 15초 deadline이 있다. 취소·종료는 합성 프로세스와
임시 오디오를 정리한다. 텍스트·토큰 로그와 오디오 캐시는 만들지 않는다.
request 세대와 session commit 잠금으로 취소된 답을 폐기한다. 이미 I2S DMA에
넘긴 소리가 물리적으로 즉시 사라진다고 보장하지 않는다.

## 실제 연결에 필요한 사용자 단계

1. Muse 앱을 사용자 계정으로 준비하고 [SDK 약관](https://gadgets.muse.ai/sdk-terms)을
   확인한다. 사용자가 [SDK tokens](https://gadgets.muse.ai/settings/sdk-tokens)에서
   토큰을 준비한다. 토큰 요청·사용에는 이 추가 약관이 적용된다.
2. 토큰과 로컬 TTS URL·공유 비밀을 로컬 generated sdkconfig에 직접 입력한다.
   채팅·Git·로그·Library에 보내지 않는다. 토큰은 펌웨어에 포함되므로 바이너리도
   로컬에 둔다. 토큰 입력·LAN TTS 실행 범위는 사용자 확인 후 진행한다.
3. 재빌드·candidate 검사 뒤 새 이미지 SHA256·덮어쓰기 범위·복구안을 제시하고
   **그 이미지의 구체적인 업로드 승인**을 받는다.
4. 승인·실물 부팅 확인 뒤 Muse 앱 Devices → Developer mode → Add Device에서
   `MuseGadget-Nemossi-XXXXXX`를 선택한다. PLUS로 페어링 확인, Wi-Fi는 휴대폰에서
   사용자 직접 입력한다. 계정·Wi-Fi 저장 범위도 먼저 사용자에게 확인한다.
5. 연결 후 PLUS를 0.3초 이상 눌러 말하고 놓는다. 최대 15초다. 실제 답 재생·
   입 움직임·취소·재연결을 보드에서 검증한다. 초기 목표는 PTT 반이중이다.

## 원본 보존과 업로드 경계

빌드는 원본 `table=0x8000`, `app0=0x10000`, `size=0x330000`을 검사한다. app0만
덮는 계획을 검토한다. **`idf.py flash`, stock Muse `flash_args`, 전체 erase는 금지**다.
생성한 bootloader·table·otadata도 쓰지 않는다. 기기에 새 이미지를 쓰지 않았다.

16MiB 원본 백업과 app0 전체 복구 slice를 로컬 0600으로 준비했다.
복구 쓰기·시험은 별도 승인 대상이다. app1 이미지가 유효하지 않아 자동 rollback은
보장하지 않는다. [공식 bootloader 호환성](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-guides/bootloader.html#bootloader-compatibility)은
원본 `5.4.1-dirty`의 실제 부팅 증거를 대체하지 않는다.

eFuse/NVS 암호화·attestation·hardware Secure Boot·flash encryption·앱 서명·OTA·
터널·bug-report upload를 끄고 컴파일 guard와 generated config 검사로 강제한다.
원본에 없는 PHY partition 읽기도 끈다. 정상 부팅의 Muse 설정·Wi-Fi 보정·후속
페어링은 기존 NVS 일부 키를 쓸 수 있다. app-only가 부팅 후 NVS 바이트 보존까지
뜻하지 않는다. NVS 초기화 오류는 전체 erase 없이 실패한다. 실물 부팅·LCD·터치·
codec·LAN·Muse 연결은 아직 미검증이다.
