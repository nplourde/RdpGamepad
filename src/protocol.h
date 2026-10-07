/*
 * Wire format of the Microsoft RdpGamepad dynamic virtual channel.
 *
 * Mirrors RdpGamepadPlugin/RdpGamepadProtocol.h from
 * https://github.com/microsoft/RdpGamepad (MIT). The XInput structures keep
 * their natural Windows alignment; the protocol messages are packed.
 */
#ifndef RDPGAMEPAD_PROTOCOL_H
#define RDPGAMEPAD_PROTOCOL_H

#include <stdint.h>

#define RDPGAMEPAD_DVC_CHANNEL_NAME "Microsoft.RdpGamepad"

/* Win32 error codes returned in mResult */
#define RDPGAMEPAD_ERROR_SUCCESS 0u
#define RDPGAMEPAD_ERROR_DEVICE_NOT_CONNECTED 1167u

/* XINPUT_GAMEPAD.wButtons */
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_GUIDE 0x0400 /* undocumented, forwarded by ViGEm */
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000

#define XINPUT_DEVTYPE_GAMEPAD 0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD 0x01
#define XINPUT_CAPS_FFB_SUPPORTED 0x0001

typedef struct
{
	uint16_t wButtons;
	uint8_t bLeftTrigger;
	uint8_t bRightTrigger;
	int16_t sThumbLX;
	int16_t sThumbLY;
	int16_t sThumbRX;
	int16_t sThumbRY;
} XINPUT_GAMEPAD;

typedef struct
{
	uint32_t dwPacketNumber;
	XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

typedef struct
{
	uint16_t wLeftMotorSpeed;
	uint16_t wRightMotorSpeed;
} XINPUT_VIBRATION;

typedef struct
{
	uint8_t Type;
	uint8_t SubType;
	uint16_t Flags;
	XINPUT_GAMEPAD Gamepad;
	XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES;

typedef enum
{
	RDPGAMEPAD_HEARTBEAT,
	RDPGAMEPAD_GET_STATE_REQUEST,
	RDPGAMEPAD_POLL_STATE_REQUEST,
	RDPGAMEPAD_SET_STATE_REQUEST,
	RDPGAMEPAD_GET_CAPABILITIES_REQUEST,
	RDPGAMEPAD_GET_STATE_RESPONSE,
	RDPGAMEPAD_SET_STATE_RESPONSE,
	RDPGAMEPAD_GET_CAPABILITIES_RESPONSE,
	RDPGAMEPAD_MESSAGE_TYPE_COUNT
} RdpGamepadMessageType;

#pragma pack(push, 1)

typedef struct
{
	uint16_t mMessageType;
	uint16_t mMessageSize;
	uint32_t mUserIndex;
} RdpGamepadHeader;

typedef struct
{
	RdpGamepadHeader header;
	XINPUT_VIBRATION mVibration;
} RdpGamepadSetStateRequest;

typedef struct
{
	RdpGamepadHeader header;
	uint32_t mFlags;
} RdpGamepadGetCapabilitiesRequest;

typedef struct
{
	RdpGamepadHeader header;
	uint32_t mResult;
	XINPUT_STATE mState;
} RdpGamepadGetStateResponse;

typedef struct
{
	RdpGamepadHeader header;
	uint32_t mResult;
} RdpGamepadSetStateResponse;

typedef struct
{
	RdpGamepadHeader header;
	uint32_t mResult;
	XINPUT_CAPABILITIES mCapabilities;
} RdpGamepadGetCapabilitiesResponse;

#pragma pack(pop)

_Static_assert(sizeof(XINPUT_STATE) == 16, "XINPUT_STATE size");
_Static_assert(sizeof(XINPUT_CAPABILITIES) == 20, "XINPUT_CAPABILITIES size");
_Static_assert(sizeof(RdpGamepadHeader) == 8, "header size");
_Static_assert(sizeof(RdpGamepadSetStateRequest) == 12, "SetStateRequest size");
_Static_assert(sizeof(RdpGamepadGetCapabilitiesRequest) == 12, "GetCapabilitiesRequest size");
_Static_assert(sizeof(RdpGamepadGetStateResponse) == 28, "GetStateResponse size");
_Static_assert(sizeof(RdpGamepadSetStateResponse) == 12, "SetStateResponse size");
_Static_assert(sizeof(RdpGamepadGetCapabilitiesResponse) == 32, "GetCapabilitiesResponse size");

/* Expected mMessageSize for each message type, indexed by RdpGamepadMessageType. */
static const uint16_t kRdpGamepadMessageSizes[RDPGAMEPAD_MESSAGE_TYPE_COUNT] = {
	sizeof(RdpGamepadHeader),                  /* Heartbeat */
	sizeof(RdpGamepadHeader),                  /* GetStateRequest */
	sizeof(RdpGamepadHeader),                  /* PollStateRequest */
	sizeof(RdpGamepadSetStateRequest),         /* SetStateRequest */
	sizeof(RdpGamepadGetCapabilitiesRequest),  /* GetCapabilitiesRequest */
	sizeof(RdpGamepadGetStateResponse),        /* GetStateResponse */
	sizeof(RdpGamepadSetStateResponse),        /* SetStateResponse */
	sizeof(RdpGamepadGetCapabilitiesResponse), /* GetCapabilitiesResponse */
};

#endif /* RDPGAMEPAD_PROTOCOL_H */
