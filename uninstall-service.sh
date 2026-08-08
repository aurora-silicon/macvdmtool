#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "uninstall-service.sh must run as root" >&2
    exit 1
fi

prefix=${1:-/usr/local}
label=org.asahilinux.macvdmd
plist="/Library/LaunchDaemons/$label.plist"

/bin/launchctl bootout "system/$label" 2>/dev/null || true
/bin/rm -f "$plist" /var/run/macvdmd.sock
/bin/rm -f "$prefix/bin/macvdmctl" "$prefix/libexec/macvdmd" "$prefix/libexec/macvdmtool"
echo "Uninstalled $label"
