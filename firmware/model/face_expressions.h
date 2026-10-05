/* SPDX-License-Identifier: Apache-2.0
 * Nemossi expression parameters derived from Stack-chan SimpleFace geometry.
 * Modified: independent expression selection; no lifecycle or input events.
 */
#ifndef NEMOSSI_FACE_EXPRESSIONS_H
#define NEMOSSI_FACE_EXPRESSIONS_H

#include <stdint.h>

typedef enum {
    FACE_EXPRESSION_DEFAULT = 0,
    FACE_EXPRESSION_JOY,
    FACE_EXPRESSION_CURIOUS,
    FACE_EXPRESSION_PONDERING,
    FACE_EXPRESSION_SURPRISED,
    FACE_EXPRESSION_DROWSY,
    FACE_EXPRESSION_DOWNCAST,
    FACE_EXPRESSION_COUNT
} face_expression_t;

typedef enum {
    FACE_EXPRESSION_EMOTION_AUTO = 0,
    FACE_EXPRESSION_EMOTION_HAPPY,
    FACE_EXPRESSION_EMOTION_NEUTRAL,
    FACE_EXPRESSION_EMOTION_SLEEPY,
    FACE_EXPRESSION_EMOTION_SAD
} face_expression_emotion_t;

typedef struct {
    const char *id;
    face_expression_emotion_t emotion;
    uint8_t left_open_cap;
    uint8_t right_open_cap;
    int8_t iris_dx;
    int8_t iris_dy;
    uint16_t mouth_rest_open_milli;
    int8_t mouth_width_delta;
    int8_t mouth_height_delta;
    int8_t mouth_dx;
    int8_t mouth_dy;
} face_expression_design_t;

#ifdef __cplusplus
extern "C" {
#endif
/* Row order and native 320x240 parameters match nemossi-expressions.json. */
extern const face_expression_design_t FACE_EXPRESSION_DESIGNS[FACE_EXPRESSION_COUNT];
#ifdef __cplusplus
}
#endif

#endif
