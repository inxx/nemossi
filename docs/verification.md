# 검증 기록

## 2026-10-05 Muse + Mac 로컬 TTS 포트

사용자가 승인한 새 PTT 경로를 구현했다. 기존 `firmware/model/face.c`와 model
headers·LCD driver·GPIO 정의는 그대로 생성 프로젝트에 복사하며 기본+6표정
렌더 회귀가 통과했다. 기존 browser mock 경로는 유지한다. [연결 절차](muse-connection.md).

최종 소스에서 `NEMOSSI_MUSE_PROJECT=<staged esp32> python3 -B scripts/verify.py`가
통과했다. 이 실행에서는 Muse 관련 검사가 skip되지 않았다.

| 검사 | 결과 |
| --- | --- |
| Python HTTP·hub·CLI·Mac TTS·이미지 gate | 73개 통과: 기존 50 + TTS 16 + 이미지 7 |
| Node 오디오·장치·얼굴 | 기존 29개 통과 |
| JavaScript 문법 | 기존 5개 파일 통과 |
| C 얼굴·서비스 | 기존 얼굴 15개 그룹·서비스 검사 통과, ASan·UBSan |
| 생성한 Muse 세션·client·player | 15개 테스트 통과, ASan·UBSan |
| 1.54 실제 audio backend의 host harness | 5개 그룹 통과, ASan·UBSan |
| ESP-IDF 6.0.1 ESP32-S3 cross build | 성공, 토큰·TTS 설정 비어 있음 |
| 생성 config·partition MD5·ESP checksum/SHA | 통과, 기존 partition geometry와 일치 |

세션/client harness는 6,413개 assert로 부분 stream write·backpressure·세대 교체·
취소 commit 경합·초기화 OOM·잘못된 HTTP/WAV·deadline·비공개 IPv4 제한을
검사한다. 실제 생성 player·voice 코드에는 마지막 I2S 실패 전달, stop 뒤 500ms
종료 대기, timeout 때 녹음 차단, OOM 때 worker 미생성 검사가 있다. 수정 전
코드의 해당 3개 실패도 같은 oracle로 재현했다. 보드 backend는 TDM/STD 계약,
48k stereo32 → 16k mono16 변환, codec sticky fault, volume·mute readback과
부분 write 때 PA 차단을 검사한다. 이것은 실물 I2C/I2S 증거가 아니다.

추가로 공식 SDK host suite를 실행했다(165개 실행). 새 포트의 log namespace
차이를 수정한 뒤 공식 diagnostic 검사 6개가 모두 통과했다. 전체 suite는 통과로
표시하지 않는다. 남은 차이는 추가 board 분기를 인식하지 못하는 기존 e-paper
CMake regex, 새 TTS 함수 stub이 없는 stock chat harness, 의도적으로 제외한
avatar fixture에 의존하는 serialchat import다. 변경한 세션은 위 전용 harness로
검증했다. host PSA/mbedcrypto 부재로 Noise core 검사는 skip되었고, IDF 소스로
실행하는 실제 ECDH·ECDSA·AES-GCM·페어링 암호화 검사는 통과했다.

Mac에 설치된 Yuna로 중립 한국어 문장을 파일로 합성했다. canonical WAV는
31,440 bytes·16kHz mono PCM16·0.981125초였고 임시 파일은 정리됐다. 사용자
음성 녹음·실제 Muse 답·스피커 재생은 수행하지 않았다. LAN 서버와 지속 서비스도
활성화하지 않았다.

최종 빌드 확인용 앱은 **1,475,872 bytes**이며 SHA256은
`ea761ba1e2c563e52bc3b87f03a2fcaf52a770912c57fabf4d9c304120d3f679`다.
기존 app0 `0x10000..0x340000`에 들어가고 예상 4KiB sector erase 범위는
`0x10000..0x179000`(끝 주소 제외)다. bootloader·table·otadata를 쓰지 않는
계획이며 eFuse/NVS encryption·Secure Boot·flash encryption·앱 서명·OTA·터널·
원격 bug report가 모두 꺼져 있음을 검사했다. **이 이미지는 업로드 승인 대상이나
연결 완료 이미지가 아니다.** 사용자 로컬 토큰·TTS 설정 후 재빌드하면 hash가
바뀌므로 다시 검사하고 그 정확한 이미지의 승인을 받아야 한다.

원본 16MiB 백업과 전체 app0 복구 slice를 로컬 0600으로 보존했다. 복구 쓰기는
시험하지 않았다. 원본 bootloader와 새 앱의 실제 호환성·실물 LCD/터치·마이크·
스피커·Muse 계정·페어링·Wi-Fi·LAN 통신은 미검증이다. 새 firmware write는 없다.
이하 기록은 이전 얼굴·mock 개발 시점의 검사 결과다.

## 2026-10-05 여섯 파생 표정 추가

공식 기본 얼굴을 유지한 기쁨·궁금함·생각 중·놀람·졸림·시무룩함을 추가했다.
기쁨·졸림·시무룩함은 원본 HAPPY/SLEEPY/SAD 눈꺼풀 모티프이며 전체 여섯
표정의 수치는 네모씨 파생 디자인으로 구분했다. 동작·통화 상태와 별개의
표정 select와 C setter를 사용한다. [표정 계약](expressions.md).

최종 소스에서 `python3 -B scripts/verify.py`가 통과했다: Python 50개,
Node 29개, JavaScript 문법 5개, C 얼굴 15개 그룹과 기존 서비스 경계 검사.
C는 ASan·UBSan을 사용한다. C의 독립 7행 도형 oracle로 기본+6표정,
9개 상태, 13단계 blink 및 speaking 0/50/100을 교차 대조했다. 기본 C 화면
13개는 이전 cfb8a32 출력과 바이트 단위로 일치하며 잘못된 expression 입력,
표정 setter의 단일 필드 변경과 clock·deadline·상태 보존도 확인했다.

JS는 이전 렌더러의 9,648개 기본 geometry 조합과 일치했다. 독립 리뷰에서
부동소수점 반올림 경계 2개의 차이를 발견해 폭 산식의 기존 평가 순서를
보존하고 회귀 assert를 추가했다. 수정 후 이전 입 구현과 경계·인접 ULP·호흡
offset 조합 2,439개를 별도로 대조해 모두 일치했다. C 실제 출력 48개와
JS의 눈 중심·반지름·cap 단계·입 native/projected 도형도 일치했다.

실제 브라우저에서 한국어 이름의 기본+6표정을 선택했고 시트에서 나란히
확인했다. 비교 시트는 실제 Canvas 렌더러를 사용한다. 기본 240×240 화면과
cfb8a32의 픽셀 차이는 0개였으며 8상태×3입열림×3motion의 기본 geometry
72개도 일치했다. JPEG 시트와 7개의 native PNG를 별도로 보존했다.

자동 합성 WAV 데모가 소리를 재생하는 중 궁금함 → 기쁨으로 변경해도
재생 상태를 유지했고, 완료 후 idle로 돌아오며 기쁨 선택을 유지했다. 실제
마이크 권한은 요청하지 않았고 기본 OFF를 유지했다. 궁금함 표정의 자연
blink 80프레임에서 눈 영역의 흰 픽셀 수가 열린 상태 199에서 닫힌 상태 0으로
줄었으며 원본 JPEG를 그대로 보존했다. reduced motion은 단위 검사와 실제
브라우저의 정지 렌더 시트로 확인했다. 브라우저 media emulation이나 사용자
OS 설정 변경은 수행하지 않았다.

서버·simulator protocol·오디오 모듈·보드 I/O 등 보호 파일 21개는 이전
commit과 바이트 단위로 일치한다. 표정 select 외의 앱 제어를 유지했고 새
패키지·SDK·flash·마이크 권한 변경은 수행하지 않았다. 실물·같은 dot 통화의
기존 미검증 경계는 그대로다.

## 2026-10-05 공식 기본 얼굴 수정

공식 Stack-chan commit `2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d`의
기본 SimpleFace와 실제 README GIF를 대조했다. 검정 배경·흰 원형 눈·넓은
직사각형 입을 0.75배로 투영하고 위아래 30px 여백을 두었다. 원본 좌표와
수정 전후 수치는 [face-reference.md](face-reference.md)에 기록했다.

저장소 루트의 `python3 -B scripts/verify.py`가 통과했다.

| 검사 | 최신 결과 |
| --- | --- |
| Python HTTP·hub·CLI | 기존 50개 통과 |
| Node 오디오·장치·얼굴 | 23개 통과: 기존 15개와 새 얼굴 8개 |
| JavaScript 문법 | 기존 5개 파일 통과 |
| C 얼굴 모델·렌더러 | 12개 그룹 통과, ASan·UBSan 포함 |
| C 서비스 경계 | 기존 검사 통과 |

C 검사는 얼굴 영역 전체를 독립 도형 oracle과 대조했다. 눈꺼풀 13단계,
감정 마스크, 시선·호흡 이동, 입 0/50/100의 경계와 내부 채움, 버퍼·invalid
input·timeout·희소 tick·UINT64_MAX 경계를 포함한다. C 출력 모션 188개
(seed 4개, 안전한 Number를 넘는 uint64 표본 8개 포함)와 입 도형 3개도 JS와
별도로 비교했다. 정수 필드는 일치했고 실수 최대 차이는 `1.11e-16`이었다.
큰 시간은 BigInt로 33ms 양자화와 공통 주기 나머지를 계산해 정밀도 손실 없이
대조했다. 독립 리뷰에서도 Node·C 검사와 원본 수식 대조를 통과했다.

실제 별도 브라우저 탭에서 대기, 수동 듣기·말하기 표정과 자연 깜박임을
확인했다. 깜박임은 실제 화면 80프레임을 9.75초간 관찰했으며 눈 영역의
흰 픽셀 수가 열린 상태 210에서 닫힌 상태 7로 줄어들었다. JPEG 원본을
그대로 보존했으며 원형 눈을 위에서 가리는 눈꺼풀을 직접 확인했다.
자동 모드의 합성 WAV 데모도 실제 브라우저에서 듣기 → 재생 준비 → 소리 재생
→ 서버 대기 → idle로 복귀했다. 발화 화면의 입 높이·너비 변화와 최종 장치의
active turn 없음·오류 없음·dot 및 hardware 미연결 표시를 확인했다. 이번
최종 조회의 last_event는 heartbeat였으므로 그 값 자체를 재생 종료 ACK
증거로 주장하지 않는다. ACK 계약은 위 HTTP·Node 통합 검사가 확인한다.

비교 페이지는 원본 고정 좌표를 별도로 그린 기준, e8366e5의 실제 이전 렌더러,
수정 후 실제 렌더러를 함께 표시한다. 숨·시선을 정지한 기본 얼굴의 240×240
전체 Canvas ImageData 대조에서 기준과 수정 후의 다른 픽셀은 0개였다.
이 결과는 같은 브라우저의 도형 대조다. C RGB565의 이진 픽셀 경계와 Canvas
안티앨리어싱이 완전히 같은 비트라는 의미는 아니다.

서버·protocol·앱 제어·오디오·CSS·보드 I/O의 23개 보호 파일은 이전 commit과
바이트 단위로 일치한다. 기존 dot·실물·SDK 미검증 경계는 그대로다. 원본
얼굴 수식의 adaptation에 대한 Apache 2.0 전문과 출처·수정 고지를 보존했다.
32개 seeded 구간 반복은 downstream scheduling 수정으로 명시했다.

## 이전 v0.2 검증 기록

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
[hardware.md](hardware.md)에 기록한다. 보드 드라이버는 기존 골격을 유지한다.
2026-10-05의 얼굴 수정은 별도의 Stack-chan 수식 adaptation이며
[출처·라이선스 고지](../THIRD_PARTY_NOTICES.md)를 포함한다.

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
