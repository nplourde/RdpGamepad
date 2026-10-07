/*
 * Loads librdpgamepad-client.so the way FreeRDP does and plays the part of the
 * Windows RdpGamepad receiver: polls state every 16 ms and prints it.
 *
 * Usage: rdpgamepad-harness <path/to/librdpgamepad-client.so> [seconds] [--rumble] [plugin options...]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <winpr/stream.h>
#include <freerdp/dvc.h>
#include <freerdp/settings.h>

#include "../src/protocol.h"

static IWTSPlugin* g_plugin;
static IWTSListenerCallback* g_listener_cb;
static rdpSettings* g_settings;
static IWTSListener g_listener;

static UINT register_plugin(IDRDYNVC_ENTRY_POINTS* ep, const char* name, IWTSPlugin* plugin)
{
	(void)ep;
	printf("registered plugin '%s'\n", name);
	g_plugin = plugin;
	return CHANNEL_RC_OK;
}

static IWTSPlugin* get_plugin(IDRDYNVC_ENTRY_POINTS* ep, const char* name)
{
	(void)ep;
	(void)name;
	return NULL;
}

static rdpSettings* get_settings(IDRDYNVC_ENTRY_POINTS* ep)
{
	(void)ep;
	return g_settings;
}

static rdpContext* get_context(IDRDYNVC_ENTRY_POINTS* ep)
{
	(void)ep;
	return NULL;
}

static UINT create_listener(IWTSVirtualChannelManager* mgr, const char* name, ULONG flags,
                            IWTSListenerCallback* cb, IWTSListener** listener)
{
	(void)mgr;
	(void)flags;
	printf("listening on channel '%s'\n", name);
	if (strcmp(name, RDPGAMEPAD_DVC_CHANNEL_NAME) != 0)
		return ERROR_INVALID_PARAMETER;
	g_listener_cb = cb;
	*listener = &g_listener;
	return CHANNEL_RC_OK;
}

static UINT destroy_listener(IWTSVirtualChannelManager* mgr, IWTSListener* listener)
{
	(void)mgr;
	(void)listener;
	return CHANNEL_RC_OK;
}

static int g_responses;

static UINT channel_write(IWTSVirtualChannel* ch, ULONG size, const BYTE* buf, void* reserved)
{
	(void)ch;
	(void)reserved;
	RdpGamepadHeader h;
	if (size < sizeof(h))
		return ERROR_INVALID_DATA;
	memcpy(&h, buf, sizeof(h));
	if (h.mMessageType >= RDPGAMEPAD_MESSAGE_TYPE_COUNT ||
	    h.mMessageSize != kRdpGamepadMessageSizes[h.mMessageType] || h.mMessageSize != size)
	{
		printf("BAD RESPONSE type=%u size=%u len=%lu\n", h.mMessageType, h.mMessageSize,
		       (unsigned long)size);
		exit(1);
	}
	g_responses++;

	if (h.mMessageType == RDPGAMEPAD_GET_STATE_RESPONSE)
	{
		static XINPUT_GAMEPAD last;
		static int printed;
		RdpGamepadGetStateResponse r;
		memcpy(&r, buf, sizeof(r));
		const XINPUT_GAMEPAD* g = &r.mState.Gamepad;
		if (!printed++ || memcmp(g, &last, sizeof(last)) != 0)
			printf("state result=%u pkt=%u buttons=%04x LT=%3u RT=%3u L=(%6d,%6d) R=(%6d,%6d)\n",
			       r.mResult, r.mState.dwPacketNumber, g->wButtons, g->bLeftTrigger,
			       g->bRightTrigger, g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY);
		last = *g;
	}
	else if (h.mMessageType == RDPGAMEPAD_GET_CAPABILITIES_RESPONSE)
	{
		RdpGamepadGetCapabilitiesResponse r;
		memcpy(&r, buf, sizeof(r));
		printf("capabilities result=%u type=%u subtype=%u flags=%04x\n", r.mResult,
		       r.mCapabilities.Type, r.mCapabilities.SubType, r.mCapabilities.Flags);
	}
	else if (h.mMessageType == RDPGAMEPAD_SET_STATE_RESPONSE)
	{
		RdpGamepadSetStateResponse r;
		memcpy(&r, buf, sizeof(r));
		printf("set state result=%u\n", r.mResult);
	}
	return CHANNEL_RC_OK;
}

static UINT channel_close(IWTSVirtualChannel* ch)
{
	(void)ch;
	return CHANNEL_RC_OK;
}

static void deliver(IWTSVirtualChannelCallback* cb, const void* msg, size_t size)
{
	wStream* s = Stream_New(NULL, size);
	Stream_Write(s, msg, size);
	Stream_SealLength(s);
	Stream_SetPosition(s, 0);
	const UINT rc = cb->OnDataReceived(cb, s);
	Stream_Free(s, TRUE);
	if (rc != CHANNEL_RC_OK)
		printf("OnDataReceived returned %u\n", rc);
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <plugin.so> [seconds] [--rumble] [options...]\n", argv[0]);
		return 2;
	}

	const int seconds = argc > 2 ? atoi(argv[2]) : 3;
	int rumble = 0;
	const char* addin_argv[16] = { "rdpgamepad" };
	size_t addin_argc = 1;
	for (int i = 3; i < argc && addin_argc < 16; i++)
	{
		if (strcmp(argv[i], "--rumble") == 0)
			rumble = 1;
		else
			addin_argv[addin_argc++] = argv[i];
	}

	g_settings = freerdp_settings_new(0);
	if (!freerdp_dynamic_channel_collection_add(g_settings,
	                                            freerdp_addin_argv_new(addin_argc, addin_argv)))
		return 1;

	void* lib = dlopen(argv[1], RTLD_NOW);
	if (!lib)
	{
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	PDVC_PLUGIN_ENTRY entry = (PDVC_PLUGIN_ENTRY)dlsym(lib, "DVCPluginEntry");
	if (!entry)
	{
		fprintf(stderr, "DVCPluginEntry not exported\n");
		return 1;
	}

	IDRDYNVC_ENTRY_POINTS ep = { register_plugin, get_plugin, NULL, get_settings, get_context };
	if (entry(&ep) != CHANNEL_RC_OK || !g_plugin)
	{
		fprintf(stderr, "DVCPluginEntry failed\n");
		return 1;
	}

	IWTSVirtualChannelManager mgr = { 0 };
	mgr.CreateListener = create_listener;
	mgr.DestroyListener = destroy_listener;
	if (g_plugin->Initialize(g_plugin, &mgr) != CHANNEL_RC_OK || !g_listener_cb)
		return 1;

	IWTSVirtualChannel channel = { channel_write, channel_close };
	IWTSVirtualChannelCallback* cb = NULL;
	BOOL accept = TRUE; /* drdynvc defaults to accepting */
	if (g_listener_cb->OnNewChannelConnection(g_listener_cb, &channel, NULL, &accept, &cb) !=
	        CHANNEL_RC_OK ||
	    !accept || !cb)
		return 1;
	if (cb->OnOpen)
		(void)cb->OnOpen(cb);

	RdpGamepadHeader get_state = { RDPGAMEPAD_GET_STATE_REQUEST, sizeof(RdpGamepadHeader), 0 };
	RdpGamepadGetCapabilitiesRequest get_caps = {
		{ RDPGAMEPAD_GET_CAPABILITIES_REQUEST, sizeof(get_caps), 0 }, 0
	};
	RdpGamepadHeader get_state_user1 = { RDPGAMEPAD_GET_STATE_REQUEST, sizeof(RdpGamepadHeader), 1 };
	RdpGamepadHeader bogus = { RDPGAMEPAD_GET_STATE_REQUEST, 99, 0 };

	deliver(cb, &get_caps, sizeof(get_caps));
	deliver(cb, &get_state_user1, sizeof(get_state_user1));
	deliver(cb, &bogus, sizeof(bogus));

	if (rumble)
	{
		RdpGamepadSetStateRequest on = { { RDPGAMEPAD_SET_STATE_REQUEST, sizeof(on), 0 },
			                             { 0x8000, 0x8000 } };
		RdpGamepadSetStateRequest off = { { RDPGAMEPAD_SET_STATE_REQUEST, sizeof(off), 0 },
			                              { 0, 0 } };
		deliver(cb, &on, sizeof(on));
		sleep_ms(300);
		deliver(cb, &off, sizeof(off));
	}

	printf("polling for %d s, move the sticks / press buttons...\n", seconds);
	for (int i = 0; i < seconds * 1000 / 16; i++)
	{
		deliver(cb, &get_state, sizeof(get_state));
		sleep_ms(16);
	}

	(void)cb->OnClose(cb);
	(void)g_plugin->Terminated(g_plugin);
	freerdp_settings_free(g_settings);
	printf("%d responses, done\n", g_responses);
	return 0;
}
