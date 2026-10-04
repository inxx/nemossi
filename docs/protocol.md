# 로컬 장치 프로토콜 v0.2

이 프로토콜은 네모씨 장치와 Mac mini의 로컬 중계 사이의 데모 규약이다.
dot의 공식 API가 아니며, 실제 dot 통화 transport의 지원 여부는 별도다.

## 상태 확인

`GET /api/health`는 서버 동작, `mode: "mock"`, 오디오 포맷, `dot_connected: false`,
현재 transport와 공식 연결 확인 상태를 반환한다. `GET /api/capabilities`는 현재 지원 범위를 반환한다.

## 네모씨 시뮬레이터 연결

`GET /api/hub`는 Mac mini 공통 연결 정보, 네모씨 장치 snapshot, 실제 dot
미연결 상태와 미래 외부 릴레이의 예약 상태를 반환한다. `GET /api/device`는
현재 단일 시뮬레이터 snapshot을 반환한다.

```json
{
  "connected": true,
  "connection_id": "0123456789abcdef0123456789abcdef",
  "revision": 3,
  "state": "idle",
  "active_turn_id": null,
  "last_event": "connect",
  "last_error": null,
  "device_kind": "nemossi_simulator",
  "hardware_tested": false,
  "dot_connected": false
}
```

`POST /api/device/connect`에 `{}`를 전송하면 snapshot을 반환한다. 이미 연결된
경우 기존 연결을 그대로 반환한다. `X-Device-Connection-Created: true|false`
응답 헤더로 새 연결 생성 여부를 구분하므로 CLI는 다른 클라이언트가 먼저 연결한
상태를 가져오지 않는다. 이 ID는 인증 토큰이나 실제 dot 세션이 아니다.

`POST /api/device/disconnect`는 `{"connection_id":"..."}`를 받는다.
`POST /api/device/event`는 다음 JSON을 받는다.

```json
{"connection_id":"...", "event":"playback_start", "turn_id":"..."}
```

| 이벤트 | 전이 |
| --- | --- |
| `heartbeat` | 현재 연결의 lease 연장 |
| `listen` | idle → listening, 활성 turn ID 생성 |
| `cancel` | 연결된 상태 → idle, 이전 작업 무효화 |
| `playback_start` | ready → speaking |
| `playback_end` | speaking → idle |
| `playback_error` | ready/speaking → error |
| `recover` | error → idle |

재생 이벤트에는 현재 활성 turn ID가 필요하다. 이전 연결·취소된 turn·허용되지
않은 상태의 이벤트는 거절한다. heartbeat 등 유효한 장치 활동이 60초 동안 없으면 lazy expiry로
offline 상태가 된다. listening/thinking/ready/speaking은 각각 20초의 상태 제한을
사용한다. 실제 WAV 길이는 여전히 최대 15초이며 listening의 여유 시간은 입력
준비와 업로드를 위한 것이다.
재연결은 disconnect → connect 순서로 새 connection ID를 만들어 이전 요청을
재사용하지 않는다. 연결된 상태에서 connect만 호출하면 기존 연결을 반환한다.

## 장치 입력과 재생 확인

`POST /api/device/turn`은 아래 기존 turn 입력 형식과 WAV 제한을 그대로 사용한다.
`X-Device-Connection-ID` 헤더가 필요하다. 듣기 상태에서 보낼 때는
`X-Device-Turn-ID`에 `listen`으로 받은 활성 turn ID를 넣는다. idle에서 텍스트를
보내면 서버가 새 turn을 시작한다.

서버 상태는 thinking → ready이며 응답은 기존 turn 응답과 `device` snapshot을
함께 반환한다. 응답의 `turn_id`는 활성 turn ID와 같다. HTTP 응답 완료를 실제
재생 완료로 간주하지 않는다. 브라우저가 재생을 시작하고 끝낼 때 각각 ACK를
보내 speaking → idle로 전환한다. CLI ACK는 명시적으로 시뮬레이션이다.

취소·연결 해제·만료가 응답 대기 중 발생하면 늦은 transport 결과를 거절하고
그 오디오를 발행하지 않는다. 장치 상태와 실제 dot 연결 성공은 별개다.

## 기존 단일 turn 입력

텍스트 데모는 `POST /api/turn`에 `Content-Type: application/json`으로 보낸다.

```json
{"text": "안녕", "session_id": "demo-session"}
```

음성 입력은 같은 endpoint에 `Content-Type: audio/wav`로 WAV 바이트를 보낸다.
세션 이름은 선택적인 `X-Session-ID` 헤더다. 입력은 **16kHz, mono, signed PCM16,
최대 15초**이며 임의 파일·압축 오디오는 거절한다.

응답 예시:

```json
{
  "turn_id": "0123456789abcdef0123456789abcdef",
  "transcript": "안녕",
  "text": "데모 응답",
  "mode": "mock",
  "audio": {
    "url": "/api/audio/fedcba9876543210fedcba9876543210.wav",
    "sample_rate": 16000,
    "format": "wav"
  },
  "duration_ms": 500
}
```

음성 입력의 `transcript`는 인식 결과 대신 데모 입력 길이를 알리는 표시다.
`audio.url`은 같은 서버의 임시 WAV다. 응답 음성은 말소리가 아닌 합성 확인음이다.
오디오는 메모리에 제한된 개수로 유지되며 만료 후 조회할 수 없다. 녹음 파일은 디스크에 저장하지 않는다.

## 취소·오류

현재 mock transport 교환은 동기 처리한다. 장치 lifecycle은 교환 중에도 다른
HTTP 요청의 취소·끊김을 받고 결과 발행 전에 유효성을 다시 검사한다.
클라이언트는 AbortController로 요청을 끊고
오래된 응답을 버리며, 입력과 재생 리소스를 정리한다. HTTP 연결을 끊는 것만으로
미래의 실제 dot 작업 취소를 보장하지 않으므로, 실제 adapter에는 별도의 세션 취소·중단 정책이 필요하다.

오류는 HTTP 상태와 JSON 오류 객체로 응답한다. 실제 dot transport를 선택해도
연결 성공을 가장하거나 다른 모델로 자동 대체하지 않는다.

## 외부 장치 연결 준비

기본 서버는 loopback용이다. LAN 연결은 명시적인 host 옵션과 로컬 환경의
`NEMOSSI_DEVICE_TOKEN`을 요구한다. 토큰은 `Authorization: Bearer …`로 전송한다.
원격 연결과 토큰 설정은 실물 통합 단계에서 따로 진행한다. 현재 데모에는 토큰이 필요 없다.
HTTP 토큰은 암호화되지 않으므로 실제 음성·자격 증명을 LAN으로 보내기 전
TLS 또는 안전한 로컬 장치 오디오 경로를 먼저 확정한다.
