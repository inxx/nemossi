# 공식 Stack-chan 얼굴 대조와 수정

검증 대상은 기본 **SimpleFace**다. 사용자가 처음 지정한 공식 저장소 링크는
보존되어 있지만 별도의 원본 사진이나 커스텀 얼굴 설정은 없었다. 따라서 공식
기본 브랜치의 commit
[`2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d`](https://github.com/stack-chan/stack-chan/tree/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d)를
고정해 실제 README 사진과 기본 얼굴 코드를 직접 대조했다.

## 원본 근거

Stack-chan은 **meganetaaan**(Shinya Ishikawa)과 커뮤니티가 개발·공개했다.
공식 저장소: <https://github.com/stack-chan/stack-chan>.

![Stack-chan 공식 README 사진](https://raw.githubusercontent.com/stack-chan/stack-chan/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/docs/images/stackchan.gif)

이 공식 사진에는 이전 3버튼 M5Stack이 나온다. 현재 표준 하드웨어와 혼동하지
않으며 눈·입의 수치는 같은 commit의 현재 SimpleFace 소스를 기준으로 한다.
사진의 검정 배경·흰 점 눈·가로 입과 소스의 원형 눈·직사각형 입이 일치한다.

- [기본 simple 선택](https://github.com/stack-chan/stack-chan/blob/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/firmware/host/app/default-behavior/on-context-created.ts)
- [SimpleFace 좌표](https://github.com/stack-chan/stack-chan/blob/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/firmware/host/modules/ui/components/face/behaviors/face.ts)
- [눈·eyelid](https://github.com/stack-chan/stack-chan/blob/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/firmware/host/modules/ui/components/face/parts/eye.ts), [입](https://github.com/stack-chan/stack-chan/blob/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/firmware/host/modules/ui/components/face/parts/mouth.ts)

## 이전 화면과 바뀐 수치

| 항목 | 이전 브라우저 e8366e5 | 수정 후 공식 비율 |
| --- | --- | --- |
| 배경 / 전경 | 흰색 / 짙은 검정 | 검정 / 흰색 |
| 눈 | 17×32 둥근 세로 막대 | 지름 12의 원형 |
| 눈 중심 | (81,105), (159,105) | (67.5,99.75), (172.5,102) |
| 눈 중심 간격 | 78 | 105 |
| 무음 입 | 중심(120,151), 27×6 둥근 막대 | 중심(120,141), 67.5×6 직사각형 |
| 발화 최대 입 | 25×29 둥근 막대 | 37.5×43.5 직사각형 |
| 깜박임 | 눈 높이 축소 | 고정 원형 눈에 원본 eyelid mask |
| listening | 커진 눈·작은 동그란 입 | 원본 NEUTRAL 도형 유지 |

수정 전 C는 브라우저와도 다른 배경색·상태별 색·큰 타원 눈·흰 하이라이트와
중공 입을 사용했다. 이를 같은 원본 파라미터로 맞췄다.

원본은 320×240이다. 전체를 0.75배로 줄여 240×180에 넣고 위아래 30픽셀
여백을 둔다. 즉 `x'=0.75*x`, `y'=30+0.75*y`다. 원형 눈과 간격을 축마다
다르게 늘이지 않는다. 입은 원본 Port의 내부 위치·너비·높이를 먼저 반올림하고
투영하므로 최종 픽셀 경계에는 1픽셀 수준의 raster 차이가 있을 수 있다.

## 모션과 상태

눈 열림은 원본처럼 12단계로 반올림한다. 검정 eyelid는 원의 위쪽부터 가린다.
깜박임은 열림 대기 400–5000ms, 전환 200–400ms 범위와 첫 1/4의 선형 닫힘,
나머지 3/4의 이차식 열림을 따른다. 눈 열림의 모델 최소값은 0.2다.

입 열림 `o`는 기존 음량 입력 0..1이다. 원본 native 식 `w=90−40o`,
`h=8+50o`를 사용한다. 무음이면 넓고 얇은 선으로 돌아가며 새 TTS나 음성
transport를 만들지 않는다. reduced motion에서도 이 음량 입력은 유지한다.

호흡은 6000ms 주기와 원본의 8단계 sine 양자화·6픽셀 진폭을 사용한다.
시선은 눈 자체만 작은 양으로 이동하며 얼굴 전체나 입을 끌고 가지 않는다.
원본에는 listening 전용 도형이 없다. idle/listening/thinking은 NEUTRAL,
happy/error/confused/sleep은 각각 원본 HAPPY/SAD/DOUBTFUL/SLEEPY eyelid를
사용한다. Sleep은 눈 열림 0, C의 blocked는 DOUBTFUL이다.

원본의 매번 무작위 구간 선택을 그대로 실행하는 대신 JS/C 대조와 큰 시간
건너뛰기 처리를 위해 같은 seed의 32개 구간을 반복한다. 구간 범위·33ms motion
샘플링·원본 easing·호흡·시선 식은 유지한다. 이는 downstream scheduling
수정이며 원본 firmware를 실행했다는 의미가 아니다.

공통 기준은 [fixture](../tests/fixtures/stackchan-simple-face.json),
브라우저의 STACKCHAN_DESIGN과 C의 face_design.h다. C의 기존 DEMO/STALE/state
진단 글자는 위아래 여백에만 유지하고 얼굴 도형의 픽셀 검증과 분리한다.

## 범위와 재사용 조건

수정 범위는 얼굴 렌더러·모션·관련 검증과 이 대조 기록이다. Mac hub, 단말
protocol, 기존 dot 경계와 보드 I/O는 바꾸지 않는다. 실물·SDK 빌드·flash는
수행하지 않는다. 화면 결과는 계속 mock이며 실제 같은 dot 통화는 미검증이다.

Apache 2.0 전문을 보존하고 수정한 파일의 출처·수정 표시를 남겼다. 공식
사진을 포함하는 비교에는 Stack-chan, meganetaaan과 공식 저장소 URL의 크레딧을
넣는다. [라이선스·표기](../THIRD_PARTY_NOTICES.md).
