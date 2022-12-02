#!/bin/execlineb -S3
multisubstitute {
	importas -D "/usr/local" DESTDIR DESTDIR
	importas -D "/bin" BINDIR BINDIR
	importas -D "/share/man" MANDIR MANDIR
	define -s SUBPROGS "always ifchange ifcreate ood sources stamp targets whichdo"
	elglob MANPAGES "man/*"
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
	if { redo-ifchange all }
	if { install -dm 755 "${DESTDIR}/${MANDIR}/man1" }
	if { install -dm 755 "${DESTDIR}/${BINDIR}" }
	if { install -cm 644 $MANPAGES "${DESTDIR}/${MANDIR}/man1" }
	if { install -cm 755 src/redo "${DESTDIR}/${BINDIR}" }
	forx -E prog { redo-$SUBPROGS } ln -s redo "${DESTDIR}/${BINDIR}/${prog}"
}
}
foreground {
	fdmove 1 2
	echo no rule for $1
}
exit 1
