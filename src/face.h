#pragma once

#include <Arduino.h>

// Face moods, cycled by a single tap on the face page. A fast spin plays a
// timed DIZZY reaction, then returns to the previous mood. Between blinks
// an eye winks on its own (sometimes both), except on sleepy, suspicious, and dizzy.
//
// To add a mood: append a MOOD_* id, bump FACE_MOOD_COUNT, and add a row to
// kMoods in face.cpp. A draw hook is only needed when the default round pupil,
// or no particles, is not enough.
#define MOOD_NORMAL      0
#define MOOD_HAPPY       1
#define MOOD_SURPRISED   2
#define MOOD_SLEEPY      3
#define MOOD_ANGRY       4
#define MOOD_SAD         5
#define MOOD_EXCITED     6
#define MOOD_LOVE        7
#define MOOD_SUSPICIOUS  8
#define MOOD_DIZZY       9
static const uint8_t FACE_MOOD_COUNT = 10;

extern uint8_t faceMood;

void faceInit();
// wifiConnected / wifiHasCreds only affect the small offline hint.
void drawFace(float tx, float ty, bool wifiConnected, bool wifiHasCreds);
void cancelShakeDizzy();
void enterDizzyFromShake();
void expireShakeDizzy();
void cycleFaceMood();
