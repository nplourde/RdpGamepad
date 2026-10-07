/*
 * FreeRDP dynamic virtual channel client for Microsoft RdpGamepad.
 *
 * The RdpGamepad receiver on the remote Windows host opens the
 * "Microsoft.RdpGamepad" channel and polls us for controller state every
 * 16 ms; we answer with the state of a local evdev gamepad and forward
 * rumble requests back to it.
 *
 * Usage: xfreerdp /dvc:rdpgamepad[,device:<path>][,match:<name>][,layout:xbox|positional][,grab]
 */
#include <stdlib.h>
#include <string.h>

#include <winpr/stream.h>
#include <freerdp/client/channels.h>
#include <freerdp/settings.h>

#include "gamepad.h"
#include "protocol.h"

#define TAG "com.freerdp.channels.rdpgamepad.client"
#define ADDIN_NAME "rdpgamepad"

typedef struct
{
	GENERIC_DYNVC_PLUGIN base;
	Gamepad pad;
} RDPGAMEPAD_PLUGIN;

static void make_header(RdpGamepadHeader* h, RdpGamepadMessageType type, uint32_t user)
{
	h->mMessageType = (uint16_t)type;
	h->mMessageSize = kRdpGamepadMessageSizes[type];
	h->mUserIndex = user;
}

static UINT send_message(GENERIC_CHANNEL_CALLBACK* callback, const void* msg)
{
	const RdpGamepadHeader* h = msg;
	return callback->channel->Write(callback->channel, h->mMessageSize, (const BYTE*)msg, NULL);
}

static UINT send_state(GENERIC_CHANNEL_CALLBACK* callback, Gamepad* pad, uint32_t user)
{
	RdpGamepadGetStateResponse rsp = { 0 };
	make_header(&rsp.header, RDPGAMEPAD_GET_STATE_RESPONSE, user);

	if (user == 0 && gamepad_get_state(pad, &rsp.mState))
		rsp.mResult = RDPGAMEPAD_ERROR_SUCCESS;
	else
		rsp.mResult = RDPGAMEPAD_ERROR_DEVICE_NOT_CONNECTED;
	return send_message(callback, &rsp);
}

static UINT send_set_state(GENERIC_CHANNEL_CALLBACK* callback, Gamepad* pad,
                           const RdpGamepadSetStateRequest* req)
{
	RdpGamepadSetStateResponse rsp = { 0 };
	make_header(&rsp.header, RDPGAMEPAD_SET_STATE_RESPONSE, req->header.mUserIndex);

	/* A failed rumble is not worth tearing down the controller over; only report absence. */
	if (req->header.mUserIndex == 0 && pad->fd >= 0)
	{
		(void)gamepad_set_rumble(pad, req->mVibration.wLeftMotorSpeed,
		                         req->mVibration.wRightMotorSpeed);
		rsp.mResult = RDPGAMEPAD_ERROR_SUCCESS;
	}
	else
		rsp.mResult = RDPGAMEPAD_ERROR_DEVICE_NOT_CONNECTED;
	return send_message(callback, &rsp);
}

static UINT send_capabilities(GENERIC_CHANNEL_CALLBACK* callback, Gamepad* pad, uint32_t user)
{
	RdpGamepadGetCapabilitiesResponse rsp = { 0 };
	XINPUT_STATE state;
	make_header(&rsp.header, RDPGAMEPAD_GET_CAPABILITIES_RESPONSE, user);

	if (user == 0 && gamepad_get_state(pad, &state))
	{
		XINPUT_CAPABILITIES* caps = &rsp.mCapabilities;
		rsp.mResult = RDPGAMEPAD_ERROR_SUCCESS;
		caps->Type = XINPUT_DEVTYPE_GAMEPAD;
		caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
		caps->Flags = pad->has_rumble ? XINPUT_CAPS_FFB_SUPPORTED : 0;
		/* Same resolution masks a wired Xbox 360 controller reports. */
		caps->Gamepad.wButtons = 0xF3FF;
		caps->Gamepad.bLeftTrigger = 0xFF;
		caps->Gamepad.bRightTrigger = 0xFF;
		caps->Gamepad.sThumbLX = (int16_t)0xFFC0;
		caps->Gamepad.sThumbLY = (int16_t)0xFFC0;
		caps->Gamepad.sThumbRX = (int16_t)0xFFC0;
		caps->Gamepad.sThumbRY = (int16_t)0xFFC0;
		caps->Vibration.wLeftMotorSpeed = pad->has_rumble ? 0xFF00 : 0;
		caps->Vibration.wRightMotorSpeed = pad->has_rumble ? 0xFF00 : 0;
	}
	else
		rsp.mResult = RDPGAMEPAD_ERROR_DEVICE_NOT_CONNECTED;
	return send_message(callback, &rsp);
}

static UINT rdpgamepad_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	RDPGAMEPAD_PLUGIN* plugin = (RDPGAMEPAD_PLUGIN*)callback->plugin;
	const size_t size = Stream_GetRemainingLength(data);
	union
	{
		RdpGamepadHeader header;
		RdpGamepadSetStateRequest set_state;
		RdpGamepadGetCapabilitiesRequest get_caps;
		BYTE raw[64];
	} msg;

	if (size < sizeof(RdpGamepadHeader) || size > sizeof(msg.raw))
		return ERROR_INVALID_DATA;

	/* copy out: the stream buffer has no alignment guarantee */
	memcpy(msg.raw, Stream_ConstPointer(data), size);

	if (msg.header.mMessageType >= RDPGAMEPAD_MESSAGE_TYPE_COUNT ||
	    msg.header.mMessageSize != kRdpGamepadMessageSizes[msg.header.mMessageType] ||
	    msg.header.mMessageSize != size)
	{
		WLog_Print(plugin->base.log, WLOG_WARN, "invalid message type %u size %u (%zu bytes)",
		           msg.header.mMessageType, msg.header.mMessageSize, size);
		return ERROR_INVALID_DATA;
	}

	switch (msg.header.mMessageType)
	{
		case RDPGAMEPAD_GET_STATE_REQUEST:
		/* The Windows plugin also streams state at 30 Hz for 2 s after a poll request; the
		 * receiver never sends one, so a single reply is enough. */
		case RDPGAMEPAD_POLL_STATE_REQUEST:
			return send_state(callback, &plugin->pad, msg.header.mUserIndex);
		case RDPGAMEPAD_SET_STATE_REQUEST:
			return send_set_state(callback, &plugin->pad, &msg.set_state);
		case RDPGAMEPAD_GET_CAPABILITIES_REQUEST:
			return send_capabilities(callback, &plugin->pad, msg.header.mUserIndex);
		default:
			return CHANNEL_RC_OK;
	}
}

static UINT rdpgamepad_on_open(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	RDPGAMEPAD_PLUGIN* plugin = (RDPGAMEPAD_PLUGIN*)callback->plugin;

	WLog_Print(plugin->base.log, WLOG_INFO, "remote RdpGamepad receiver connected");
	return CHANNEL_RC_OK;
}

static UINT rdpgamepad_on_close(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	RDPGAMEPAD_PLUGIN* plugin = (RDPGAMEPAD_PLUGIN*)callback->plugin;

	WLog_Print(plugin->base.log, WLOG_INFO, "remote RdpGamepad receiver disconnected");
	/* stops any rumble left running and releases a grab */
	gamepad_close(&plugin->pad);
	free(callback);
	return CHANNEL_RC_OK;
}

static UINT rdpgamepad_init(GENERIC_DYNVC_PLUGIN* base, rdpContext* context, rdpSettings* settings)
{
	RDPGAMEPAD_PLUGIN* plugin = (RDPGAMEPAD_PLUGIN*)base;
	WINPR_UNUSED(context);

	gamepad_init(&plugin->pad, base->log);

	const ADDIN_ARGV* args = freerdp_dynamic_channel_collection_find(settings, ADDIN_NAME);
	for (int i = 1; args && i < args->argc; i++)
	{
		const char* arg = args->argv[i];
		if (strncmp(arg, "device:", 7) == 0)
			plugin->pad.device = _strdup(arg + 7);
		else if (strncmp(arg, "match:", 6) == 0)
			plugin->pad.match = _strdup(arg + 6);
		else if (strcmp(arg, "layout:xbox") == 0)
			plugin->pad.layout = GAMEPAD_LAYOUT_XBOX;
		else if (strcmp(arg, "layout:positional") == 0)
			plugin->pad.layout = GAMEPAD_LAYOUT_POSITIONAL;
		else if (strcmp(arg, "grab") == 0)
			plugin->pad.grab = true;
		else
		{
			WLog_Print(base->log, WLOG_ERROR,
			           "unknown option '%s' (expected device:<path>, match:<name>, "
			           "layout:xbox|positional, grab)",
			           arg);
			return ERROR_INVALID_PARAMETER;
		}
	}
	return CHANNEL_RC_OK;
}

static void rdpgamepad_terminate(GENERIC_DYNVC_PLUGIN* base)
{
	RDPGAMEPAD_PLUGIN* plugin = (RDPGAMEPAD_PLUGIN*)base;
	gamepad_free(&plugin->pad);
}

static const IWTSVirtualChannelCallback rdpgamepad_callbacks = { rdpgamepad_on_data_received,
	                                                             rdpgamepad_on_open,
	                                                             rdpgamepad_on_close, NULL };

/* The only exported symbol; FreeRDP resolves it with dlsym() after loading the add-in. */
FREERDP_ENTRY_POINT(__attribute__((visibility("default"))) UINT VCAPITYPE DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))
{
	return freerdp_generic_DVCPluginEntry(pEntryPoints, TAG, RDPGAMEPAD_DVC_CHANNEL_NAME,
	                                      sizeof(RDPGAMEPAD_PLUGIN),
	                                      sizeof(GENERIC_CHANNEL_CALLBACK), &rdpgamepad_callbacks,
	                                      rdpgamepad_init, rdpgamepad_terminate);
}
