# 기본 얼굴과 네모씨 파생 표정

기본은 공식 Stack-chan SimpleFace다. main cfb8a32의 검정 배경, 흰 원형 눈,
직사각형 입, 좌표·비율·모션을 유지한다. **기쁨, 궁금함, 생각 중, 놀람,
졸림, 시무룩함**은 그 위에 추가한 네모씨 파생 디자인이다. 공식 원본에
이 여섯 표정 이름과 파라미터가 그대로 제공된다는 의미는 아니다.

Stack-chan은 meganetaaan과 커뮤니티가 개발·공개했다:
<https://github.com/stack-chan/stack-chan>. 원본 commit은
`2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d`다. 기쁨·졸림·시무룩함은
공식 [Eye의 HAPPY/SLEEPY/SAD 눈꺼풀](https://github.com/stack-chan/stack-chan/blob/2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d/firmware/host/modules/ui/components/face/parts/eye.ts)
모티프를 활용한다. 출처·Apache 2.0 전문·수정 고지는
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)에 보존한다.

## 선택과 동작

시뮬레이터 아래 **표정**에서 기본 또는 여섯 표정을 선택한다. 별도의
**동작 확인**은 이전 대기·듣기·생각·말하기 등의 상태 미리보기를 유지한다.
표정은 대화 상태와 분리된다. 선택해도 진행 중인 turn·오디오·상태·모션
시계는 바뀌지 않으며 발화 음량에 따른 입 움직임도 계속된다. 기본으로
돌리면 이전 공식 얼굴의 상태별 도형이 그대로 나온다.

눈은 항상 원본 native 반지름 8의 원형이다. 눈꺼풀로 가려 표정을 만들며
눈을 타원으로 늘이거나 하이라이트·장식을 추가하지 않는다. 320×240 좌표를
동일한 0.75배와 세로 offset 30으로 투영한다. 색·눈 중심·기본 크기와
기존 blink/gaze/breath 샘플러를 유지한다.

## 공통 수치

아래 수치는 투영 전 원본 좌표 단위다. JS와 C는 같은
[표정 fixture](../tests/fixtures/nemossi-expressions.json)를 따른다.

| 표정 | 눈꺼풀 | 왼쪽/오른쪽 열림 cap | 눈 시선 offset | 무음 입 변형 |
| --- | --- | --- | --- | --- |
| 기본 | 기존 상태 매핑 | 12 / 12 | (0,0) | 변경 없음 |
| 기쁨 | HAPPY | 12 / 12 | (0,0) | 높이 +2 |
| 궁금함 | NEUTRAL | 12 / 9 | (+2,−1) | 너비 −12, 높이 +2 |
| 생각 중 | NEUTRAL | 8 / 10 | (−2,−2) | 너비 −18, 중심 x −4 |
| 놀람 | NEUTRAL | 12 / 12 | (0,−1) | 기본 입 열림 0.28 |
| 졸림 | SLEEPY | 10 / 10 | (0,+1) | 너비 −16, 중심 y +1 |
| 시무룩함 | SAD | 10 / 10 | (0,+1) | 너비 −12, 중심 y +3 |

먼저 원본 12단계 눈 열림 `baseStep`을 계산한 뒤 각 눈을
`round(baseStep*cap/12)`로 합성한다. 따라서 표정 중에도 깜박임이 적용된다.
sleep 상태에서는 어느 표정을 선택해도 두 눈의 열림은 0이다.

발화 음량 `v`는 기존 0..1 입력이며 발화 외에는 0이다. 표정의 무음 열림을
`r`이라 하면 `o=r+(1-r)*v`다. 너비는
`90−40o+width_delta*(1−o)`, 높이는
`8+50o+height_delta*(1−o)`다. 코드의 기본 폭 항은 이전 평가 순서인
`50+40*(1−o)`를 유지해 반올림 경계에서 기본 얼굴을 보존한다.
원본 범위인 너비 50..90, 높이 8..58을 유지하며
음량이 커지면 높아지고 좁아진다. 무음에서도 놀람은 파생 디자인의 열린
직사각형 입으로 돌아간다. 기본 표정의 무음은 원본 90×8이다. 원본 Port처럼
내부 x/y/w/h를 먼저 반올림하고 표정 중심 offset·호흡과 투영을 적용한다.

## 구현 범위

브라우저는 `Face.setExpression(id)`와 독립 select를 사용한다. C는 독립
`face_expression_t`와 `face_model_set_expression()`를 사용하며 기존 상태
event·deadline·demo·입 timeout과 분리된다. 잘못된 표정 입력은 모델을
변경하지 않는다. 실제 선택 UI와 비교 시트는 같은 Canvas 렌더러로 그린다.

서버·통화 protocol·마이크 권한·보드 I/O는 변경하지 않는다. 새 패키지·SDK를
설치하거나 보드를 flash하지 않는다. 실제 dot 통화와 실물 동작의 기존
미검증 경계는 그대로다. 세부 결과는 [검증 기록](verification.md)에 기록한다.
