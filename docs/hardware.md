# Waveshare 보드 근거와 검증 범위

대상은 **Waveshare ESP32-S3-Touch-LCD-1.54**다. ESP32-S3R8, 외부 Flash
16MB, PSRAM 8MB, ST7789 240×240 SPI LCD, CST816 계열 터치, ES7210 입력 ADC,
ES8311 출력 코덱, NS4150B 앰프, QMI8658 IMU 구성은
[공식 제품 문서](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)에서 확인했다.
터치 드라이버는 공식 예제와 같은 Espressif `esp_lcd_touch_cst816s`를 사용한다.

## 현재 Muse 포트

2026-10-05 승인된 새 경로는 [Muse + Mac 로컬 TTS](muse-connection.md)다.
분리한 IDF 6.0.1에서 ESP32-S3 cross build와 이미지·partition 검사가 통과했다.
기존 LCD·기본 얼굴·6표정 코드를 그대로 사용하며 PLUS GPIO4는 PTT·페어링,
터치는 표정 순환에 쓴다. 공통 I2C와 ES7210 TDM RX·ES8311 STD TX를 구현했고,
PA GPIO7은 오류에서 끈다. 볼륨은 20% 이하·16kHz 반이중이다. BOOT GPIO0와
PWR GPIO5는 앱이 조작하지 않는다. 실제 LCD·touch·codec·Wi-Fi는 미검증이다.

원본 16MiB flash 백업을 로컬 0600으로 보존했다. 새 앱은 기존 app0에 들어가며
bootloader·table·otadata를 쓰지 않는 계획이다. 새 펌웨어 업로드는 수행하지 않았다.
아래 버전·구현 설명은 기존 `firmware/main/` 데모 기준이며 Muse 포트와 구분한다.

## 고정한 공식 소스

공식 저장소는 [waveshareteam/ESP32-S3-Touch-LCD-1.54](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54)다.
조사 기준 commit은
[`5157db7c888e476fd57f8a95020800377447478f`](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/commit/5157db7c888e476fd57f8a95020800377447478f)
(2026-07-06)이며, 아래 파일의 기준 디렉터리는
`examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/`다.

| 자료 | 확인한 내용 |
| --- | --- |
| [bsp_display.h](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp/bsp_display.h), [bsp_display.c](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp/bsp_display.c) | LCD 핀, SPI2, 40MHz, 예제 SPI mode 3, RGB565, 색 반전, backlight PWM |
| [bsp_i2c.h](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp/bsp_i2c.h), [bsp_touch.h](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp/bsp_touch.h) | shared I2C 및 터치 핀 |
| [bsp_codec.c](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp/bsp_codec.c) | I2S0, MCLK/BCLK/LRCLK, ADC/DAC 데이터 핀, 앰프 핀, 16kHz 예제 |
| [main.c](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/main/main.c) | PLUS/BOOT/PWR 핀과 active-low 버튼 |
| [bsp_power_manager.c](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp/bsp_power_manager.c), [bsp_sdcard.h](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/blob/5157db7c888e476fd57f8a95020800377447478f/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp/bsp_sdcard.h) | 배터리·충전·SD 예약 핀 |

[공식 회로도](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf)
1쪽의 오른쪽 아래 GPIO 표를 대조해 IMU INT, native USB, UART도 확인했다.
확인한 PDF의 SHA-256은
`3f551b71e2e80eaa4a766f3aa6cba623332554068524e42df629a51835c87102`다.
공식 예제 파일을 임시 조사 영역에서 읽었으며 저장소에 vendor snapshot을 복사하지 않았다.

## GPIO

핀 상수는 [board_pins.h](../firmware/main/board_pins.h)에 모았다.
GPIO 번호와 칩의 패키지 pin 번호를 혼동하지 않는다.

| 주변장치 | 신호 | GPIO |
| --- | --- | --- |
| LCD SPI2 | MOSI / SCLK / CS / DC / RESET / BL | 39 / 38 / 21 / 45 / 40 / 46 |
| LCD | MISO | 미연결 (`-1`) |
| shared I2C0 | SDA / SCL | 42 / 41 |
| CST816 touch | RESET / INT | 47 / 48 |
| QMI8658 | INT | 6 |
| I2S0 | MCLK / BCLK(SCLK) / LRCLK | 8 / 9 / 10 |
| ES8311 출력 | ESP32 DOUT → DAC DSDIN | 12 |
| ES7210 입력 | ADC ASDOUT → ESP32 DIN | 11 |
| NS4150B | PA CTRL | 7 |
| 버튼 | PLUS / BOOT / PWR | 4 / 0 / 5, active-low |
| 배터리 | ADC / BAT EN / CHG STAT | 1 / 2 / 3 |
| SDMMC | CLK / CMD / D0 / D1 / D2 / D3 | 16 / 15 / 17 / 18 / 13 / 14 |
| native USB | D− / D+ | 19 / 20 |
| UART | TX / RX | 43 / 44 |

회로도는 GPIO0을 `KEY_MINUS`라고 표기하고 공식 예제는 BOOT라고 부른다.
기본 앱은 **PLUS GPIO4만 버튼 입력으로 등록**한다. PWR GPIO5, BOOT GPIO0,
배터리 전원 GPIO2, 앰프 GPIO7, SD, IMU, UART, audio I2S는 초기화하거나
토글하지 않는다. 첫 bring-up은 USB 전원 기준이며 배터리 전원 유지 동작은 후속 검증 대상이다.

## 버전과 실제 구현 경계

[Waveshare ESP-IDF 안내](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/ESP-IDF)는
**ESP-IDF 5.5.0 이상**을 요구한다. 이 프로젝트는 **5.5.x**, 우선 **5.5.1**을
대상으로 지정했다. 공식 소스 디렉터리 이름도 5.5.1이다. 다만 factory의
`dependencies.lock`에는 IDF 5.4.1이 남아 있어 그 lock을 복사하지 않았다.
프로젝트 manifest는 IDF `>=5.5.0,<5.6.0`, button 4.1.3, esp_lcd_touch 1.1.2,
cst816s 1.0.6을 지정한다. 이 component 버전들은 공식 factory lock에서 확인했다.

현재 펌웨어는 IDF의 ST7789 드라이버로 LCD를 초기화하고, 하나의 internal DMA
RGB565 frame을 완료 callback까지 보존한다. 렌더 task는 DMA 대기 중 buffer를
재사용하지 않는다. touch I2C polling은 별도 task이고 PLUS/touch 입력은 queue로
전달한다. 기본 `DEMO`에서 12초 얼굴 상태 순환을 보여 주며 입력은 시연을
재시작한다. 실제 push-to-talk 녹음과 터치 Talk/Cancel/Settings 화면은 구현되지 않았다.

ES7210과 ES8311의 I2S 설정은 동일하지 않다. 공식 factory는 출력에 stereo STD,
입력에 4-slot TDM을 사용하고 입력 codec에서 선택한 채널을 읽는다. 따라서
단순한 mono I2S 예제를 마이크 driver로 간주하지 않았다. `device_services.c`의
capture/playback와 기존 dot transport는 모두 명시적으로 `UNAVAILABLE`을 반환한다.
호스트의 16kHz mono PCM16 WAV는 transport 데이터 계약이며 물리 codec 검증을 뜻하지 않는다.

## 기존 데모 검증 기록

`sh firmware/tests/run_host_tests.sh`는 순수 C 상태·렌더링·서비스 경계 테스트를
실행한다. 이 결과는 ESP-IDF cross build, LCD 전송, 실제 터치, 마이크, 스피커,
AEC, Wi-Fi 또는 같은 dot 통화의 검증을 대신하지 않는다.

최초 데모 단계에는 SDK가 없었으며, 이후 승인 범위에서 분리된 IDF 5.5.1 데모와
IDF 6.0.1 Muse 포트의 toolchain을 준비하고 cross build를 완료했다. SDK·component·
바이너리는 저장소에 넣지 않았다. 기기 flash는 실행하지 않았다.
빌드 명령은 [firmware/README.md](../firmware/README.md)에 기록했다.

기존 데모의 firmware 소스와 primitive 얼굴은 자체 구현·공식 SimpleFace adaptation이다.
Muse 포트는 고정한 공식 SDK를 별도로 생성하며 upstream license·header를 보존한다.
Jollybot artwork는 복사하지 않는다. [제삼자 출처와 라이선스](../THIRD_PARTY_NOTICES.md).
