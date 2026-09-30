# wmsliver - a Window Maker screen locker that wears your theme
VERSION = 0.1.0

PREFIX ?= /usr/local
MANDIR ?= $(PREFIX)/share/man
DOCDIR ?= $(PREFIX)/share/doc/wmsliver

# POWER=0 builds without the power-profiles-daemon support (no libsystemd needed)
POWER ?= 1

PKGS = x11 xft xinerama imlib2
ifeq ($(POWER),1)
PKGS += libsystemd
CPPFLAGS += -DWITH_POWER
endif

CPPFLAGS += -DVERSION=\"$(VERSION)\"
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -std=c99 $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lpam

wmsliver: wmsliver.c

install: wmsliver
	install -Dm755 wmsliver $(DESTDIR)$(PREFIX)/bin/wmsliver
	install -Dm644 wmsliver.pam $(DESTDIR)/etc/pam.d/wmsliver
	install -Dm644 config.example $(DESTDIR)$(DOCDIR)/config.example
	install -Dm644 README.md $(DESTDIR)$(DOCDIR)/README.md

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wmsliver $(DESTDIR)/etc/pam.d/wmsliver
	rm -rf $(DESTDIR)$(DOCDIR)

clean:
	rm -f wmsliver

.PHONY: install uninstall clean
