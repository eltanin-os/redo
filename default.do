#!/bin/execlineb -S3
multisubstitute {
	importas -D "/usr/local" DESTDIR DESTDIR
	importas -D "/bin" BINDIR BINDIR
	define -s SUBPROGS "always ifchange ifcreate ood sources stamp targets whichdo"
}
ifelse { test "${1}" = "all" } {
	redo-ifchange src/redo
}
ifelse { test "${1}" = "clean" } {
	backtick targets { redo-targets }
	importas -isu targets targets
	rm -f $targets
}
ifelse { test "${1}" = "install" } {
	foreground { redo-ifchange all }
	foreground { install -dm 755 "${DESTDIR}/${BINDIR}" }
	foreground { install -cm 755 src/redo "${DESTDIR}/${BINDIR}" }
	forx -E prog { redo-$SUBPROGS } ln -s redo "${DESTDIR}/${BINDIR}/${prog}"
}
exit 0
