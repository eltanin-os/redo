#!/bin/execlineb -S3
multisubstitute {
	importas -D "/usr/local" DESTDIR DESTDIR
	importas -D "/bin" BINDIR BINDIR
	define -s SUBPROGS "always ifchange ifcreate ood sources stamp targets whichdo"
}
case -- $1 {
".*\.[1ch]" {
	exit 0
}
"all" {
	redo-ifchange src/redo
}
"clean" {
	backtick targets { redo-targets }
	importas -isu targets targets
	rm -f $targets
}
"install" {
	foreground { redo-ifchange all }
	foreground { install -dm 755 "${DESTDIR}/${BINDIR}" }
	foreground { install -cm 755 src/redo "${DESTDIR}/${BINDIR}" }
	forx -E prog { redo-$SUBPROGS } ln -s redo "${DESTDIR}/${BINDIR}/${prog}"
}
}
foreground {
	fdmove 1 2
	echo no rule for $1
}
exit 1
