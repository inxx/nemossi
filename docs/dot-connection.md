# 같은 dot 통화 연결 조사

> 2026-10-05: 사용자가 기존 얼굴을 유지한 Muse+TTS 전환을 승인했다.
> 아래는 이전 같은 dot 조사 기록이며, 현재 구현은 [Muse 연결](muse-connection.md)을 따른다.
> USB UAC 실험은 별도 `feat/usb-uac-dot` 브랜치에 보존했고 실물에 쓰지 않았다.

## 현재 판정: 실제 보드 통화 미검증, USB 오디오 경로는 개발 후보

목표는 사용자의 기존 dot과 통화하는 것이다. ESP32의 음성 스트림을 **그 dot의
기존 통화 세션**에 직접 붙이는 공개 API는 확인하지 못했다. 일반 OS 마이크·스피커
장치로 기존 통화 앱에 연결하는 경로는 별도로 검토하며, API 미확인만으로 해당
경로까지 불가능하다고 판단하지 않는다. 현재 보드에서 실제 통화가 되는지는 미검증이다.

2026-10-04에 확인한 공식 근거:

| 근거 | 확인된 내용 | 확인되지 않은 내용 |
| --- | --- | --- |
| [Message your dot](https://learn.chatgpt.com/docs/dots/channels) | dot 대화의 전화 버튼 또는 데스크톱 프로필의 Call로 같은 dot에 전화 | 외부 ESP32 클라이언트가 통화 세션에 참여하는 API |
| [ChatGPT Voice](https://learn.chatgpt.com/docs/features/voice) | 데스크톱 앱의 음성 시작·마이크 권한, iOS Remote 경로, 작업 간 음성 이동 | 외부 장치용 raw PCM 입력·출력 또는 기존 dot 세션 토큰 발급 |
| [Codex App Server](https://learn.chatgpt.com/docs/app-server) | 공식 Codex thread/turn 클라이언트 인터페이스 | 문서에 기존 dot의 실시간 외부 오디오 참여 방법은 명시되지 않음 |
| [Realtime API](https://developers.openai.com/api/docs/guides/realtime) | 별도 Realtime 세션을 WebRTC/WebSocket으로 구성하는 방법 | 새 API 에이전트를 기존 dot과 동일하게 연결한다는 보장 |

현재 제공된 음성 도구는 활성 음성 종료와 다른 Codex 작업으로의 이동을 다룬다. 하드웨어 PCM 연결 기능은 제공하지 않는다. 이 도구들을 시험하면서 활성 통화에 영향을 주는 조작은 하지 않았다.

## 일반 OS 오디오 장치 경로

네모씨를 표준 USB 마이크·스피커로 구현하고 Mac mini의 기존 dot 통화 앱에서
그 입출력을 쓰는 구성이 후보다. USB를 유일한 최종 형태로 확정하지 않는다.

- [Espressif USB Device UAC](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_device/usb_device_uac.html)는
  ESP32-S3의 마이크·스피커 콜백과 macOS용 옵션을 지원한다. Waveshare 보드의
  codec 모드·채널 변환·버퍼링은 추가 구현과 실물 검증이 필요하다. 확인한 factory
  소스에는 UAC 초기화가 없으며 현재 네모씨 펌웨어에도 없다.
- [Apple TN3190](https://developer.apple.com/documentation/technotes/tn3190-usb-audio-device-design-considerations)는
  macOS 내장 USB Audio class driver를 사용하는 설계를 안내한다. Mac의
  [입력](https://support.apple.com/guide/mac-help/change-the-sound-input-settings-mchlp2567/mac)과
  [출력](https://support.apple.com/guide/mac-help/change-the-sound-output-settings-mchlp2256/mac)은
  각각 선택할 수 있다. 실제 dot 앱의 장치 선택과 오디오 경로는 아직 시험하지 않았다.
- [S3 BLE 문서](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-guides/ble/overview.html)와
  [IDF 5.5.1 기능표](https://github.com/espressif/esp-idf/blob/v5.5.1/docs/en/api-guides/ble/ble-feature-support-status.rst)에
  따르면 현재 보드는 Classic HFP/A2DP와 표준 LE Audio의 BIS/CIS 경로를 지원하지
  않는다. BLE 데이터 통신을 일반 Bluetooth 헤드셋 지원으로 간주하지 않는다.

기존 dot의 desktop 웹·앱 통화 지원과 보드의 USB 오디오 구현 가능성은 공식 근거가
있지만, 둘을 실제로 연결한 증거는 없다. 새 모델이나 별도 AI 세션을 대신 생성하지 않는다.

## 구현 경계

서버의 `TransportAdapter`는 장치 프로토콜과 실제 통화 transport를 분리한다. `MockTransport`는 WAV·UI 흐름을 검증하며 `dot_connected`는 false다. `UnsupportedDotTransport`는 명확한 미지원 오류를 반환한다. 실제 구현이 준비되기 전 별도 모델을 자동 연결하지 않는다.

현재 Mac hub의 장치 연결 ID는 시뮬레이터 요청을 구분할 뿐 dot 세션이나 인증
정보를 만들지 않는다.

실제 adapter를 추가하려면 다음 근거가 필요하다.

1. 공식적으로 지원되는 세션 연결 또는 오디오 장치 경로.
2. 해당 경로가 사용자의 기존 dot을 유지한다는 확인.
3. 입력·출력 포맷, 인증 방법, 종료·재연결·중단 정책.
4. 사용자가 직접 수행해야 하는 통화 시작·장치 선택·권한 부여 단계.

## 지금 가능한 최소 사용자 동작

실제 같은 dot 통화는 ChatGPT에서 그 dot의 대화를 열고 전화 버튼으로 시작한다. 네모씨 데모는 로컬 UI에서 얼굴·음성 패킷·취소 흐름을 확인하는 단계다. 네모씨 하드웨어에서 실제 통화가 된다는 검증은 아직 없다.

앱 토큰을 읽거나 비공개 API를 호출하거나 OS 오디오 설정을 바꾸지 않는다. 가능한 공식 경로가 확인되면 필요한 사용자 동작을 구체적으로 안내한다.
