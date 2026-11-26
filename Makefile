BUILD_DIR ?= build
PREFIX ?= /usr
KDE ?= OFF

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_INSTALL_PREFIX=$(PREFIX) -DCMAKE_BUILD_TYPE=Release -DSURFACE_IR_KDE=$(KDE)
	cmake --build $(BUILD_DIR)

install: all
	DESTDIR="$(DESTDIR)" cmake --install $(BUILD_DIR)

uninstall:
	-surface-ir pam-disable
	-systemctl disable --now surface-ir-hellod.service
	cmake -DMANIFEST=$(BUILD_DIR)/install_manifest.txt -P packaging/uninstall.cmake

clean:
	cmake --build $(BUILD_DIR) --target clean

.PHONY: all install uninstall clean
