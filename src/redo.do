#!/bin/execlineb -S3
multisubstitute {
	importas -sD "cc" CC CC
	importas -sD "" LDFLAGS LDFLAGS
}
if { redo-ifchange ${2}.c.o }
$CC $LDFLAGS -o $3 ${2}.c.o -ltertium
