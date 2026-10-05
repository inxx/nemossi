# Mac mini 허브와 네모씨 장치 경계

지금은 Waveshare ESP32-S3-Touch-LCD-1.54를 기준으로 스마트안경 없이 개발한다.
Mac mini가 개발 호스트와 로컬 TTS 역할을 맡는다. 현재 목표는 사용자가 승인한
Muse PTT 단말이다. 기존 hub·시뮬레이터는 별도의 mock 경로로 유지한다.

## Muse 포트

`scripts/prepare_muse.py`가 고정한 공식 SDK의 voice-only 세션에 1.54 보드 backend와
기존 C 얼굴을 결합한다. PLUS 녹음 → Muse 텍스트 답 → `server/local_tts.py` →
16kHz mono PCM16 WAV → 보드 스피커 순서다. `tts_client.c`는 private IPv4의
명시적인 TTS URL만 사용한다. 세대 번호와 session commit 잠금으로 취소된 응답을
폐기하며 새 녹음 전에 player 종료를 확인한다. 기본+6표정은 대화 상태와 독립적으로
유지하고, 성공한 스피커 PCM 전송 진폭을 입에 적용한다.

공통 I2C, ES7210 TDM RX·ES8311 STD TX와 PA 제어는
`firmware/muse-port/main/voice_board_nemossi.c`에 있다. 오디오는 16kHz 반이중이고
오류·부분 write에서 PA를 끈다. SDK 6.0.1 cross build는 통과했지만 실물 부팅과
Muse 연결은 미검증이다. [설정·검증 경계](muse-connection.md).

## 기존 mock 구현

```text
브라우저 / CLI 네모씨 시뮬레이터
                ↕ 장치 이벤트와 상태, PCM16 WAV
           Mac mini HTTP 서버
                ↕
     장치 lifecycle ↔ 공통 hub ↔ MockTransport
```

- `server/device.py`는 단일 네모씨 시뮬레이터의 연결·상태·활성 turn을 관리한다.
- `server/hub.py`는 장치 lifecycle과 voice transport 사이에서 입력을 중계한다.
  장치가 취소·끊김·만료 상태로 바뀌면 이전 응답을 유효한 결과로 발행하지 않는다.
- `server/transport.py`는 응답 생성 경계다. 지금은 mock 텍스트와 합성 확인음을
  반환한다. 기존 dot 통화와 인증 세션을 새로 생성하지 않는다.
- `web/`와 `scripts/simulate.py`는 같은 장치 규약을 사용하는 시뮬레이터다.
  서버에서 응답 준비가 끝난 상태와 실제 브라우저 재생 상태를 구분한다.
- `firmware/main/`의 기존 데모는 얼굴·입력 골격이다. 새 Muse 포트는 위 별도
  생성 경로를 사용한다. UAC 실험은 checkpoint `2095601`에 보존하고 중단했다.

장치 연결 ID와 turn ID는 요청을 구분하고 늦은 응답을 막기 위한 상관 식별자다.
로그인·인증 토큰이 아니며 dot 세션을 뜻하지 않는다. 기존 HTTP 접근 제한을
그대로 적용하고 실제 비밀값은 만들거나 설정하지 않는다.

## 미래 장치 경계

향후 Mac mini가 네모씨와 별도 스마트안경 프로젝트의 공통 연결 지점이 될 수
있다. 안경 쪽은 폰 등의 외부 릴레이를 거칠 수 있으므로 직접 Mac 연결을
가정하지 않는다.

```text
네모씨 장치 ↔ Mac mini 공통 연결 지점 ↔ 외부 릴레이(폰 등) ↔ 스마트안경
```

이번 단계에는 `future_peer`를 예약·미지원 상태로 알리는 경계만 있다. 안경의
등록 API, SDK, Bluetooth 제어, 화면 렌더러 또는 실제 릴레이를 만들지 않았다.
다중 장치 라우팅·플러그인 플랫폼·장기 저장소도 현재 범위에 필요하지 않아
추가하지 않았다.

## 이전 같은 dot 조사

이전 목표는 사용자의 기존 dot과 통화하는 것이었다. 표준 USB 오디오 장치 경로는
공식 기술 기반이 있는 후보지만 보드와 실제 통화에서 미검증이다. 다른 연결
방식도 검토할 수 있으며 USB 전용 최종 형태를 확정하지 않았다.

사용자는 Muse + TTS로 방향 전환을 승인했다. 이전 dot 조사는 기록으로 남기며
새 Muse 대화를 기존 dot 유지로 설명하지 않는다. 시뮬레이터 연결 성공은 실제
Muse·dot·하드웨어 연결 성공을 뜻하지 않는다.
