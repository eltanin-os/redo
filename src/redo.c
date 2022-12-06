#include <tertium/cpu.h>
#include <tertium/std.h>

#define arglist(...) arglist_(__VA_ARGS__, nil)

#define TMPFILE ".tmpfile.XXXXXXXXX"

#define REDO_XFLAG "_TERTIUM_REDO_XFLAG"
#define REDO_ROOTDIR "_TERTIUM_REDO_ROOTDIR"
#define REDO_ROOTFD "_TERTIUM_REDO_ROOTFD"
#define REDO_TARGET "_TERTIUM_REDO_TARGET"
#define REDO_DEPFD "_TERTIUM_REDO_DEPFD"
#define REDO_DEPTH "_TERTIUM_REDO_DEPTH"

#define HDEC(x) ((x <= '9') ? x - '0' : (((uchar)x | 32) - 'a') + 10)

static ctype_node *tmpfiles;

static ctype_fd redo_rootfd;
static ctype_fd redo_depfd;
static usize redo_rootdir_len;
static int redo_depth;
static char *redo_rootdir;
static char *pwd;

static int fflag;
static int xflag;

/* ctors/dtors routines */
static ctype_status
recdel(char **args)
{
	ctype_dent *ep;
	ctype_dir dir;
	if (c_dir_open(&dir, args, 0, nil) < 0) return -1;
	while ((ep = c_dir_read(&dir))) {
		switch (ep->info) {
		case C_DIR_FSDP:
			c_nix_rmdir(ep->path);
			break;
		case C_DIR_FSNS:
		case C_DIR_FSERR:
			break;
		default:
			c_nix_unlink(ep->path);
		}
	}
	c_dir_close(&dir);
	return 0;
}

static void
cleantrash(void)
{
	ctype_node *p;
	usize len;
	char **args;
	char *argv[2];

	len = 0;
	p = tmpfiles->next;
	do { ++len; } while ((p = p->next)->prev);

	if (!(args = c_std_alloc(len+1, sizeof(char *)))) goto nomem;
	len = 0;
	p = tmpfiles->next;
	do { args[len++] = p->p; } while ((p = p->next)->prev);
	args[len] = 0;
	if (recdel(args) < 0) goto nomem;
	goto free;
nomem:
	/* non-recursive fallback */
	p = tmpfiles->next;
	do { c_nix_unlink(p->p); } while ((p = p->next)->prev);
free:
	c_std_free(args);
	while ((p = c_adt_lpop(&tmpfiles))) c_adt_lfree(p);
}

static void
trackfile(char *s, usize n)
{
	ctype_node *p;
	p = c_adt_lnew(s, n);
	if (c_adt_lpush(&tmpfiles, p) < 0) c_err_diex(1, nil);
}

/* fmt routines */
static ctype_status
hex32(ctype_fmt *p)
{
	u32 x;
	char buf[sizeof(u32)];
	x = va_arg(p->args, u32);
	c_uint_32pack(buf, x);
	for (int i = 0; i < 4; ++i) c_fmt_print(p, "%02x", (uchar)buf[i]);
	return 0;
}

static ctype_status
hex64(ctype_fmt *p)
{
	u64 x;
	char buf[sizeof(u64)];
	x = va_arg(p->args, u64);
	c_uint_64pack(buf, x);
	for (int i = 0; i < 8; ++i) c_fmt_print(p, "%02x", (uchar)buf[i]);
	return 0;
}

/* err routines */
static char **
arglist_(char *prog, ...)
{
	char **av;
	va_list ap;
	va_start(ap, prog);
	if (!(av = c_exc_varglist(prog, ap))) c_err_diex(1, nil);
	va_end(ap);
	return av;
}

static void
arrfmt(ctype_arr *p, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (c_arr_vfmt(p, fmt, ap) < 0) {
		errno = C_ERR_ENAMETOOLONG;
		c_err_diex(1, "failed to generate path");
	}
	va_end(ap);
}

static ctype_fd
fdopen2(char *s, uint opts)
{
	ctype_fd fd;
	if ((fd = c_nix_fdopen2(s, opts)) < 0)
		c_err_die(1, "failed to open \"%s\"", s);
	return fd;
}

static void
fdstat(ctype_stat *p, ctype_fd fd)
{
	if (c_nix_fdstat(p, fd) < 0)
		c_err_die(1, "failed to obtain file info");
}

static void
setenv(char *k, char *v)
{
	if (c_exc_setenv(k, v) < 0) c_err_die(1, nil);
}

static void *
dynalloc(ctype_arr *p, usize m, usize n)
{
	void *v;
	if (!(v = c_dyn_alloc(p, m, n))) c_err_diex(1, nil);
	return v;
}

static void
dynfmt(ctype_arr *p, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (c_dyn_vfmt(p, fmt, ap) < 0) c_err_diex(1, nil);
	va_end(ap);
}

static size
getln(ctype_ioq *p, ctype_arr *ap)
{
	size r;
	if ((r = c_ioq_getln(p, ap)) < 0) c_err_diex(1, nil);
	return r;
}

static void
mkpath(char *s, uint mode, uint dmode)
{
	if (c_nix_mkpath(s, mode, dmode) < 0)
		c_err_die(1, "failed to generate path %s", s);
}

static ctype_fd
mktemp(char *s, usize n, uint opts)
{
	ctype_fd fd;
	fd = c_nix_mktemp5(s, n, opts, C_NIX_OAPPEND, 0644);
	if (fd < 0) c_err_die(1, "failed to obtain temporary file");
	trackfile(s, n);
	return fd;
}

static char *
strfmt(char *fmt, ...)
{
	va_list ap;
	char *tmp;
	va_start(ap, fmt);
	if (c_str_vfmt(&tmp, fmt, ap) < 0) c_err_diex(1, nil);
	va_end(ap);
	return tmp;
}

static void
waitchild(ctype_id id)
{
	ctype_status status;
	switch (c_exc_wait(id, &status)) {
	case 1:
		if (!status) return;
		c_err_diex(1, "child closed with exit status %d", status);
	case 2:
		c_err_diex(1, "child closed with signal %d", status);
	}
}

/* env routines */
static int
getnum(char *k)
{
	int x;
	if (!(k = c_std_getenv(k))) return -1;
	x = c_std_strtovl(k, 10,  C_LIM_INTMIN, C_LIM_INTMAX, nil, nil);
	return x;
}

static void
setnum(char *k, int x)
{
	ctype_arr arr;
	char buf[sizeof(x) << 1];
	c_arr_init(&arr, buf, sizeof(buf));
	arrfmt(&arr, "%d", x);
	setenv(k, c_arr_data(&arr));
}

/* path routines */
static char *
basefilename(char *dofile, char *s)
{
	static char buf[C_LIM_PATHMAX];
	int nstrip;
	char *tmp;

	dofile = c_gen_basename(dofile);
	if (!C_STR_CMP("default.", dofile)) {
		nstrip = 0;
		tmp = dofile + 8;
		while ((tmp = c_str_chr(tmp+1, -1, '.'))) ++nstrip;
	} else {
		nstrip = 1;
	}
	c_str_cpy(buf, sizeof(buf), c_gen_basename(s));
	while (nstrip) {
		tmp = c_str_rchr(buf, sizeof(buf), '.');
		if (!tmp) break;
		*tmp = 0;
		--nstrip;
	}
	return buf;
}

static char *
dirname(char *s)
{
	static char buf[C_LIM_PATHMAX];
	c_str_cpy(buf, -1, s);
	return c_gen_dirname(buf);
}

static ctype_status
exist(char *s)
{
	ctype_stat st;
	if (c_nix_lstat(&st, s) < 0) return 0;
	return 1;
}

static char *
getpwd(void)
{
	static char buf[C_LIM_PATHMAX];
	char *s;
	s = c_nix_getcwd(buf, sizeof(buf));
	if (!s) c_err_die(1, "failed to obtain current dir");
	return s;
}

static char *
getdbpath(void)
{
	static char buf[C_LIM_PATHMAX];
	ctype_arr arr;
	c_arr_init(&arr, buf, sizeof(buf));
	arrfmt(&arr, "%s/.redo", redo_rootdir);
	return c_arr_data(&arr);
}

static char *
toabs(char *s)
{
	ctype_arr arr;
	char buf[C_LIM_PATHMAX];
	char *tmp;
	c_arr_init(&arr, buf, sizeof(buf));
	if (s[0] == '/') {
		tmp = dirname(s);
	} else {
		arrfmt(&arr, "%s/%s", pwd, s);
		tmp = c_gen_dirname(c_arr_data(&arr));
	}
	s = strfmt("%s/%s", tmp, c_gen_basename(s));
	while ((tmp = c_str_str(s, -1, "/../"))) {
		c_str_cpy(c_str_rchr(s, (tmp - s) - 1, '/'), -1, tmp + 3);
	}
	while ((tmp = c_str_str(s, -1, "/./"))) c_str_cpy(tmp, -1, tmp + 2);
	return s;
}

static char *
pathshrink(char *s)
{
	if (c_str_cmp(s, redo_rootdir_len, redo_rootdir)) return s;
	return s + redo_rootdir_len + 1;
}

/* utils routines */
static void
checkparent(void)
{
	if (redo_depfd == -1) c_err_diex(1, "must be run from inside a .do");
}

static ctype_status
fdput(ctype_fmt *p, char *s, usize n)
{
	return c_nix_allrw(&c_nix_fdwrite, *(ctype_fd *)p->farg, s, n);
}

static ctype_status
fdfmt(ctype_fd fd, char *fmt, ...)
{
	ctype_fmt f;
	va_list ap;
	c_fmt_init(&f, &fd, &fdput);
	va_start(ap, fmt);
	va_copy(f.args, ap);
	va_end(ap);
	return c_fmt_fmt(&f, fmt);
}

static int
hexcmp(char *p, usize n, char *s)
{
	while (n--) {
		if (((HDEC(p[0]) << 4) | HDEC(p[1])) != (uchar)*s++) return 1;
		p += 2;
	}
	return 0;
}

static char *
progname(char *dofile, int *toexec)
{
	static ctype_arr arr; /* "memory leak" */
	ctype_stat st;
	ctype_ioq ioq;
	ctype_fd fd;
	int isexec;
	char buf[C_IOQ_BSIZ];
	char *s;

	fd = fdopen2(dofile, C_NIX_OREAD);
	isexec = (c_nix_fdstat(&st, fd) == 0) && (st.mode & C_NIX_IXUSR);
	/* get first line */
	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);
	c_arr_trunc(&arr, 0, sizeof(uchar));
	if (!getln(&ioq, &arr)) goto shfallback;
	c_nix_fdclose(fd);
	/* check for shellbang */
	c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar)); /* linefeed */
	s = c_arr_data(&arr);
	if (!(s[0] == '#' && s[1] == '!')) goto shfallback;
	if (isexec) return dofile;
	*toexec = 1;
	return (char *)c_arr_data(&arr) + 2;
shfallback:
	c_arr_trunc(&arr, 0, sizeof(uchar));
	dynfmt(&arr, "/bin/sh -e%s", xflag ? "x" : "");
	*toexec = 1;
	return c_arr_data(&arr);
}

static char **
getargs(char *dofile, char *target, char *out)
{
	int toexec;
	char *base, *prog;
	prog = progname(dofile, (toexec = 0, &toexec));
	base = basefilename(dofile, target);
	if (toexec) return arglist(prog, dofile, target, base, out);
	return arglist(prog, target, base, out);
}

static void
execout(char **args)
{
	ctype_id id;
	id = c_exc_spawn0(*args, args, environ);
	if (!id) c_err_die(1, "failed to execute %s", *args);
	waitchild(id);
}

/* deps routines */
static int
modified(ctype_fd fd, char *s)
{
	ctype_stat st;
	char buf[sizeof(u64)];
	/* last modification time */
	fdstat(&st, fd);
	c_uint_32pack(buf, st.mtim.sec);
	if (hexcmp(s+1, sizeof(u32), buf)) return 1;
	/* size */
	c_uint_64pack(buf, st.size);
	if (hexcmp(s+10, sizeof(u64), buf)) return 1;
	/* inode */
	c_uint_64pack(buf, st.ino);
	if (hexcmp(s+27, sizeof(u64), buf)) return 1;
	/* file mode */
	c_uint_32pack(buf, st.mode);
	if (hexcmp(s+44, sizeof(u32), buf)) return 1;
	/* user id */
	c_uint_32pack(buf, st.uid);
	if (hexcmp(s+53, sizeof(u32), buf)) return 1;
	/* group id */
	c_uint_32pack(buf, st.gid);
	if (hexcmp(s+62, sizeof(u32), buf)) return 1;
	return 0;
}

static char *
pathdep(char *target)
{
	static char buf[C_LIM_PATHMAX];
	ctype_arr arr;
	c_arr_init(&arr, buf, sizeof(buf));
	arrfmt(&arr, "%s/.redo/%s.dep", redo_rootdir, target);
	target = c_arr_data(&arr);
	mkpath(dirname(target), 0777, 0777);
	return target;
}

static int
depcheck(char *target)
{
	ctype_ioq ioq;
	ctype_arr arr;
	ctype_fd fd;
	int ok;
	char buf[C_IOQ_BSIZ];
	char *dep, *s;

	if (fflag) return 0;
	dep = pathdep(target);
	if ((fd = c_nix_fdopen2(dep, C_NIX_OREAD)) < 0) return exist(target);
	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);
	c_mem_set(&arr, sizeof(arr), 0);
	ok = 1;
	while (ok && getln(&ioq, &arr)) {
		c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar));
		s = c_arr_data(&arr);
		switch (*s) {
		case '-': /* ifcreate */
			if (exist(s+1)) ok = 0;
			break;
		case '=': /* check */
			/* TYPE(+0),MTIME(+1),SIZE(+10),INODE(+27),
			 * MODE(+44),UID(+53),GID(+62),NAME(+71) */
			if ((fd = c_nix_fdopen2(s+71, C_NIX_OREAD)) < 0) {
				ok = 0;
				c_arr_trunc(&arr, 0, sizeof(uchar));
				continue;
			}
			if (modified(fd, s)) {
				ok = 0;
			} else {
				if (c_str_cmp(target, -1, s+71)) {
				if (C_STR_SCMP(".do",
				    s+((c_arr_bytes(&arr)))-3))
					ok = depcheck(s+71);
				}
			}
			c_nix_fdclose(fd);
			break;
		case '+': /* target must exist */
			if (!exist(s+1)) ok = 0;
			break;
		case '!': /* always */
			/* XXX: once per run */
		default:
			ok = 0;
		}
		c_arr_trunc(&arr, 0, sizeof(uchar));
	}
	c_dyn_free(&arr);
	c_nix_fdclose(c_ioq_fileno(&ioq));
	return ok;
}

static void
depwrite(ctype_fd fd, char *dep)
{
	static int first = 1;
	ctype_stat st;
	if (fd < 0) return;
	c_nix_stat(&st, dep);
	if (first) {
		c_fmt_install('a', &hex32);
		c_fmt_install('b', &hex64);
		first = 0;
	}
	fdfmt(fd, "=%a %b %b %a %a %a %s\n",
	    st.mtim.sec, st.size, st.ino, st.mode, st.uid, st.gid, dep);
}

/* do routines */
static ctype_status
always(ctype_fd fd)
{
	return fdfmt(fd, "!\n");
}

static char **
whichdo(char *target)
{
	static ctype_arr arr;
	usize len;
	char *ext, *s, *tmp;
	void **ptr;
	target = tmp = strfmt("%s", target);
	ext = c_str_chr(c_gen_basename(tmp), -1, '.');
	c_arr_trunc(&arr, 0, sizeof(uchar));
	ptr = dynalloc(&arr, (len = 0), sizeof(void *));
	*ptr = strfmt("%s.do", tmp);
	for (;;) {
		tmp = c_gen_dirname(tmp);
		s = ext;
		while (s) {
			ptr = dynalloc(&arr, ++len, sizeof(void *));
			*ptr = strfmt("%s/default%s.do", tmp, s);
			s = c_str_chr(s + 1, -1, '.');
		}
		ptr = dynalloc(&arr, ++len, sizeof(void *));
		*ptr = strfmt("%s/default.do", tmp);
		if (!c_str_cmp(tmp, -1, redo_rootdir)) break;
	}
	c_std_free(target);
	ptr = dynalloc(&arr, ++len, sizeof(void *));
	*ptr = nil;
	return c_arr_data(&arr);
}

static ctype_status
ifcreate(ctype_fd fd, char *s)
{
	return fdfmt(fd, "-%s\n", s);
}

static int
rundo(char *dofile, char *dir, char *target)
{
	ctype_arr arr;
	ctype_stat st;
	char out[C_LIM_PATHMAX];
	char **args;
	/* redirection */
	c_arr_init(&arr, out, sizeof(out));
	arrfmt(&arr, "%s/%s", dir, TMPFILE);
	c_nix_fdclose(mktemp(out, c_arr_bytes(&arr), C_NIX_OTMPANON)); /* $3 */
	/* env */
	setenv(REDO_TARGET, target);
	setnum(REDO_DEPTH, redo_depth + 1);
	/* exec */
	args = getargs(dofile, target, out);
	execout(args);
	c_std_free(args);
	/* target */
	if (exist(out)) {
		c_nix_rename(target, out);
		return 1;
	}
	return 0;
}

static ctype_status
ifchange(char *target)
{
	ctype_arr arr;
	ctype_fd depfd;
	ctype_status r;
	int depth;
	char deptmp[C_LIM_PATHMAX];
	char *dep, *dir, *file;
	char **s;

	if (depcheck(target)) return 0;
	dep = pathdep(target);

	c_arr_init(&arr, deptmp, sizeof(deptmp));
	arrfmt(&arr, "%s/.redo/%s", redo_rootdir, TMPFILE);
	depfd = mktemp(deptmp, sizeof(deptmp), 0);
	setnum(REDO_DEPFD, depfd);

	s = whichdo(target);
	for (; *s; ++s) {
		if (exist(*s)) break;
		ifcreate(depfd, *s); /* parent */
		c_std_free(*s);
	}
	if (!*s) {
		if (exist(target)) {
			c_nix_unlink(dep);
			goto next;
		} else {
			c_err_diex(1, "%s: no .do file.\n",
			    pathshrink(target));
		}
	}
	depth = redo_depth << 1;
	c_err_warnx("%*.*s %s",
	    depth, depth, " ", pathshrink(target));

	dir = dirname(target);
	file = c_gen_basename(target);
	if (rundo(*s, dir, file)) fdfmt(depfd, "+%s\n", target);
	depwrite(depfd, *s);
	c_nix_rename(dep, deptmp);
	for (; *s; ++s) c_std_free(*s);
next:
	c_std_free(s);
	c_nix_fdclose(depfd);
	c_nix_unlink(deptmp);
	return 0;
}

static void
targets(char *dep, usize n)
{
	dep[(n -= 4)] = 0; /* strip ".dep" */
	dep += redo_rootdir_len + 6; /* strip db path $PWD/.redo/ */
	if (!fflag && depcheck(dep)) return;
	if (exist(dep)) c_ioq_fmt(ioq1, "%s\n", dep);
}

static void
sources(char *dep, usize n)
{
	static ctype_kvtree t; /* "memory leak" */
	static ctype_arr arr; /* "memory leak" */
	ctype_fd fd;
	ctype_ioq ioq;
	char *s;
	char buf[C_IOQ_BSIZ];
	(void)n;
	fd = fdopen2(dep, C_NIX_OREAD);
	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);
	c_arr_trunc(&arr, 0, sizeof(uchar));
	while (getln(&ioq, &arr) > 0) {
		c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar));
		s = c_arr_data(&arr);
		if (*s != '=') goto next;
		s = s + 71;
		if (exist(s) && !exist(pathdep(s))) {
			if (c_adt_kvadd(&t, s, nil) == 1) goto next;
			c_ioq_fmt(ioq1, "%s\n", s);
		}
next:
		c_arr_trunc(&arr, 0, sizeof(uchar));
	}
	c_nix_fdclose(fd);
}

static void
walkdeps(void (*func)(char *, usize), char *s)
{
	ctype_dir dir;
	ctype_dent *p;
	char *args[2];
	args[0] = s;
	args[1] = nil;
	if (c_dir_open(&dir, args, 0, nil) < 0) {
		c_err_die(1, "failed to open directory \"%s\"", s);
	}
	while ((p = c_dir_read(&dir))) {
		if (p->info == C_DIR_FSF) func(p->path, p->len);
	}
	c_dir_close(&dir);
}

/* usage routines */
static void
default1_usage(void)
{
	c_ioq_fmt(ioq2, "usage: %s [path ...]\n", c_std_getprogname());
	c_std_exit(1);
}

static void
default2_usage(void)
{
	c_ioq_fmt(ioq2, "usage: %s\n", c_std_getprogname());
	c_std_exit(1);
}

static void
noargs(int argc, char **argv)
{
	if (c_std_noopt(argmain, *argv)) default2_usage();
	argc -= argmain->idx;
	if (argc) default2_usage();
}

static void
redo_usage(void)
{
	c_ioq_fmt(ioq2, "usage: %s [-j jobs] [-x] [target ...]\n",
	    c_std_getprogname());
	c_std_exit(1);
}

static void
whichdo_usage(void)
{
	c_ioq_fmt(ioq2, "usage: %s [target]\n", c_std_getprogname());
	c_std_exit(1);
}

/* main routines */
static ctype_status
redo_ifcreate(int argc, char **argv)
{
	ctype_status r;
	(void)argc;
	if (c_std_noopt(argmain, *argv)) default1_usage();
	argv += argmain->idx;
	r = 0;
	for (; *argv; ++argv) {
		*argv = toabs(*argv);
		if (exist(*argv)) r = 1;
		ifcreate(redo_depfd, *argv);
		c_std_free(*argv);
	}
	return r;
}

/* XXX: deal with signals */
static ctype_status
redo_ifchange(int argc, char **argv)
{
	ctype_status r;
	char *dir;
	if (!argc) return 0;
	c_std_atexit(cleantrash);
	r = 0;
	for (; *argv; ++argv) {
		dir = dirname((*argv = toabs(*argv)));
		c_exc_setenv("PWD", dir);
		c_nix_chdir(dir);
		r |= ifchange(*argv);
		if (!fflag) depwrite(redo_depfd, *argv);
		c_std_free(*argv);
	}
	return r;
}

static ctype_status
redo_whichdo(int argc, char **argv)
{
	char **s;
	if (c_std_noopt(argmain, *argv)) whichdo_usage();
	argc -= argmain->idx;
	argv += argmain->idx;
	if (argc - 1) whichdo_usage();
	*argv = toabs(*argv);
	s = whichdo(*argv);
	c_std_free(*argv);
	for (; *s; ++s) {
		c_ioq_fmt(ioq1, "%s\n", pathshrink(*s));
		if (exist(*s)) break;
		c_std_free(*s);
	}
	c_ioq_flush(ioq1);
	for (; *s; ++s) c_std_free(*s);
	c_std_free(s);
	return 0;
}

static ctype_status
redo(int argc, char **argv)
{
	char tmp[] = "all";
	char *args[] = { tmp, nil };

	while (c_std_getopt(argmain, argc, argv, "j:x")) {
		switch (argmain->opt) {
		case 'j': /* XXX */
			break;
		case 'x':
			setnum(REDO_XFLAG, (xflag = 1));
			break;
		default:
			redo_usage();
		}
	}
	argc -= argmain->idx;
	argv += argmain->idx;

	if (!argc) {
		argc = 1;
		argv = args;
	}

	setenv(REDO_ROOTDIR, pwd);
	fflag = 1;
	return redo_ifchange(argc, argv);
}

ctype_status
main(int argc, char **argv)
{
	char *prog, *s;

	c_std_setprogname((prog = argv[0]));
	--argc, ++argv;

	pwd = getpwd();
	/* prepare environment */
	redo_depfd = getnum(REDO_DEPFD);
	if ((redo_depth = getnum(REDO_DEPTH)) < 0) redo_depth = 0;
	if (!(redo_rootdir = c_std_getenv(REDO_ROOTDIR))) redo_rootdir = pwd;
	redo_rootdir_len = c_str_len(redo_rootdir, -1);
	/* catch flags */
	if ((xflag = getnum(REDO_XFLAG)) < 0) xflag = 0;

	/* main routines */
	s = getdbpath();
	prog = c_gen_basename(prog);
	if (!C_STR_SCMP("redo", prog)) {
		mkpath(s, 0777, 0777);
		return redo(argc, argv);
	} else if (!C_STR_SCMP("redo-always", prog)) {
		checkparent();
		noargs(argc, argv);
		always(redo_depfd);
	} else if (!C_STR_SCMP("redo-ifchange", prog)) {
		checkparent();
		if (c_std_noopt(argmain, *argv)) default1_usage();
		argv += argmain->idx;
		return redo_ifchange(argc, argv);
	} else if (!C_STR_SCMP("redo-ifcreate", prog)) {
		checkparent();
		return redo_ifcreate(argc, argv);
	} else if (!C_STR_SCMP("redo-ood", prog)) {
		noargs(argc, argv);
		walkdeps(targets, s);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-sources", prog)) {
		noargs(argc, argv);
		walkdeps(sources, s);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-stamp", prog)) {
		/* DUMMY */
		noargs(argc, argv);
		return 0;
	} else if (!C_STR_SCMP("redo-targets", prog)) {
		noargs(argc, argv);
		fflag = 1;
		walkdeps(targets, s);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-whichdo", prog)) {
		return redo_whichdo(argc, argv);
	} else {
		c_err_die(1, "invalid progname %s", prog);
	}
	return 0;
}
