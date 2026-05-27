#ifndef __LSWTCS_KEYCODES_H__
#define __LSWTCS_KEYCODES_H__

/* Subset of Android KeyEvent codes used by libTTapp's input handler.
 * Only the ones we actually feed to nativeOnKeyDown / nativeOnKeyUp /
 * NuInputDevicePS::GetGamePadButtonIndex are listed. */
#define AKEYCODE_HOME           3
#define AKEYCODE_DPAD_UP        19
#define AKEYCODE_DPAD_DOWN      20
#define AKEYCODE_DPAD_LEFT      21
#define AKEYCODE_DPAD_RIGHT     22
#define AKEYCODE_BUTTON_A       96
#define AKEYCODE_BUTTON_B       97
#define AKEYCODE_BUTTON_X       99
#define AKEYCODE_BUTTON_Y       100
#define AKEYCODE_BUTTON_L1      102
#define AKEYCODE_BUTTON_R1      103
#define AKEYCODE_BUTTON_L2      104
#define AKEYCODE_BUTTON_R2      105
#define AKEYCODE_BUTTON_THUMBL  106
#define AKEYCODE_BUTTON_THUMBR  107
#define AKEYCODE_BUTTON_START   108
#define AKEYCODE_BUTTON_SELECT  109

/* Internal "gamepad slot" indices the game uses after key→button mapping.
 * Mirrors the Vita port's enum so patch.c stays semantically identical. */
#define GAMEPAD_START   1
#define GAMEPAD_ACTION  2
#define GAMEPAD_JUMP    3
#define GAMEPAD_SPECIAL 4
#define GAMEPAD_TAG     5
#define GAMEPAD_L1      6
#define GAMEPAD_R1      7
#define GAMEPAD_L2      8
#define GAMEPAD_R2      9
#define GAMEPAD_L3      10
#define GAMEPAD_R3      11

#endif
