#!/bin/rc -e
redo-ifchange $2.c.o
$CC $LDFLAGS -o $3 $2.c.o -ltertium
