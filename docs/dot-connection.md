# 같은 dot 통화 연결 조사

## 현재 판정: 공식 외부 하드웨어 오디오 연결 미확인

목표는 사용자의 기존 dot과 통화하는 것이다. 현재 확인한 공개 문서와 이 작업에 제공된 도구만으로 ESP32의 음성 스트림을 **그 dot의 기존 통화 세션**에 붙이는 공식 경로를 확정하지 못했다. 이는 제품에 가능한 경로가 전혀 없다는 단정이 아니다.

2026-10-04에 확인한 공식 근거:

| 근거 | 확인된 내용 | 확인되지 않은 내용 |
| --- | --- | --- |
| [Message your dot](https://learn.chatgpt.com/docs/dots/channels) | dot 대화의 전화 버튼 또는 데스크톱 프로필의 Call로 같은 dot에 전화 | 외부 ESP32 클라이언트가 통화 세션에 참여하는 API |
| [ChatGPT Voice](https://learn.chatgpt.com/docs/features/voice) | 데스크톱 앱의 음성 시작·마이크 권한, iOS Remote 경로, 작업 간 음성 이동 | 외부 장치용 raw PCM 입력·출력 또는 기존 dot 세션 토큰 발급 |
| [Codex App Server](https://learn.chatgpt.com/docs/app-server) | 공식 Codex thread/turn 클라이언트 인터페이스 | 문서에 기존 dot의 실시간 외부 오디오 참여 방법은 명시되지 않음 |
| [Realtime API](https://developers.openai.com/api/docs/guides/realtime) | 별도 Realtime 세션을 WebRTC/WebSocket으로 구성하는 방법 | 새 API 에이전트를 기존 dot과 동일하게 연결한다는 보장 |

현재 제공된 음성 도구는 활성 음성 종료와 다른 Codex 작업으로의 이동을 다룬다. 하드웨어 PCM 연결 기능은 제공하지 않는다. 이 도구들을 시험하면서 활성 통화에 영향을 주는 조작은 하지 않았다.

## 구현 경계

서버의 `TransportAdapter`는 장치 프로토콜과 실제 통화 transport를 분리한다. `MockTransport`는 WAV·UI 흐름을 검증하며 `dot_connected`는 false다. `UnsupportedDotTransport`는 명확한 미지원 오류를 반환한다. 실제 구현이 준비되기 전 별도 모델을 자동 연결하지 않는다.

실제 adapter를 추가하려면 다음 근거가 필요하다.

1. 공식적으로 지원되는 세션 연결 또는 오디오 장치 경로.
2. 해당 경로가 사용자의 기존 dot을 유지한다는 확인.
3. 입력·출력 포맷, 인증 방법, 종료·재연결·중단 정책.
4. 사용자가 직접 수행해야 하는 통화 시작·장치 선택·권한 부여 단계.

## 지금 가능한 최소 사용자 동작

실제 같은 dot 통화는 ChatGPT에서 그 dot의 대화를 열고 전화 버튼으로 시작한다. 네모씨 데모는 로컬 UI에서 얼굴·음성 패킷·취소 흐름을 확인하는 단계다. 네모씨 하드웨어에서 실제 통화가 된다는 검증은 아직 없다.

앱 토큰을 읽거나 비공개 API를 호출하거나 OS 오디오 설정을 바꾸지 않는다. 가능한 공식 경로가 확인되면 필요한 사용자 동작을 구체적으로 안내한다.
