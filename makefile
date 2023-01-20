.SUFFIXES:
.SUFFIXES: .o .c .s

DESTDIR ?=
PREFIX ?= /usr/local
INCDIR ?= /include
BINDIR ?= /bin
LIBDIR ?= /lib
MANDIR ?= /man

CC ?= cc
CFLAGS ?= -O0 -g -std=c99 -Wall -Wextra -pedantic
AR ?= ar
RANLIB ?= ranlib

MANPAGES=\
	man/redo-sources.1\
	man/redo-ifcreate.1\
	man/redo-ifchange.1\
	man/redo-whichdo.1\
	man/redo-stamp.1\
	man/redo-always.1\
	man/redo-targets.1\
	man/redo-ood.1\
	man/redo.1

all: src/redo

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ -c $<

src/redo: src/redo.o
	$(CC) $(LDFLAGS) -o $@ $< -ltertium

install: all
	install -dm 755 "$(DESTDIR)/$(PREFIX)/$(MANDIR)/man1"
	install -dm 755 "$(DESTDIR)/$(PREFIX)/$(BINDIR)"
	install -cm 644 $(MANPAGES) "$(DESTDIR)/$(PREFIX)/$(MANDIR)/man1"
	install -cm 755 src/redo "$(DESTDIR)/$(PREFIX)/$(BINDIR)"
	for i in always ifchange ifcreate ood sources stamp targets whichdo; do ln -s redo "$(DESTDIR)/$(PREFIX)/$(BINDIR)/redo-$${i}"; done

clean:
	rm -f src/redo src/redo.o

