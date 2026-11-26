PREFIX ?= /usr
PAMDIR ?= /usr/lib/security
SYSCONFDIR ?= /etc/surface-ir-hello
SYSTEMDDIR ?= /usr/lib/systemd/system
PY_SITEDIR := $(shell /usr/bin/python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -fPIC

all: pam/pam_surface_ir.so

pam/pam_surface_ir.so: pam/pam_surface_ir.c
	$(CC) $(CFLAGS) -shared $< -lpam -o $@

install: all
	install -Dm755 pam/pam_surface_ir.so $(DESTDIR)$(PAMDIR)/pam_surface_ir.so
	install -Dm755 src/surface-ir $(DESTDIR)$(PREFIX)/bin/surface-ir
	install -d $(DESTDIR)$(PY_SITEDIR)/surface_ir_hello
	install -m644 src/surface_ir_hello/*.py $(DESTDIR)$(PY_SITEDIR)/surface_ir_hello/
	install -Dm644 packaging/surface-ir-hellod.service $(DESTDIR)$(SYSTEMDDIR)/surface-ir-hellod.service
	install -d -m755 $(DESTDIR)$(SYSCONFDIR)
	test -f $(DESTDIR)$(SYSCONFDIR)/config.toml || install -m644 packaging/config.toml $(DESTDIR)$(SYSCONFDIR)/config.toml

uninstall:
	-surface-ir pam-disable
	-systemctl disable --now surface-ir-hellod.service
	rm -f $(DESTDIR)$(PAMDIR)/pam_surface_ir.so
	rm -f $(DESTDIR)$(PREFIX)/bin/surface-ir
	rm -rf $(DESTDIR)$(PY_SITEDIR)/surface_ir_hello
	rm -f $(DESTDIR)$(SYSTEMDDIR)/surface-ir-hellod.service

clean:
	rm -f pam/pam_surface_ir.so

.PHONY: all install uninstall clean
