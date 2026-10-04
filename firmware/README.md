# Nemossi firmware

Waveshare ESP32-S3-Touch-LCD-1.54용 ESP-IDF **5.5.x** bring-up 골격이다.
핀과 근거 commit은 [hardware.md](../docs/hardware.md)에 기록했다.
SDK 설치나 플래시는 수행하지 않았으며 ESP-IDF cross build와 실물 동작은 미검증이다.

## 현재 동작

기본 `CONFIG_NEMOSSI_DEMO_MODE=y`에서는 240×240 primitive 얼굴에 `DEMO` 표시가
나오고 idle → listening → thinking → speaking이 각 3초씩 순환한다. 눈 깜빡임,
작은 시선 이동, 합성 mouth level은 오디오 없이 상태 모델에서 계산한다.
PLUS GPIO4 또는 touch press는 이 시연을 재시작하고 release는 입력 로그에 남는다.
버튼·터치·LCD의 실물 동작은 아직 확인하지 않았다.

DEMO를 끄면 얼굴은 `BLOCKED` 상태이며 입력에도 실제 dot 세션을 열지 않는다.
`main/device_services.h`가 audio capture/playback과 외부 dot 호출의 독립 경계다.
현재 구현은 항상 `NM_SERVICE_UNAVAILABLE`을 반환하며 마이크, 앰프, Wi-Fi 또는
유료 API를 활성화하지 않는다. 새로운 OpenAI API 대화나 Muse 세션을 기존 dot으로
취급하는 코드도 없다. 실제 연결의 조건은 [dot-connection.md](../docs/dot-connection.md)를 따른다.

## 설치 없이 호스트 검증

저장소 루트에서 실행한다. `CC`를 지정하지 않으면 PATH의 clang 또는 cc를 사용한다.
임시 바이너리는 임시 디렉터리에 생성하고 실행 후 정리한다.

```sh
sh firmware/tests/run_host_tests.sh
# 선택: 다른 C compiler로 순수 C 모델만 검증
CC=cc sh firmware/tests/run_host_tests.sh
```

모델은 ESP-IDF와 독립적이며 `model/face.h`의 monotonic millisecond API를 쓴다.
backward timestamp, invalid state/event, stale input, mouth level 범위와 timeout,
seeded blink, canvas bounds를 검사한다. 서비스 테스트는 unavailable 응답과
capture count 0을 검사한다. 호스트 테스트 통과는 보드용 빌드 성공을 의미하지 않는다.

## ESP-IDF가 준비된 뒤의 빌드

권장 SDK는 **5.5.1**이다. 설치 범위는 ESP-IDF, ESP32-S3 cross compiler, SDK
Python 환경, CMake/Ninja 및 manifest의 Espressif components다. 설치 위치와
다운로드 범위를 결정한 뒤 공식 설치 절차를 실행해야 하며, 현재는 제안만 기록한다.

SDK의 `export.sh`로 활성화한 shell에서 다음을 실행한다.

```sh
cd firmware
idf.py --version
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

최초 configure/build는 `main/idf_component.yml`에 지정한 components를
다운로드할 수 있다. 이 절차는 아직 실행하지 않았다. 플래시와 monitor는 연결된
보드, 포트, 범위를 확인한 뒤 별도로 진행한다. 기본 console은 native USB Serial/JTAG다.

LCD는 공식 factory 설정인 SPI2 40MHz, mode 3, RGB565, inversion을 사용한다.
프레임 memory는 internal DMA 약 115KB이며 SPI 완료 callback을 기다리는 동안
UI는 frame을 덮어쓰지 않고 다음 tick으로 진행한다. allocation 또는 초기화 실패는
로그에 남기고 해당 작업을 멈추며 재부팅 loop를 만들지 않는다. touch polling은
별도 task여서 I2C read가 렌더 task를 막지 않는다.

## 로컬 설정과 후속 bring-up

`sdkconfig.defaults`에는 공개 가능한 기본값만 있다. `sdkconfig.example`의 빈
예약 항목은 향후 공식 adapter를 붙일 때 사용할 수 있으며 현재 firmware는
소비하지 않는다. 필요한 시점에 `sdkconfig.local`로 복사하고 다음처럼 configure한다.

```sh
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.local' reconfigure
```

`sdkconfig.local`, 생성된 `sdkconfig`, build, managed_components는 Git에서 제외한다.
Kconfig 문자열과 build 결과는 비밀값을 plaintext로 포함할 수 있으므로 저장소나
공개 artifact에 올리지 않는다. 현재 단계에 Wi-Fi·토큰 입력은 필요하지 않다.

다음 검증은 보드용 compile → USB 전원 LCD 색/방향 → PLUS/touch 입력 → codec
loopback 및 채널 확인 순으로 진행한다. PWR GPIO5와 battery enable GPIO2는 기본
앱에서 조작하지 않는다. 오디오 확인 후에도 같은 dot의 공식 외부 audio/session
transport를 검증하기 전에는 실제 통화 성공 상태를 표시하지 않는다.
