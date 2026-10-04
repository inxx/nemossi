# 네모씨 · nemossi

**같은 dot과 통화하는 작은 단말**을 만드는 프로젝트입니다. 대상은 Waveshare
ESP32-S3-Touch-LCD-1.54와 Mac mini입니다. Stack-chan 스타일의 얼굴을 단순한
도형으로 그리고, 말하기·듣기·생각하기·재생 상태를 표시합니다.

현재 결과는 **설치 없이 실행되는 로컬 데모와 ESP-IDF 펌웨어 골격**입니다.
실제 dot 통화의 외부 하드웨어 연결은 공식 경로가 아직 확인되지 않았습니다.
데모 응답과 합성 확인음은 실제 dot·음성인식·TTS가 아닙니다.

## 바로 실행

Python 3.9 이상으로 저장소 루트에서 실행합니다. 패키지 설치와 API 키가 필요 없습니다.

```sh
python3 -m server --host 127.0.0.1 --port 8765
```

브라우저에서 <http://127.0.0.1:8765>를 엽니다.

1. 데모 말하기를 누르거나 문장을 입력합니다.
2. 듣기 → 생각하기 → 응답 재생 상태와 입 움직임을 확인합니다.
3. 취소를 누르면 입력·네트워크 요청·재생을 중단하고 대기 상태로 돌아갑니다.
4. 설정에서 볼륨·밝기·표정을 확인합니다. 실제 마이크는 사용자가 선택한 경우에만 권한을 요청합니다.

마이크 입력도 현재는 로컬 mock에 전달됩니다. 인식된 문장을 만들지 않으며, 입력 음성은 저장하지 않습니다.
기본 서버는 이 컴퓨터의 loopback에만 연결됩니다. 지속 실행 서비스로 등록하지 않습니다.

## 검증

Python 3.9+, Node.js 22+, C 컴파일러(clang 또는 cc)가 있으면 다음 명령으로
HTTP 통합·브라우저 오디오 모듈·펌웨어 모델을 함께 검사합니다.

```sh
python3 scripts/verify.py
```

서버만 검사할 때는 다음 명령을 사용합니다.

```sh
python3 -m unittest discover -s tests -v
```

호스트 C 테스트는 ESP-IDF 컴파일이나 실제 LCD·터치·마이크·스피커 동작을 증명하지 않습니다.
검증 범위와 결과는 [verification.md](docs/verification.md)에 기록합니다.

## 구조

```text
server/        Python 표준 라이브러리 서버와 transport 경계
web/           240×240 얼굴, 데모 조작, 선택적 마이크 입력
firmware/      ESP-IDF 5.5 대상 보드 bring-up 골격과 순수 C 얼굴 모델
tests/         HTTP 및 오디오 프로토콜 검증
docs/          요구사항, 공식 핀맵, 연결 조사와 검증 범위
```

## 실물 보드로 진행할 때

[공식 핀맵과 빌드 준비](docs/hardware.md), [펌웨어 안내](firmware/README.md)를 확인합니다.
ESP-IDF 설치·보드 flash·serial monitor는 현재 수행하지 않았습니다.
Wi-Fi 비밀번호와 장치 토큰을 커밋하거나 코드에 하드코딩하지 않습니다.
`sdkconfig` 등 로컬 설정과 빌드 산출물은 Git에서 제외됩니다.

## dot 통화 연결

목표와 [공식 연결 조사](docs/dot-connection.md)를 유지합니다. 현재 `mock` adapter는
장치 흐름 검증용입니다. 새로운 LLM이나 Realtime API 세션을 기존 dot으로 표시하지 않습니다.
공식 연결을 확인한 뒤 `TransportAdapter` 구현을 추가합니다.

- [요구사항](docs/requirements.md)
- [장치 프로토콜](docs/protocol.md)
- [공식 Waveshare 자료](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)
- [Stack-chan](https://github.com/stack-chan/stack-chan): 얼굴 표현 참고. 기존 이미지·에셋을 복사하지 않았습니다.
