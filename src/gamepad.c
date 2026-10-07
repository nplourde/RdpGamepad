/*
 * Linux evdev gamepad reader producing XInput-shaped state.
 *
 * Instead of tracking the event stream, the current state is queried with
 * EVIOCGKEY/EVIOCGABS on every request. The receiver polls at ~60 Hz, so this
 * is cheap and never drifts (no SYN_DROPPED handling needed).
 */
#include "gamepad.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SCAN_INTERVAL_MS 1000

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static bool test_bit(const unsigned long* bits, unsigned bit)
{
	return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

static long elapsed_ms(const struct timespec* since, const struct timespec* now)
{
	return (now->tv_sec - since->tv_sec) * 1000L + (now->tv_nsec - since->tv_nsec) / 1000000L;
}

void gamepad_init(Gamepad* pad, wLog* log)
{
	memset(pad, 0, sizeof(*pad));
	pad->fd = -1;
	pad->ff_id = -1;
	pad->log = log;
	pad->layout = GAMEPAD_LAYOUT_AUTO;
	/* make the first state request scan immediately */
	pad->last_scan.tv_sec = -SCAN_INTERVAL_MS;
}

void gamepad_free(Gamepad* pad)
{
	gamepad_close(pad);
	free(pad->device);
	free(pad->match);
	pad->device = NULL;
	pad->match = NULL;
}

void gamepad_close(Gamepad* pad)
{
	if (pad->fd < 0)
		return;

	if (pad->ff_id >= 0)
	{
		(void)ioctl(pad->fd, EVIOCRMFF, pad->ff_id);
		pad->ff_id = -1;
	}
	close(pad->fd);
	pad->fd = -1;
	WLog_Print(pad->log, WLOG_INFO, "released gamepad %s (%s)", pad->path, pad->name);
}

/* Name of the kernel driver bound to /dev/input/eventN, e.g. "xpad" or "playstation". */
static void read_driver_name(const char* event_path, char* out, size_t out_size)
{
	char real[PATH_MAX];
	char sys[PATH_MAX + 64];
	char link[PATH_MAX];

	out[0] = '\0';
	if (!realpath(event_path, real))
		return;

	const char* node = strrchr(real, '/');
	node = node ? node + 1 : real;
	(void)snprintf(sys, sizeof(sys), "/sys/class/input/%s/device/device/driver", node);

	const ssize_t len = readlink(sys, link, sizeof(link) - 1);
	if (len <= 0)
		return;
	link[len] = '\0';
	(void)snprintf(out, out_size, "%s", basename(link));
}

static bool is_xbox_family(const char* driver, const struct input_id* id)
{
	return id->vendor == 0x045e || strstr(driver, "xpad") || strstr(driver, "xone") ||
	       strstr(driver, "microsoft");
}

static int first_abs(const unsigned long* abs_bits, int a, int b)
{
	if (a >= 0 && test_bit(abs_bits, (unsigned)a))
		return a;
	if (b >= 0 && test_bit(abs_bits, (unsigned)b))
		return b;
	return -1;
}

/* Opens path if it looks like a gamepad (has a south face button and a left stick). */
static bool try_open(Gamepad* pad, const char* path, bool quiet)
{
	unsigned long key_bits[NLONGS(KEY_CNT)] = { 0 };
	unsigned long abs_bits[NLONGS(ABS_CNT)] = { 0 };
	unsigned long ff_bits[NLONGS(FF_CNT)] = { 0 };
	struct input_id id = { 0 };
	char driver[64];

	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		if (!quiet)
			WLog_Print(pad->log, WLOG_WARN, "cannot open %s: %s", path, strerror(errno));
		return false;
	}

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0 ||
	    !test_bit(key_bits, BTN_SOUTH) || !test_bit(abs_bits, ABS_X) ||
	    !test_bit(abs_bits, ABS_Y))
	{
		if (!quiet)
			WLog_Print(pad->log, WLOG_WARN, "%s does not look like a gamepad", path);
		close(fd);
		return false;
	}

	char name[sizeof(pad->name)] = "unknown";
	(void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	if (pad->match && !strcasestr(name, pad->match))
	{
		close(fd);
		return false;
	}

	(void)ioctl(fd, EVIOCGID, &id);
	(void)ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits);
	read_driver_name(path, driver, sizeof(driver));

	if (pad->grab && ioctl(fd, EVIOCGRAB, (void*)1) < 0)
		WLog_Print(pad->log, WLOG_WARN, "cannot grab %s: %s", path, strerror(errno));

	pad->fd = fd;
	pad->ff_id = -1;
	pad->has_rumble = test_bit(ff_bits, FF_RUMBLE) && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR;
	(void)snprintf(pad->path, sizeof(pad->path), "%s", path);
	(void)snprintf(pad->name, sizeof(pad->name), "%s", name);

	switch (pad->layout)
	{
		case GAMEPAD_LAYOUT_XBOX:
			pad->xbox_layout = true;
			break;
		case GAMEPAD_LAYOUT_POSITIONAL:
			pad->xbox_layout = false;
			break;
		default:
			pad->xbox_layout = is_xbox_family(driver, &id);
			break;
	}

	/* Generic HID Xbox pads put the right stick on Z/RZ and the triggers on BRAKE/GAS. */
	const bool sim_triggers = !test_bit(abs_bits, ABS_RX) && test_bit(abs_bits, ABS_BRAKE) &&
	                          test_bit(abs_bits, ABS_GAS);
	pad->axis_lx = ABS_X;
	pad->axis_ly = ABS_Y;
	if (sim_triggers)
	{
		pad->axis_rx = first_abs(abs_bits, ABS_Z, -1);
		pad->axis_ry = first_abs(abs_bits, ABS_RZ, -1);
		pad->axis_lt = ABS_BRAKE;
		pad->axis_rt = ABS_GAS;
	}
	else
	{
		pad->axis_rx = first_abs(abs_bits, ABS_RX, -1);
		pad->axis_ry = first_abs(abs_bits, ABS_RY, -1);
		pad->axis_lt = first_abs(abs_bits, ABS_Z, ABS_BRAKE);
		pad->axis_rt = first_abs(abs_bits, ABS_RZ, ABS_GAS);
	}

	WLog_Print(pad->log, WLOG_INFO,
	           "using gamepad %s (%s, %04x:%04x, driver %s, %s layout, rumble %s)", path, name,
	           id.vendor, id.product, driver[0] ? driver : "?",
	           pad->xbox_layout ? "xbox" : "positional", pad->has_rumble ? "yes" : "no");
	return true;
}

static int compare_event_nodes(const struct dirent** a, const struct dirent** b)
{
	/* event2 before event10 */
	return atoi((*a)->d_name + 5) - atoi((*b)->d_name + 5);
}

static int is_event_node(const struct dirent* e)
{
	return strncmp(e->d_name, "event", 5) == 0;
}

static void scan(Gamepad* pad)
{
	if (pad->device)
	{
		(void)try_open(pad, pad->device, false);
		return;
	}

	struct dirent** list = NULL;
	const int n = scandir("/dev/input", &list, is_event_node, compare_event_nodes);
	for (int i = 0; i < n; i++)
	{
		char path[288];
		(void)snprintf(path, sizeof(path), "/dev/input/%s", list[i]->d_name);
		if (pad->fd < 0)
			(void)try_open(pad, path, true);
		free(list[i]);
	}
	free(list);
}

static bool ensure_open(Gamepad* pad)
{
	if (pad->fd >= 0)
		return true;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (elapsed_ms(&pad->last_scan, &now) < SCAN_INTERVAL_MS)
		return false;
	pad->last_scan = now;

	scan(pad);
	return pad->fd >= 0;
}

/* Discards queued events; we only care about whether the device is still there. */
static bool drain(Gamepad* pad)
{
	struct input_event events[64];
	for (;;)
	{
		const ssize_t r = read(pad->fd, events, sizeof(events));
		if (r > 0)
			continue;
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && errno == EAGAIN)
			return true;
		return false; /* ENODEV after unplug, or EOF */
	}
}

static bool read_abs(Gamepad* pad, int code, struct input_absinfo* info)
{
	return code >= 0 && ioctl(pad->fd, EVIOCGABS(code), info) == 0;
}

static int16_t scale_stick(Gamepad* pad, int code, bool invert)
{
	struct input_absinfo a;
	if (!read_abs(pad, code, &a) || a.maximum <= a.minimum)
		return 0;

	const long long range = (long long)a.maximum - a.minimum;
	long long v = ((long long)a.value - a.minimum) * 65535 / range - 32768;
	if (v < -32768)
		v = -32768;
	if (v > 32767)
		v = 32767;
	/* evdev Y grows downwards, XInput Y grows upwards */
	if (invert)
		v = -v - 1;
	return (int16_t)v;
}

static uint8_t scale_trigger(Gamepad* pad, int code)
{
	struct input_absinfo a;
	if (!read_abs(pad, code, &a) || a.maximum <= a.minimum)
		return 0;

	long long v = ((long long)a.value - a.minimum) * 255 / ((long long)a.maximum - a.minimum);
	if (v < 0)
		v = 0;
	if (v > 255)
		v = 255;
	return (uint8_t)v;
}

bool gamepad_get_state(Gamepad* pad, XINPUT_STATE* state)
{
	unsigned long keys[NLONGS(KEY_CNT)] = { 0 };
	struct input_absinfo hat;
	XINPUT_GAMEPAD g = { 0 };

	memset(state, 0, sizeof(*state));
	if (!ensure_open(pad))
		return false;

	if (!drain(pad) || ioctl(pad->fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
	{
		gamepad_close(pad);
		return false;
	}

	static const struct
	{
		unsigned code;
		uint16_t button;
	} common[] = {
		{ BTN_SOUTH, XINPUT_GAMEPAD_A },
		{ BTN_EAST, XINPUT_GAMEPAD_B },
		{ BTN_TL, XINPUT_GAMEPAD_LEFT_SHOULDER },
		{ BTN_TR, XINPUT_GAMEPAD_RIGHT_SHOULDER },
		{ BTN_SELECT, XINPUT_GAMEPAD_BACK },
		{ BTN_START, XINPUT_GAMEPAD_START },
		{ BTN_MODE, XINPUT_GAMEPAD_GUIDE },
		{ BTN_THUMBL, XINPUT_GAMEPAD_LEFT_THUMB },
		{ BTN_THUMBR, XINPUT_GAMEPAD_RIGHT_THUMB },
		{ BTN_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
		{ BTN_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
		{ BTN_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
		{ BTN_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
	};
	for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
	{
		if (test_bit(keys, common[i].code))
			g.wButtons |= common[i].button;
	}

	/* 0x133 is both BTN_NORTH and BTN_X, 0x134 both BTN_WEST and BTN_Y. */
	if (test_bit(keys, BTN_NORTH))
		g.wButtons |= pad->xbox_layout ? XINPUT_GAMEPAD_X : XINPUT_GAMEPAD_Y;
	if (test_bit(keys, BTN_WEST))
		g.wButtons |= pad->xbox_layout ? XINPUT_GAMEPAD_Y : XINPUT_GAMEPAD_X;

	if (read_abs(pad, ABS_HAT0X, &hat))
	{
		if (hat.value < 0)
			g.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
		else if (hat.value > 0)
			g.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	}
	if (read_abs(pad, ABS_HAT0Y, &hat))
	{
		if (hat.value < 0)
			g.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
		else if (hat.value > 0)
			g.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	}

	g.sThumbLX = scale_stick(pad, pad->axis_lx, false);
	g.sThumbLY = scale_stick(pad, pad->axis_ly, true);
	g.sThumbRX = scale_stick(pad, pad->axis_rx, false);
	g.sThumbRY = scale_stick(pad, pad->axis_ry, true);

	/* Pads without analog triggers (e.g. Switch Pro) only have BTN_TL2/BTN_TR2. */
	if (pad->axis_lt >= 0)
		g.bLeftTrigger = scale_trigger(pad, pad->axis_lt);
	else if (test_bit(keys, BTN_TL2))
		g.bLeftTrigger = 255;
	if (pad->axis_rt >= 0)
		g.bRightTrigger = scale_trigger(pad, pad->axis_rt);
	else if (test_bit(keys, BTN_TR2))
		g.bRightTrigger = 255;

	if (memcmp(&g, &pad->last, sizeof(g)) != 0)
	{
		pad->last = g;
		pad->packet++;
	}
	state->dwPacketNumber = pad->packet;
	state->Gamepad = g;
	return true;
}

bool gamepad_set_rumble(Gamepad* pad, uint16_t strong, uint16_t weak)
{
	if (pad->fd < 0 || !pad->has_rumble)
		return false;

	struct input_event play = { .type = EV_FF };

	if (strong == 0 && weak == 0)
	{
		if (pad->ff_id < 0)
			return true;
		play.code = (uint16_t)pad->ff_id;
		play.value = 0;
		return write(pad->fd, &play, sizeof(play)) == (ssize_t)sizeof(play);
	}

	/* Re-uploading an effect with the same id updates it in place. replay.length 0 = until stopped. */
	struct ff_effect effect = { .type = FF_RUMBLE, .id = (int16_t)pad->ff_id };
	effect.u.rumble.strong_magnitude = strong;
	effect.u.rumble.weak_magnitude = weak;
	if (ioctl(pad->fd, EVIOCSFF, &effect) < 0)
	{
		WLog_Print(pad->log, WLOG_WARN, "rumble upload failed: %s", strerror(errno));
		return false;
	}
	pad->ff_id = effect.id;

	play.code = (uint16_t)pad->ff_id;
	play.value = 1;
	return write(pad->fd, &play, sizeof(play)) == (ssize_t)sizeof(play);
}
