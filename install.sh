#!/usr/bin/env bash
# Builds librdpgamepad-client.so and installs it where FreeRDP looks for add-ins.
#
#   ./install.sh build      build only (in a podman container if freerdp-devel is missing)
#   ./install.sh install    build, then install (systemd-sysext on rpm-ostree systems)
#   ./install.sh uninstall
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

NAME=rdpgamepad
LIB=librdpgamepad-client.so
SYSEXT_DIR=/var/lib/extensions/$NAME
FEDORA_IMAGE=${FEDORA_IMAGE:-quay.io/fedora/fedora:$(. /etc/os-release; echo "${VERSION_ID%%.*}")}

build() {
	if pkg-config --exists freerdp-client3 winpr3 2>/dev/null; then
		cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_HARNESS=ON
		cmake --build build
	else
		echo ">> freerdp-devel not installed, building in $FEDORA_IMAGE"
		podman run --rm -v "$PWD":/src:Z -w /src "$FEDORA_IMAGE" bash -c '
			dnf -y -q install gcc cmake pkgconf-pkg-config freerdp-devel >/dev/null &&
			cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_HARNESS=ON &&
			cmake --build build'
	fi
	echo ">> built build/$LIB"
}

addin_dir() {
	local libdir
	libdir=$(pkg-config --variable=libdir freerdp3 2>/dev/null || true)
	echo "${libdir:-/usr/lib64}/freerdp3"
}

sysext_arch() {
	case "$(uname -m)" in
		x86_64) echo x86-64 ;;
		aarch64) echo arm64 ;;
		*) uname -m ;;
	esac
}

install_sysext() {
	local stage
	stage=$(mktemp -d)
	chmod 755 "$stage"

	install -Dm755 "build/$LIB" "$stage/usr/lib64/freerdp3/$LIB"
	install -Dm644 /dev/stdin "$stage/usr/lib/extension-release.d/extension-release.$NAME" <<-EOF
		ID=_any
		ARCHITECTURE=$(sysext_arch)
	EOF

	sudo rm -rf "$SYSEXT_DIR"
	sudo mkdir -p /var/lib/extensions
	sudo cp -a "$stage" "$SYSEXT_DIR"
	sudo chown -R root:root "$SYSEXT_DIR"
	sudo restorecon -R "$SYSEXT_DIR" 2>/dev/null || true
	# systemd-sysext.service re-merges /var/lib/extensions at every boot
	sudo systemctl enable systemd-sysext.service
	sudo systemd-sysext refresh
	rm -rf "$stage"
	echo ">> installed as system extension $SYSEXT_DIR"
}

install_plain() {
	sudo install -Dm755 "build/$LIB" "$(addin_dir)/$LIB"
	echo ">> installed $(addin_dir)/$LIB"
}

case "${1:-install}" in
	build)
		build
		;;
	install)
		build
		if [ -e /run/ostree-booted ]; then install_sysext; else install_plain; fi
		echo ">> connect with: xfreerdp /v:<host> /dvc:$NAME   (or sdl-freerdp / wlfreerdp)"
		;;
	uninstall)
		if [ -d "$SYSEXT_DIR" ]; then
			sudo rm -rf "$SYSEXT_DIR"
			sudo systemd-sysext refresh
		else
			sudo rm -f "$(addin_dir)/$LIB"
		fi
		echo ">> uninstalled"
		;;
	*)
		echo "usage: $0 [build|install|uninstall]" >&2
		exit 2
		;;
esac
