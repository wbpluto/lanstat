ZIG ?= zig
CC_HOST ?= clang
CFLAGS := -std=c11 -Wall -Wextra -Werror
ARCH ?= aarch64

ZIG_TARGET_x86_64 := x86_64-linux-musl
ZIG_TARGET_aarch64 := aarch64-linux-musl
ZIG_TARGET_armv7 := arm-linux-musleabihf
ZIG_TARGET_mipsel := mipsel-linux-musleabi
ZIG_TARGET_mips := mips-linux-musleabi

SRCS_CORE := src/hosts.c src/http.c
SRCS_ALL := src/main.c src/hosts.c src/capture.c src/http.c

.PHONY: all assets test check-js check-arches build build-x86_64 build-aarch64 build-armv7 build-mipsel build-mips package clean

all: check-js test build package

assets: src/web_assets.h

src/web_assets.h: web/index.html web/oui_packed.txt scripts/embed_web.py
	python3 scripts/embed_web.py web/index.html src/web_assets.h

check-js:
	python3 -c 'import re, subprocess, tempfile; html = open("web/index.html", encoding="utf-8").read(); s = re.findall(r"<script>(.*?)</script>", html, re.S); assert len(s) == 1; f = tempfile.NamedTemporaryFile("w", suffix=".js", encoding="utf-8"); f.write(s[0]); f.flush(); subprocess.run(["node", "--check", f.name], check=True); f.close()'
	python3 -c 'import subprocess, tempfile; src = open("openwrt/www/luci-static/resources/view/lanstat.js", encoding="utf-8").read(); f = tempfile.NamedTemporaryFile("w", suffix=".js", encoding="utf-8"); f.write("function _luci_view() {\n" + src + "\n}\n"); f.flush(); subprocess.run(["node", "--check", f.name], check=True); f.close()'

test: assets
	mkdir -p dist
	$(CC_HOST) $(CFLAGS) -O1 -g -fsanitize=address,undefined -Isrc $(SRCS_CORE) tests/test_lanstat.c -o dist/test_lanstat_host
	./dist/test_lanstat_host

build: assets
	mkdir -p dist
	$(ZIG) cc -target $(ZIG_TARGET_$(ARCH)) $(CFLAGS) -Os -s -static -Isrc $(SRCS_ALL) -o dist/lanstat

build-x86_64:
	$(MAKE) build ARCH=x86_64

build-aarch64:
	$(MAKE) build ARCH=aarch64

build-armv7:
	$(MAKE) build ARCH=armv7

build-mipsel:
	$(MAKE) build ARCH=mipsel

build-mips:
	$(MAKE) build ARCH=mips

check-arches: assets
	mkdir -p dist/arch-check
	for arch in x86_64 aarch64 armv7 mipsel mips; do \
		case $$arch in \
			x86_64) target="$(ZIG_TARGET_x86_64)" ;; \
			aarch64) target="$(ZIG_TARGET_aarch64)" ;; \
			armv7) target="$(ZIG_TARGET_armv7)" ;; \
			mipsel) target="$(ZIG_TARGET_mipsel)" ;; \
			mips) target="$(ZIG_TARGET_mips)" ;; \
		esac; \
		echo "Building $$arch ($$target)..."; \
		$(ZIG) cc -target "$$target" $(CFLAGS) -Os -s -static -Isrc $(SRCS_ALL) -o "dist/arch-check/lanstat-$$arch" || exit 1; \
	done

package: build
	python3 scripts/gen_lmo.py openwrt/usr/lib/lua/luci/i18n
	python3 -c 'import shutil, pathlib; p=pathlib.Path("dist/pkg"); shutil.rmtree(p, ignore_errors=True)'
	mkdir -p dist/pkg/usr/sbin dist/pkg/etc/config dist/pkg/etc/init.d \
		dist/pkg/usr/share/luci/menu.d dist/pkg/usr/share/rpcd/acl.d \
		dist/pkg/www/luci-static/resources/view dist/pkg/usr/lib/lua/luci/i18n
	cp dist/lanstat dist/pkg/usr/sbin/lanstat
	cp openwrt/etc/config/lanstat dist/pkg/etc/config/lanstat
	cp openwrt/etc/init.d/lanstat dist/pkg/etc/init.d/lanstat
	cp openwrt/usr/share/luci/menu.d/lanstat.json dist/pkg/usr/share/luci/menu.d/lanstat.json
	cp openwrt/usr/share/rpcd/acl.d/lanstat.json dist/pkg/usr/share/rpcd/acl.d/lanstat.json
	cp openwrt/www/luci-static/resources/view/lanstat.js dist/pkg/www/luci-static/resources/view/lanstat.js
	cp openwrt/usr/lib/lua/luci/i18n/lanstat.zh-cn.lmo dist/pkg/usr/lib/lua/luci/i18n/lanstat.zh-cn.lmo
	cp openwrt/usr/lib/lua/luci/i18n/lanstat.zh-tw.lmo dist/pkg/usr/lib/lua/luci/i18n/lanstat.zh-tw.lmo
	cp openwrt/install-router.sh dist/pkg/install.sh
	cp openwrt/rollback-router.sh dist/pkg/rollback.sh
	chmod 0755 dist/pkg/usr/sbin/lanstat dist/pkg/etc/init.d/lanstat dist/pkg/install.sh dist/pkg/rollback.sh
	chmod 0644 dist/pkg/etc/config/lanstat \
		dist/pkg/usr/share/luci/menu.d/lanstat.json \
		dist/pkg/usr/share/rpcd/acl.d/lanstat.json \
		dist/pkg/www/luci-static/resources/view/lanstat.js \
		dist/pkg/usr/lib/lua/luci/i18n/lanstat.zh-cn.lmo \
		dist/pkg/usr/lib/lua/luci/i18n/lanstat.zh-tw.lmo
	@if tar --version 2>/dev/null | grep -qi gnu; then \
		tar --owner=0 --group=0 -C dist/pkg -czf dist/lanstat-$(ARCH).tar.gz .; \
	else \
		COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --uname root --gname root -C dist/pkg -czf dist/lanstat-$(ARCH).tar.gz .; \
	fi
	@if command -v sha256sum >/dev/null 2>&1; then \
		(cd dist && sha256sum lanstat lanstat-$(ARCH).tar.gz > SHA256SUMS.txt); \
	else \
		(cd dist && shasum -a 256 lanstat lanstat-$(ARCH).tar.gz > SHA256SUMS.txt); \
	fi

clean:
	python3 -c 'import shutil, pathlib; p=pathlib.Path("dist"); [shutil.rmtree(p/x, ignore_errors=True) for x in ["pkg", "arch-check"]]; [f.unlink(missing_ok=True) for f in p.glob("lanstat-*.tar.gz")]; [pathlib.Path("dist", x).unlink(missing_ok=True) for x in ["test_lanstat_host", "lanstat", "SHA256SUMS.txt"]]'
