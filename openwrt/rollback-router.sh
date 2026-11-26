#!/bin/sh
set -eu

if [ -x /etc/init.d/lanstat ]; then
	/etc/init.d/lanstat stop 2>/dev/null || true
	/etc/init.d/lanstat disable 2>/dev/null || true
fi

if [ -x /etc/init.d/darkstat ]; then
	/etc/init.d/darkstat enable
	/etc/init.d/darkstat start
	printf 'Rolled back to darkstat.\n'
else
	printf 'lanstat stopped; /etc/init.d/darkstat not found.\n'
fi
