/*
 * Linux evdev gamepad reader producing XInput-shaped state.
 */
#ifndef RDPGAMEPAD_GAMEPAD_H
#define RDPGAMEPAD_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <linux/input.h>
#include <winpr/wlog.h>

#include "protocol.h"

typedef enum
{
	GAMEPAD_LAYOUT_AUTO,
	GAMEPAD_LAYOUT_XBOX,      /* BTN_X (0x133) is the Xbox X button, as reported by xpad */
	GAMEPAD_LAYOUT_POSITIONAL /* BTN_NORTH/BTN_WEST are the top/left buttons (kernel spec) */
} GamepadLayout;

typedef struct
{
	/* configuration */
	char* device;    /* explicit evdev path, or NULL to auto-detect */
	char* match;     /* substring of the device name to look for when auto-detecting */
	GamepadLayout layout;
	bool grab;
	wLog* log;

	/* runtime */
	int fd;
	char path[256];
	char name[256];
	struct timespec last_scan;
	bool xbox_layout;
	int ff_id;
	bool has_rumble;
	int axis_lx, axis_ly, axis_rx, axis_ry, axis_lt, axis_rt; /* ABS_* code or -1 */
	XINPUT_GAMEPAD last;
	uint32_t packet;
} Gamepad;

void gamepad_init(Gamepad* pad, wLog* log);
void gamepad_free(Gamepad* pad);

/* Reads the current controller state, opening a device if needed.
 * Returns false when no controller is connected. */
bool gamepad_get_state(Gamepad* pad, XINPUT_STATE* state);

/* Sets rumble motor speeds (0..65535). Returns false when unsupported. */
bool gamepad_set_rumble(Gamepad* pad, uint16_t strong, uint16_t weak);

/* Stops rumble and closes the device; it is reopened on the next state request. */
void gamepad_close(Gamepad* pad);

#endif /* RDPGAMEPAD_GAMEPAD_H */
