#!/bin/rc -e
if (~ $1 *.[1ch]) exit
MAINDIR=$PWD
if (test -e config.rc) {
	redo-ifchange config.rc
	. $MAINDIR/config.rc ||;
}; if not {
	redo-ifcreate config.rc
}
SUBPROGS='redo-'^(always ifchange ifcreate ood sources stamp targets whichdo)
MANPAGES=man/*
switch ($1) {
case all
	redo-ifchange src/redo
case clean
	rm -f `{redo-targets}
case install
	redo-always
	redo-ifchange all install-man
	install -dm 755 $"DESTDIR/$"BINDIR
	install -cm 755 src/redo $"DESTDIR/$"BINDIR
	for (prog in $SUBPROGS) ln -s redo $"DESTDIR/$"BINDIR/$prog
case install-man
	redo-always
	redo-ifchange $MANPAGES
	install -dm 755 $"DESTDIR/$"MANDIR/man1
	install -cm 644 $MANPAGES $"DESTDIR/$"MANDIR/man1
case *
	echo no rule for ''''$1'''' >[1=2]
	exit 1
}
