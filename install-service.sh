#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "install-service.sh must run as root" >&2
    exit 1
fi

service_user=${1:-${SUDO_USER:-}}
prefix=${2:-/usr/local}
case "$service_user" in
    ''|*[!A-Za-z0-9_.-]*)
        echo "A valid non-root service user is required" >&2
        exit 1
        ;;
esac

service_uid=$(id -u "$service_user")
if [ "$service_uid" -eq 0 ]; then
    echo "Refusing to authorize root as the service user" >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
label=org.asahilinux.macvdmd
plist="/Library/LaunchDaemons/$label.plist"
tmp_plist=$(mktemp "/tmp/$label.XXXXXX")
trap 'rm -f "$tmp_plist"' EXIT HUP INT TERM

for binary in macvdmtool macvdmd macvdmctl; do
    if [ ! -x "$script_dir/$binary" ]; then
        echo "Missing built binary: $binary" >&2
        exit 1
    fi
done

/usr/bin/install -d -m 755 -o root -g wheel "$prefix/bin" "$prefix/libexec"
/usr/bin/install -m 755 -o root -g wheel "$script_dir/macvdmtool" "$prefix/libexec/macvdmtool"
/usr/bin/install -m 755 -o root -g wheel "$script_dir/macvdmd" "$prefix/libexec/macvdmd"
/usr/bin/install -m 755 -o root -g wheel "$script_dir/macvdmctl" "$prefix/bin/macvdmctl"

/usr/bin/sed -e "s|@PREFIX@|$prefix|g" -e "s|@USER@|$service_user|g" \
    "$script_dir/org.asahilinux.macvdmd.plist.in" > "$tmp_plist"
/usr/bin/plutil -lint "$tmp_plist" >/dev/null
/bin/launchctl bootout "system/$label" 2>/dev/null || true
/usr/bin/install -m 644 -o root -g wheel "$tmp_plist" "$plist"
/bin/launchctl bootstrap system "$plist"
/bin/launchctl enable "system/$label"
/bin/launchctl kickstart -k "system/$label"

i=0
while [ ! -S /var/run/macvdmd.sock ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
if [ ! -S /var/run/macvdmd.sock ]; then
    echo "macvdmd failed to create its socket; see /var/log/macvdmd.err" >&2
    exit 1
fi

"$prefix/bin/macvdmctl" ping
echo "Installed $label for $service_user. Use: macvdmctl reboot"
