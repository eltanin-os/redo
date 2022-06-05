#!/bin/rc -e
redo-ifchange $2
$CC $CFLAGS $CPPFLAGS -o $3 -c $2
