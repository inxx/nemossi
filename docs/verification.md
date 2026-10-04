# 검증 기록

검증일: 2026-10-04. 버전 0.2.0의 결과는 설치 없이 실행하는 **Mac mini mock
허브·네모씨 시뮬레이터와 공식 핀 기반 펌웨어 골격**이다. 아래 결과는 실제 같은
dot 통화나 보드 동작의 완료 판정이 아니다. 스마트안경 실행 코드는 포함하지 않는다.

## 실행한 검사

macOS의 Python 3.9.6, Node.js 22.23.1, clang 21에서 저장소 루트의
`python3 -B scripts/verify.py`를 실행해 모두 통과했다. 추가 패키지를 설치하지 않았다.
HTTP 검사는 실행 중에만 존재하는 loopback 서버를 사용한다.

| 검사 | 결과 | 확인한 내용 |
| --- | --- | --- |
| Python 기존 bridge | 16개 통과 | health/capabilities, 텍스트·WAV 응답, WAV 형식·길이, 요청 제한, audio TTL·용량, 경로·Origin·Host, LAN 토큰 경계 |
| Python hub/device | 24개 통과 | 연결 lease·상태 제한, 단일 연결 경합, 상태·turn 식별자, 재생 ACK, 취소·끊김·재연결 중 늦은 응답과 오디오 폐기, token·Origin 경계 |
| Python CLI 시뮬레이터 | 10개 통과 | 실제 HTTP 대화·취소·재연결·오류 복구, 합성 WAV 입력, 기존 연결 보존, 연결 생성 경합, timeout·응답 크기·loopback 제한 |
| Node 오디오·장치 모듈 | 15개 통과 | PCM16 WAV와 샘플 변환, Python 형식 호환, PCM 진폭 기반 입 움직임, 요청 직렬화, 취소·늦은 응답·본문 timeout, 재연결 ACK와 중복 폴링 방지 |
| JavaScript 문법 | 5개 파일 통과 | app/audio/device/face/recorder-worklet |
| 순수 C 얼굴 모델 | 8개 그룹 통과 | 상태 전환, 시간 제한, blink·gaze, demo, RGB565 렌더링·버퍼 경계 |
| C 서비스 경계 | 통과 | 오디오·dot 미연결에서 UNAVAILABLE을 반환하고 녹음을 시작하지 않음 |

C 호스트 검사는 AddressSanitizer와 UndefinedBehaviorSanitizer를 사용한다.
이번 독립 검토에서는 Node 14개와 Python 비 HTTP 14개가 통과했고,
suspend/start 교차 실행에서 재개된 폴링 loop가 하나인지 별도로 확인했다. 마지막
폴링 회귀 테스트가 추가된 뒤 전체 실행 결과는 위의 Node 15개다. 리뷰 환경은
소켓 생성 제한으로 HTTP 검사를 실행하지 못했으며, 담당자의 HTTP 결과와 구분한다.
남은 고·중요도 코드 결함은 발견되지 않았다.

기존 마이크 모의 자원 정리 검사에서는 취소 중 늦게 도착한 MediaStream과 정상
종료의 트랙·AudioContext 정리를 확인했다. 실제 마이크 권한·녹음 시험과는 별개다.

## v0.2 브라우저 확인

Mac mini의 별도 Codex 브라우저 탭에서 로컬 서버를 열었다. 실제 DOM·스크린샷과
읽기 전용 장치 상태 관찰로 다음 흐름을 확인했다.

- 연결 전 대화 버튼과 텍스트 입력 비활성화, 연결 확인 후 입력 가능.
- 텍스트와 합성 WAV 데모 모두 ready → speaking → idle 전이.
- 브라우저 재생의 `playback_start`와 실제 종료 후 `playback_end` ACK.
- 입력 중 취소 → idle, 이후 늦은 응답·재생 없음.
- 연결 해제 뒤 입력 비활성화, 재연결 때 새 connection ID 생성.
- 테스트용 mock 오류 이벤트를 폴링으로 표시하고 화면의 오류 복구로 idle 복귀.
- 볼륨·밝기 변경과 설정 재열기, 8개 표정, dot 미확인·외부 릴레이 예약 안내.
- 브라우저 오류·경고 로그 없음.

화면과 API는 실제 dot·보드를 연결 완료로 표시하지 않는다. 기본 마이크는
OFF로 유지했고 실제 마이크 권한·녹음을 시험하지 않았다.

## 기존 v0.1 브라우저 확인

로컬 서버를 Codex의 별도 브라우저 탭에서 실행해 다음 흐름을 확인했다.

- 텍스트 입력 → mock 응답 → 합성 확인음 → 대기 복귀.
- 기본 데모 말하기 → 0.75초 WAV 전송 → mock 응답·재생 → 대기 복귀.
- 요청 중 취소 → 대기 복귀, 늦은 응답이 새 메시지나 재생을 만들지 않음.
- 볼륨·밝기 설정과 8개 표정 선택, dot 연결 미확인 안내.
- 브라우저 오류·경고 로그 없음.

기본 데모에는 마이크 권한이 필요 없다. 실물 마이크 권한·녹음은 시험하지 않았다.
브라우저 표시와 C 렌더 프레임도 시각적으로 검토했다.

## 펌웨어 근거와 한계

LCD·공유 I2C·터치·PLUS·오디오 핀은 Waveshare 공식 저장소의 고정된
[`5157db7c888e476fd57f8a95020800377447478f`](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/tree/5157db7c888e476fd57f8a95020800377447478f)
factory 예제와 회로도에 대조했다. 상세 링크·회로도 해시·SDK 버전 불일치는
[hardware.md](hardware.md)에 기록한다. 기존 upstream 코드나 얼굴 에셋을 복사하지 않았다.

| 남은 검증 | 현재 상태 | 다음 증거 |
| --- | --- | --- |
| 같은 dot 통화 | 공식 외부 오디오 연결·실제 사용 미검증 | 동일 dot 확인, 선택한 장치로 실제 통화 |
| ESP-IDF cross build | SDK 미설치로 미실행 | ESP-IDF 5.5.1 환경의 빌드 로그 |
| 보드 LCD·터치·PLUS | 실물 미검증 | 연결된 보드에서 입력·표시 확인 |
| ES7210/ES8311 오디오 | 초기화·녹음·재생 미구현 | 공식 codec 모드에 맞춘 드라이버와 실물 오디오 검증 |
| 펌웨어·Mac hub 통신 | 브라우저·CLI 흐름만 검증 | 실물의 장치 요청·응답·재연결 확인 |
| USB 오디오 후보 | UAC 공식 자료만 확인 | macOS 장치 인식, codec stream, 동일 dot 통화 |
| 미래 스마트안경 peer | 외부 릴레이 예약 정보만 제공 | 별도 프로젝트에서 폰 중계 등 실제 경로 확인 |

ESP-IDF 설치, 보드 flash, serial monitor, 실제 마이크·스피커 시험, 자격 증명 설정,
유료 API 호출과 지속 서비스 등록은 수행하지 않았다. 공식 dot 연결 근거가 확보되기
전에는 새 Realtime API 모델을 기존 dot으로 표시하지 않는다.

## 재현

```sh
python3 scripts/verify.py
python3 -m server --host 127.0.0.1 --port 8765
```

브라우저에서 <http://127.0.0.1:8765>를 열고 연결을 누른 뒤 데모 말하기 또는 텍스트 입력을 사용한다.
현재 모든 응답은 명시적으로 mock이며 오디오 응답은 합성 확인음이다.
