# 로컬 장치 프로토콜 v0.1

이 프로토콜은 네모씨 장치와 Mac mini의 로컬 중계 사이의 데모 규약이다.
dot의 공식 API가 아니며, 실제 dot 통화 transport의 지원 여부는 별도다.

## 상태 확인

`GET /api/health`는 서버 동작, `mode: "mock"`, 오디오 포맷, `dot_connected: false`,
현재 transport와 공식 연결 확인 상태를 반환한다. `GET /api/capabilities`는 현재 지원 범위를 반환한다.

## 한 번의 입력

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
  "turn_id": "opaque-id",
  "transcript": "안녕",
  "text": "데모 응답",
  "mode": "mock",
  "audio": {
    "url": "/api/audio/opaque-id.wav",
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

현재 mock turn은 동기 처리한다. 클라이언트는 AbortController로 요청을 끊고
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
