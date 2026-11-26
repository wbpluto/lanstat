#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"

if [ ! -x "$SCRIPT_DIR/usr/sbin/lanstat" ]; then
	chmod +x "$SCRIPT_DIR/usr/sbin/lanstat"
fi

if ! "$SCRIPT_DIR/usr/sbin/lanstat" -v; then
	printf 'Error: binary does not match this router architecture (%s)\n' "$(uname -m)" >&2
	exit 1
fi

if [ -x /etc/init.d/darkstat ]; then
	printf 'Stopping and disabling darkstat (kept installed for rollback)...\n'
	/etc/init.d/darkstat stop 2>/dev/null || true
	/etc/init.d/darkstat disable 2>/dev/null || true
	waited=0
	while [ -n "$(pidof darkstat || true)" ] && [ "$waited" -lt 6 ]; do
		sleep 1
		waited=$((waited + 1))
	done
fi

if [ -x /etc/init.d/lanstat ]; then
	/etc/init.d/lanstat stop 2>/dev/null || true
	waited=0
	while [ -n "$(pidof lanstat || true)" ] && [ "$waited" -lt 6 ]; do
		sleep 1
		waited=$((waited + 1))
	done
fi

mkdir -p /usr/sbin /etc/config /etc/init.d
cp "$SCRIPT_DIR/usr/sbin/lanstat" /usr/sbin/lanstat
chmod 0755 /usr/sbin/lanstat
if [ ! -f /etc/config/lanstat ]; then
	cp "$SCRIPT_DIR/etc/config/lanstat" /etc/config/lanstat
	chmod 0644 /etc/config/lanstat
fi
cp "$SCRIPT_DIR/etc/init.d/lanstat" /etc/init.d/lanstat
chmod 0755 /etc/init.d/lanstat

if [ -f "$SCRIPT_DIR/usr/share/luci/menu.d/lanstat.json" ]; then
	mkdir -p /usr/share/luci/menu.d /usr/share/rpcd/acl.d /www/luci-static/resources/view /usr/lib/lua/luci/i18n
	cp "$SCRIPT_DIR/usr/share/luci/menu.d/lanstat.json" /usr/share/luci/menu.d/lanstat.json
	cp "$SCRIPT_DIR/usr/share/rpcd/acl.d/lanstat.json" /usr/share/rpcd/acl.d/lanstat.json
	cp "$SCRIPT_DIR/www/luci-static/resources/view/lanstat.js" /www/luci-static/resources/view/lanstat.js
	cp "$SCRIPT_DIR/usr/lib/lua/luci/i18n/lanstat.zh-cn.lmo" /usr/lib/lua/luci/i18n/lanstat.zh-cn.lmo
	cp "$SCRIPT_DIR/usr/lib/lua/luci/i18n/lanstat.zh-tw.lmo" /usr/lib/lua/luci/i18n/lanstat.zh-tw.lmo
	chmod 0644 /usr/share/luci/menu.d/lanstat.json \
		/usr/share/rpcd/acl.d/lanstat.json \
		/www/luci-static/resources/view/lanstat.js \
		/usr/lib/lua/luci/i18n/lanstat.zh-cn.lmo \
		/usr/lib/lua/luci/i18n/lanstat.zh-tw.lmo
	rm -f /tmp/luci-indexcache*
	rm -rf /tmp/luci-modulecache/*
	if [ -x /etc/init.d/rpcd ]; then
		/etc/init.d/rpcd reload >/dev/null 2>&1 || true
	fi
fi

/etc/init.d/lanstat enable
/etc/init.d/lanstat start

sleep 2
PID="$(pidof lanstat || true)"
if [ -z "$PID" ]; then
	printf 'Error: lanstat failed to start; check logread | grep lanstat\n' >&2
	exit 1
fi

printf 'lanstat running with PID %s\n' "$PID"
grep -E 'VmRSS|VmSize' "/proc/${PID%% *}/status" 2>/dev/null || true

PORT="$(uci -q get lanstat.main.port || echo 6670)"
ROUTER_IP="$(uci -q get network.lan.ipaddr 2>/dev/null | cut -d/ -f1 || true)"
[ -n "$ROUTER_IP" ] || ROUTER_IP="<router-ip>"
if command -v uclient-fetch >/dev/null 2>&1; then
	uclient-fetch -q -T 3 -O - "http://127.0.0.1:${PORT}/api/stats" | head -c 240
	printf '\n'
elif command -v curl >/dev/null 2>&1; then
	curl -s --max-time 3 "http://127.0.0.1:${PORT}/api/stats" | head -c 240
	printf '\n'
fi
printf 'Done! Open http://%s:%s/ in your browser.\n' "$ROUTER_IP" "$PORT"
