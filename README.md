# rdpgamepad for FreeRDP

A FreeRDP 3 client add-in that implements the client side of
[microsoft/RdpGamepad](https://github.com/microsoft/RdpGamepad). It lets a gamepad
plugged into a Linux desktop show up as a virtual Xbox 360 controller on a remote
Windows machine when you connect with `xfreerdp`, `sdl-freerdp` or `wlfreerdp`.

```
Linux desktop                                   Remote Windows host
┌──────────────────────────────┐               ┌──────────────────────────────┐
│ /dev/input/eventN (evdev)    │               │ RdpGamepadViGEm (receiver)   │
│        ▲                     │  DVC          │        │                     │
│ librdpgamepad-client.so  ◄───┼── "Microsoft. ├──►     ▼                     │
│ (loaded by xfreerdp)         │   RdpGamepad" │ ViGEmBus → virtual X360 pad  │
└──────────────────────────────┘               └──────────────────────────────┘
```

The receiver polls the client for controller state every 16 ms, and the client answers
with the current evdev state. Rumble requests from the remote side are played on the
local controller.

## Remote (Windows) side

Install these two from upstream, exactly as you would for the Windows client:

1. [ViGEmBus](https://github.com/ViGEm/ViGEmBus/releases/latest)
2. `RdpGamepadReceiverInstall-*.exe` from the
   [RdpGamepad releases](https://github.com/microsoft/RdpGamepad/releases/latest)

## Local (Linux) side

```sh
./install.sh            # build + install
./install.sh uninstall
```

* If `freerdp-devel` is not installed, the build runs inside a `fedora:<version>`
  podman container, so nothing is layered onto the host.
* On rpm-ostree systems (Bazzite, Silverblue, Kinoite) `/usr` is read-only. FreeRDP
  only loads add-ins from `/usr/lib64/freerdp3/lib<name>-client.so`, so the add-in is
  installed as a **systemd-sysext** in `/var/lib/extensions/rdpgamepad`.
  `systemd-sysext.service` merges it at every boot.
* On mutable systems the add-in is copied straight into `/usr/lib64/freerdp3`.

Then connect with the add-in enabled:

```sh
xfreerdp /v:my-windows-box /u:me /dvc:rdpgamepad
```

### Options

Pass options as comma-separated values after the add-in name, e.g.
`/dvc:rdpgamepad,match:DualSense,grab`.

| Option | Meaning |
|---|---|
| `device:<path>` | Use this evdev node, e.g. `/dev/input/by-id/usb-…-event-joystick`. Default: the first device that has a south face button and a left stick. |
| `match:<text>` | Auto-detect only devices whose name contains `<text>` (case-insensitive). |
| `layout:xbox` / `layout:positional` | How to read `BTN_NORTH`/`BTN_WEST`. `xpad`, `xone`, `hid-microsoft` and Microsoft-vendor pads use the Xbox layout, where 0x133 is the X button. Every other pad uses positional, where top = Y and left = X. Default: chosen from the driver. |
| `grab` | Use `EVIOCGRAB` so local apps stop seeing the pad while you are connected. Steam reads hidraw, so it is not affected. |

The device is opened when the remote receiver connects and released when it disconnects.
If you unplug the controller, the add-in looks for a controller again once a second.

### Debugging

```sh
WLOG_LEVEL=INFO xfreerdp ... /dvc:rdpgamepad
# or only this channel:
WLOG_FILTER='com.freerdp.channels.rdpgamepad.client:INFO' xfreerdp ...
```

The add-in logs which device it picked, plus its driver, layout and rumble support.

`tests/harness.c` loads the add-in the same way FreeRDP does and acts as the receiver,
so you can check your controller without a remote machine:

```sh
./install.sh build
./build/rdpgamepad-harness ./build/librdpgamepad-client.so 10 --rumble
```

## Limitations

* Only controller 0 is forwarded. The upstream receiver only ever asks for user index 0.
* The add-in is linked against FreeRDP 3 (`libfreerdp-client3.so.3`). After a major
  FreeRDP upgrade, rebuild and reinstall it with `./install.sh`.
* If a game on the remote machine uses "the first controller" and that host has its own
  pad attached, disable the local pad there. See the upstream README.
