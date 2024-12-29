PKG_CONFIG?=pkg-config
WAYLAND_PROTOCOLS!=$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER!=$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner

PKGS="wlroots-0.18" wayland-server xkbcommon libinput
CFLAGS_PKG_CONFIG!=$(PKG_CONFIG) --cflags $(PKGS)
CFLAGS+=$(CFLAGS_PKG_CONFIG)
CFLAGS+=-Ibuild/protocols
LIBS!=$(PKG_CONFIG) --libs $(PKGS)

SRC_FILES := $(wildcard src/*.c)
OBJ_FILES := $(patsubst src/%.c, build/%.o, $(SRC_FILES))

all: build/owl build/owl-ipc

build:
	mkdir -p build

build/protocols: build
	mkdir -p build/protocols

build/protocols/xdg-shell-protocol.h: build/protocols
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

build/protocols/wlr-layer-shell-unstable-v1-protocol.h: build/protocols
	$(WAYLAND_SCANNER) server-header \
		./protocols/wlr-layer-shell-unstable-v1.xml $@

build/protocols/xdg-output-unstable-v1-protocol.h: build/protocols
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-output/xdg-output-unstable-v1.xml $@

build/%.o: src/%.c src/%.h build build/protocols/xdg-shell-protocol.h build/protocols/wlr-layer-shell-unstable-v1-protocol.h build/protocols/xdg-output-unstable-v1-protocol.h
	$(CC) -c $< $(CFLAGS) -DWLR_USE_UNSTABLE -o $@

build/owl: $(OBJ_FILES)
	$(CC) $^ $> $(CFLAGS) $(LDFLAGS) $(LIBS) -o $@

build/owl-ipc: owl-ipc/owl-ipc.c
	$(CC) $< -o $@

install: build/owl build/owl-ipc default.conf owl-portals.conf
	cp build/owl /usr/local/bin/owl; \
	cp build/owl-ipc /usr/local/bin/owl-ipc; \
	mkdir -p /usr/share/owl; \
	cp default.conf /usr/share/owl/default.conf; \
	cp owl.desktop /usr/share/wayland-sessions/owl.desktop; \
	chmod 777 /usr/share/wayland-sessions/owl.desktop; \
	cp owl-portals.conf /usr/share/xdg-desktop-portal/owl-portals.conf

uninstall:
	rm /usr/local/bin/owl; \
	rm /usr/local/bin/owl-ipc; \
	rm -rf /usr/share/owl; \
	rm /usr/share/xdg-desktop-portal/owl-portals.conf

clean:
	rm -rf build 2>/dev/null

.PHONY: all clean install uninstall
