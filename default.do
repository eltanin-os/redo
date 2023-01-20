#!/bin/execlineb -S3
multisubstitute {
	importas -D "" DESTDIR DESTDIR
	importas -D "/usr/local" PREFIX PREFIX
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
	if { install -dm 755 "${DESTDIR}/${PREFIX}/${MANDIR}/man1" }
	if { install -dm 755 "${DESTDIR}/${PREFIX}/${BINDIR}" }
	if { install -cm 644 $MANPAGES "${DESTDIR}/${PREFIX}/${MANDIR}/man1" }
	if { install -cm 755 src/redo "${DESTDIR}/${PREFIX}/${BINDIR}" }
	forx -E prog { redo-$SUBPROGS } ln -s redo "${DESTDIR}/${PREFIX}/${BINDIR}/${prog}"
}
}
foreground {
	fdmove 1 2
	echo no rule for $1
}
exit 1
