#!/bin/sh
# Install packages from public APT sources and an optional secret-mounted source.
set -eu

private_sources=/etc/apt/sources.list.d/pipewireao-private.sources
private_auth=/etc/apt/auth.conf.d/pipewireao-private.conf
private_keyring=/etc/apt/keyrings/pipewireao-private.gpg

cleanup() {
	rm -f "$private_sources" "$private_auth" "$private_keyring"
	rm -rf /var/lib/apt/lists/*
}
trap cleanup EXIT HUP INT TERM

mkdir -p /etc/apt/keyrings
if [ -f /run/secrets/apt_sources ]; then
	install -m 0644 /run/secrets/apt_sources "$private_sources"
fi
if [ -f /run/secrets/apt_auth ]; then
	install -m 0600 /run/secrets/apt_auth "$private_auth"
fi
if [ -f /run/secrets/apt_keyring ]; then
	install -m 0644 /run/secrets/apt_keyring "$private_keyring"
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y "$@"
