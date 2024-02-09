/* TODO:
 * 0. Once API is completely set, refactor heavily (if not rewrite)
 * 1. Fix redo(-if*) race condition on writing deps across different processes
 */
#include <tertium/cpu.h>
#include <tertium/std.h>

#define USAGE_REDO " [-j jobs] [-x] [target ...]"
#define USAGE_DEF0 " [path]"
#define USAGE_DEF1 " [path ...]"
#define USAGE_DEF2 ""

#define arglist(...) arglist_(__VA_ARGS__, nil)
#define noopt(argc, argv, func) { \
 if (c_std_noopt(argmain, *(argv))) (func); \
 argc -= argmain->idx; \
 argv += argmain->idx; \
}

#define TMPBASE ".tmpfile."
#define TMPFILE ".tmpfile.XXXXXXXXX"

#define REDO_XFLAG "_TERTIUM_REDO_XFLAG"
#define REDO_ROOTDIR "_TERTIUM_REDO_ROOTDIR"
#define REDO_ROOTFD "_TERTIUM_REDO_ROOTFD"
#define REDO_TARGET "_TERTIUM_REDO_TARGET"
#define REDO_DEPFD "_TERTIUM_REDO_DEPFD"
#define REDO_DEPTH "_TERTIUM_REDO_DEPTH"

#define HDEC(x) ((x <= '9') ? x - '0' : (((uchar)x | 32) - 'a') + 10)

static ctype_node *tmpfiles;

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

static ctype_status
recdel1(char *s)
{
	char *args[2];
	args[0] = s;
	args[1] = nil;
	return recdel(args);
}

static void
cleantrash(void)
{
	ctype_node *p;
	usize len;
	char **args;

	if (!tmpfiles) return;
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
hex(ctype_fmt *p)
{
	int len, i;
	uchar *s;
	len = va_arg(p->args, int);
	s = va_arg(p->args, uchar*);
	for (i = 0; i < len; ++i) c_fmt_print(p, "%02x", s[i]);
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
		c_err_die(1, "failed to generate path");
	}
	va_end(ap);
}

static ctype_fd
fdopen2(char *s, uint opts)
{
	ctype_fd fd;
	if ((fd = c_nix_fdopen2(s, opts)) < 0) {
		c_err_die(1, "failed to open \"%s\"", s);
	}
	return fd;
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
getln(ctype_arr *ap, ctype_ioq *p)
{
	size r;
	if ((r = c_ioq_getln(ap, p)) < 0) c_err_diex(1, nil);
	return r;
}

static void
mkpath(char *s, uint mode, uint dmode)
{
	if (c_nix_mkpath(s, mode, dmode) < 0) {
		c_err_die(1, "failed to generate path %s", s);
	}
}

static ctype_fd
mktemp(char *s, usize n, uint opts)
{
	ctype_fd fd;
	fd = c_nix_mktemp5(s, n, opts, C_NIX_OAPPEND, 0644);
	if (fd < 0) c_err_die(1, "failed to obtain temporary file \"%s\"", s);
	trackfile(s, n);
	return fd;
}

static usize
strfmt(char **sp, char *fmt, ...)
{
	va_list ap;
	size len;
	va_start(ap, fmt);
	if ((len = c_str_vfmt(sp, fmt, ap)) < 0) c_err_diex(1, nil);
	va_end(ap);
	return (usize)len;
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
	c_str_cpy(buf, sizeof(buf), s);
	return c_gen_dirname(buf);
}

static ctype_status
exist(char *s)
{
	ctype_stat st;
	if (c_nix_lstat(&st, s) < 0) {
		if (errno == C_ERR_ENOENT) return 0;
		c_err_die(1, "failed to obtain path info \"%s\"", s);
	}
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
	usize n;
	char buf[C_LIM_PATHMAX];
	char *tmp;

	c_arr_init(&arr, buf, sizeof(buf));
	if (s[0] == '/') {
		tmp = dirname(s);
	} else {
		arrfmt(&arr, "%s/%s", pwd, s);
		tmp = c_gen_dirname(c_arr_data(&arr));
	}
	n = strfmt(&s, "%s/%s", tmp, c_gen_basename(s));
	return c_nix_normalizepath(s, n);
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
	/* check executability */
	fd = fdopen2(dofile, C_NIX_OREAD);
	isexec = (c_nix_fdstat(&st, fd) == 0) && (st.mode & C_NIX_IXUSR);
	/* get first line */
	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);
	c_arr_trunc(&arr, 0, sizeof(uchar));
	if (!getln(&arr, &ioq)) goto shfallback;
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

static int
cmp(void *va, void *vb)
{
	ctype_dent *a, *b;
	a = va;
	b = vb;
	return c_str_cmp(a->name, C_STD_MIN(a->nlen, b->nlen) + 1, b->name);
}

static char *
linkname(char *s, ctype_stat *p)
{
	static char buf[C_LIM_PATHMAX];
	ctype_stat st;

	if (!p) {
		p = &st;
		if (c_nix_lstat(&st, s) < 0) {
			c_err_die(1, "failed to obtain path info \"%s\"", s);
		}
	}

	if (!C_NIX_ISLNK(p->mode)) return nil;

	if (c_nix_readlink(buf, sizeof(buf), s) < 0) {
		c_err_die(1, "failed to read symlink \"%s\"", s);
	}
	return buf;
}

static void
hashfd(ctype_hst *h, char *s, ctype_stat *sp)
{
	ctype_dir dir;
	ctype_dent *p;
	usize n;
	char *args[2];

	/* avoid path strip below */
	if (C_NIX_ISLNK(sp->mode)) {
		s = linkname(s, sp);
		c_hsh_md5->update(h, s, c_str_len(s, -1));
		return;
	} else if (!C_NIX_ISDIR(sp->mode)) {
		c_hsh_putfile(h, c_hsh_md5, s);
		return;
	}

	/* strip path for hash reproducibility */
	n = c_str_len(s, -1);
	if (s[n - 1] != '/') ++n;

	args[0] = s;
	args[1] = nil;
	if (c_dir_open(&dir, args, 0, &cmp) < 0) {
		c_err_die(1, "failed to open \"%s\" for hashing", s);
	}
	while ((p = c_dir_read(&dir))) {
		switch (p->info) {
		case C_DIR_FSD:
		case C_DIR_FSDC:
		case C_DIR_FSDP:
			continue;
		case C_DIR_FSDNR:
		case C_DIR_FSNS:
		case C_DIR_FSERR:
			/* XXX */
			continue;
		}
		c_hsh_md5->update(h, p->path + n, p->len - n);
		switch (p->info) {
		case C_DIR_FSSL:
		case C_DIR_FSSLN:
			s = linkname(p->path, p->stp);
			c_hsh_md5->update(h, s, c_str_len(s, -1));
			break;
		default:
			c_hsh_putfile(h, c_hsh_md5, p->path);
		}
	}
	c_dir_close(&dir);
}

/* Thank the WOP (workaround oriented programming) APIs for the drama
 * to replace a dir atomically:
 * 1. symlink:
 * problem = dir is not a dir anymore, need to track orphans
 * pseudo =
 * l=$(readlink cur)
 * ln -s new tmp
 * rename(tmp, cur)
 * rm -Rf $l
 * 2. mount black magic:
 * problem = requires privileges
 * pseudo =
 * mount -o bind,rw cur tmp
 * mount -o bind,ro new cur
 * rm -Rf tmp/*
 * cp -R new/. tmp
 * umount cur tmp
 * 3. renameat2:
 * problem = not reliable nor portable
 */
static ctype_status
replace(char *d, char *s)
{
	ctype_stat sta, stb;
	ctype_status r;
	char *l;
	char sym[sizeof(TMPFILE)];
	char tmp[sizeof(TMPFILE)];

	if (c_nix_lstat(&sta, s) < 0) {
		c_err_die(1, "failed to obtain path info \"%s\"", d);
	}
	r = 1;
	if (C_NIX_ISDIR(sta.mode)) {
		c_str_cpy(sym, sizeof(sym), TMPFILE);
		c_nix_fdclose(c_nix_mktemp3(sym, sizeof(sym), C_NIX_OTMPANON));
		if (c_nix_symlink(sym, s) < 0) {
			c_err_die(1,
			    "failed to create symlink from \"%s\" to \"%s\"",
			    sym, s);
		}
		s = sym;
		++r;
	}
	if (c_nix_lstat(&sta, d) < 0) {
		if (errno == C_ERR_ENOENT) goto ret;
		c_err_die(1, "failed to obtain path info \"%s\"", d);
	}
	if (C_NIX_ISDIR(sta.mode)) {
		/* not atomic just the best that can be done */
		c_str_cpy(tmp, sizeof(tmp), TMPFILE);
		c_nix_fdclose(c_nix_mktemp3(tmp, sizeof(tmp), C_NIX_OTMPANON));
		if (c_nix_rename(tmp, d) < 0) return -1;
		if (c_nix_rename(d, s) < 0) {
			c_nix_rename(d, tmp); /* try to undo */
			return -1;
		}
		recdel1(tmp);
		return r;
	} else if (C_NIX_ISLNK(sta.mode)) {
		if (c_nix_stat(&stb, d) < 0) {
			if (errno == C_ERR_ENOENT) goto ret;
			c_err_die(1, "failed to obtain path info \"%s\"", d);
		}
		if (C_NIX_ISDIR(stb.mode)) {
			l = c_gen_basename(linkname(d, &sta));
			if (!C_STR_CMP(TMPBASE, l)) {
				if (c_nix_rename(d, s) < 0) return -1;
				recdel1(l);
				return r;
			}
		}
	}
ret:
	if (c_nix_rename(d, s) < 0) return -1;
	return r;
}

/* deps routines */
static int
modified(char *s)
{
	ctype_hst h;
	ctype_stat st;
	ctype_taia t;
	char buf[C_HSH_MD5DIG];

	if (c_nix_lstat(&st, s + 67) < 0) {
		if (errno == C_ERR_ENOENT) return 0;
		c_err_die(1, "failed to obtain path info \"%s\"", s + 67);
	}
	/* time stamp */
	c_taia_fromtime(&t, &st.ctim);
	c_taia_pack(buf, &t);
	if (!hexcmp(s + 1, C_TAIA_PACK, buf)) return 2;
	/* hash */
	c_hsh_md5->init(&h);
	hashfd(&h, s + 67, &st);
	c_hsh_md5->end(&h, buf);
	if (!hexcmp(s + 34, C_HSH_MD5DIG, buf)) return 1;
	return 0;
}

static char *
pathdep(char *target)
{
	static char buf[C_LIM_PATHMAX];
	ctype_arr arr;
	c_arr_init(&arr, buf, sizeof(buf));
	arrfmt(&arr, "%s/.redo%s.dep", redo_rootdir, target);
	target = c_arr_data(&arr);
	mkpath(dirname(target), 0755, 0755);
	return target;
}

static int
rebuild(char *dep, char *target)
{
	ctype_stat st;
	ctype_taia time;
	ctype_ioq ioq;
	ctype_arr arr;
	ctype_fd depfd, fd;
	usize len;
	char deptmp[C_LIM_PATHMAX];
	char buf[C_IOQ_SMALLBSIZ];
	char tm[C_TAIA_PACK];
	char *s;

	if ((depfd = c_nix_fdopen2(dep, C_NIX_OREAD)) < 0) return 1;

	c_arr_init(&arr, deptmp, sizeof(deptmp));
	arrfmt(&arr, "%s/.redo/" TMPFILE, redo_rootdir);
	fd = mktemp(deptmp, c_arr_bytes(&arr), 0);

	c_mem_set(&arr, sizeof(arr), 0);
	c_ioq_init(&ioq, depfd, buf, sizeof(buf), &c_nix_fdread);
	while (getln(&arr, &ioq)) {
		len = c_arr_bytes(&arr);
		c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar));
		s = c_arr_data(&arr);
		switch (*s) {
		case '=': /* check */
			/* TYPE(+0),TAIA(+1),MD5(+34),NAME(+67) */
			if (!c_str_cmp(s + 67, len - 67, target)) {
				if (c_nix_stat(&st, s + 67) < 0) return 0;
				c_taia_fromtime(&time, &st.mtim);
				c_taia_pack(tm, &time);
				fdfmt(fd, "%c%H%s\n",
				    *s, sizeof(tm), tm, s + 33);
				break;
			}
			/* FALLTHROUGH */
		default:
			fdfmt(fd, "%s\n", s);
		}
		c_arr_trunc(&arr, 0, sizeof(uchar));
	}
	c_dyn_free(&arr);
	c_nix_fdclose(depfd);
	c_nix_fdclose(fd);
	if (c_nix_rename(dep, deptmp) < 0) return 0;
	return 1;
}

static int
depcheck(char *target)
{
	ctype_ioq ioq;
	ctype_arr arr;
	ctype_fd fd;
	int ok;
	char buf[C_IOQ_SMALLBSIZ];
	char *dep, *s;

	if (fflag) return 0;

	dep = pathdep(target);
	if ((fd = c_nix_fdopen2(dep, C_NIX_OREAD)) < 0) return exist(target);

	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);
	c_mem_set(&arr, sizeof(arr), 0);
	ok = 1;
	while (ok && getln(&arr, &ioq)) {
		c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar));
		s = c_arr_data(&arr);
		switch (*s) {
		case '-': /* ifcreate */
			ok = !exist(s + 1);
			break;
		case '=': /* check */
			/* TYPE(+0),TAIA(+1),MD5(+34),NAME(+67) */
			ok = modified(s);
			switch (ok) {
			case 0:
				ok = 0;
				break;
			case 1:
				if (!(ok = rebuild(pathdep(target), s + 67))) {
					break;
				}
				/* FALLTHROUGH */
			case 2:
				if (c_str_cmp(target, -1, s + 67)) {
					if (C_STR_SCMP(".do",
					    s + ((c_arr_bytes(&arr))) - 3)) {
						ok = depcheck(s + 67);
					}
				}
			}
			break;
		case '+': /* target must exist */
			ok = exist(s + 1);
			break;
		case '@': /* meta dep */
			ok = depcheck(s + 1);
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
	ctype_hst h;
	ctype_taia time;
	ctype_stat st;
	char hash[C_HSH_MD5DIG];
	char tm[C_TAIA_PACK];

	if (fd < 0) return;
	if (c_nix_lstat(&st, dep) < 0) c_err_die(1, nil);
	/* timestamp */
	c_taia_fromtime(&time, &st.ctim);
	c_taia_pack(tm, &time);
	/* hash */
	c_hsh_md5->init(&h);
	hashfd(&h, dep, &st);
	c_hsh_md5->end(&h, hash);
	/* write */
	fdfmt(fd, "=%H %H %s\n", sizeof(tm), tm, sizeof(hash), hash, dep);
}

/* do routines */
static ctype_status
always(ctype_fd fd)
{
	return fdfmt(fd, "!\n");
}

static void *
whichdo(char *target)
{
	static ctype_arr arr;
	usize len;
	char *ext, *s, *tmp;
	char **ptr;
	strfmt(&tmp, "%s", target);
	target = tmp;
	ext = c_str_chr(c_gen_basename(tmp), -1, '.');
	c_arr_trunc(&arr, 0, sizeof(uchar));
	ptr = dynalloc(&arr, (len = 0), sizeof(void *));
	strfmt(ptr, "%s.do", tmp);
	for (;;) {
		tmp = c_gen_dirname(tmp);
		s = ext;
		while (s) {
			ptr = dynalloc(&arr, ++len, sizeof(void *));
			strfmt(ptr, "%s/default%s.do", tmp, s);
			s = c_str_chr(s + 1, -1, '.');
		}
		ptr = dynalloc(&arr, ++len, sizeof(void *));
		strfmt(ptr, "%s/default.do", tmp);
		if (!c_str_cmp(tmp, -1, redo_rootdir)) break;
		if (!c_str_cmp(tmp, -1, "/")) break;
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
	ctype_status r;
	char out[C_LIM_PATHMAX];
	char **args;
	/* redirection */
	c_arr_init(&arr, out, sizeof(out));
	arrfmt(&arr, "%s/" TMPFILE, dir);
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
		if ((r = replace(target, out)) < 0) {
			c_err_die(1, "failed to replace %s", target);
		}
		return r;
	}
	return 0;
}

static ctype_status
ifchange(char *target)
{
	ctype_stat st;
	ctype_arr arr;
	ctype_fd depfd;
	ctype_status r;
	usize len;
	int depth;
	char deptmp[C_LIM_PATHMAX];
	char *dep, *dir, *file;
	char **s;

	if (depcheck(target)) return 0;
	dep = pathdep(target);

	c_arr_init(&arr, deptmp, sizeof(deptmp));
	arrfmt(&arr, "%s/.redo/" TMPFILE, redo_rootdir);
	depfd = mktemp(deptmp, c_arr_bytes(&arr), 0);
	setnum(REDO_DEPFD, depfd);

	s = whichdo(target);
	for (; *s; ++s) {
		if (exist(*s)) break;
		ifcreate(depfd, *s); /* parent */
		c_std_free(*s);
	}
	if (!*s) {
		if (!c_nix_lstat(&st, target)) {
			c_nix_unlink(dep);
			if (C_NIX_ISDIR(st.mode)) {
				len = c_str_len(dep, -1);
				c_str_cpy(dep + (len - 4), len, ".src");
				c_nix_fdclose(mktemp(dep, len, 0));
			}
			goto next;
		} else {
			c_err_diex(1, "%s: no .do file.\n",
			    pathshrink(target));
		}
	} else {
		len = c_str_len(dep, -1);
		c_str_cpy(dep + (len - 4), len, ".src");
		c_nix_unlink(dep);
		c_str_cpy(dep + (len - 4), len, ".dep");
	}
	depth = redo_depth << 1;

	c_err_warnx("%*.*s %s",
	    depth, depth, " ", pathshrink(target));

	dir = dirname(target);
	file = c_gen_basename(target);
	if ((r = rundo(*s, dir, file))) {
		/* if "dir"(sym) add a slash so it's treated as a dir */
		fdfmt(depfd, "+%s%s\n", target, (r == 1) ? "" : "/");
	}
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
sources(char *dep, usize n, char *file)
{
	static ctype_kvtree t;
	static ctype_arr arr;
	ctype_ioq ioq;
	ctype_fd fd;
	char buf[C_IOQ_BSIZ];
	char *s;

	if (file) file = toabs(file);
	(void)n;
	fd = fdopen2(dep, C_NIX_OREAD);
	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);

	c_arr_trunc(&arr, 0, sizeof(uchar));
	while (getln(&arr, &ioq)) {
		c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar));
		s = c_arr_data(&arr);
		if (*s != '=') goto next;
		s = s + 67;
		if (fflag) {
			if (c_str_cmp(file, n, s)) goto next;
			dep[n - 4] = 0; /* strip ".dep" */
			dep = dep + redo_rootdir_len + 6; /* strip db path */
			c_ioq_fmt(ioq1, "%s\n", dep);
			break;
		} else {
			if (c_adt_kvadd(&t, s, nil) == 1) goto next;
			c_ioq_fmt(ioq1, "%s\n", s);
			if (file) {
				s = pathdep(s);
				if (exist(s)) {
					sources(s, c_str_len(s, -1), file);
				}
			}
		}
next:
		c_arr_trunc(&arr, 0, sizeof(uchar));
	}
	c_nix_fdclose(fd);
	c_std_free(file);
}

static void
sourcefile(char *dep, usize n, char *file)
{
	if (file) {
		file = toabs(file);
		if (c_str_cmp(dep, n, pathdep(file))) {
			c_std_free(file);
			return;
		}
		c_std_free(file);
	}
	sources(dep, n, nil);
}

static void
targets(char *dep, usize n, char *file)
{
	if (file && fflag) {
		sources(dep, n, file);
		return;
	}

	dep[n - 4] = 0; /* strip ".dep" */
	dep = dep + redo_rootdir_len + 6; /* strip db path */
	if (fflag) {
		if (!exist(dep)) return;
	} else if (depcheck(dep)) {
		return;
	}
	c_ioq_fmt(ioq1, "%s\n", dep);
}

static void
walkdeps(void (*func)(char *, usize, char *), char *s)
{
	ctype_dir dir;
	ctype_dent *p;
	ctype_stat st;
	usize len;
	char *args[2], *dep;

	args[0] = getdbpath();
	args[1] = nil;
	if (s) {
		/* tmp */
		args[1] = toabs(s);
		dep = pathdep(args[1]);
		c_std_free(args[1]);
		/* is it target? */
		if (!exist(dep)) {
			if (!c_nix_stat(&st, s)) {
				if (C_NIX_ISDIR(st.mode)) {
					len = c_str_len(dep, -1);
					c_str_cpy(dep + (len - 4), len, ".src");
					/* is it filter? */
					if (!exist(dep)) {
						/* strip .dep */
						dep[len - 4] = 0;
						args[0] = dep;
						s = nil;
					}
				}
				/* it is source */
			} else {
				c_err_diex(1,
				    "\"%s\" is not a dir/source/target", s);
			}
		}
	}

	if (c_dir_open(&dir, args, 0, nil) < 0) {
		c_err_die(1, "failed to open directory \"%s\"", *args);
	}
	while ((p = c_dir_read(&dir))) {
		if (p->info == C_DIR_FSF &&
		    !C_STR_SCMP(".dep", p->path + (p->len - 4))) {
			func(p->path, p->len, s);
		}
	}
	c_dir_close(&dir);
}

static void
dbsync(void)
{
	ctype_dir dir;
	ctype_dent *p;
	ctype_arr arr;
	usize len;
	char *args[2];

	c_mem_set(&arr, sizeof(arr), 0);
	dynfmt(&arr, "%s", redo_rootdir);

	args[0] = getdbpath();
	args[1] = nil;
	if (c_dir_open(&dir, args, 0, nil) < 0) {
		c_err_die(1, "failed to open directory \"%s\"", *args);
	}
	len = c_str_len(*args, -1);
	while ((p = c_dir_read(&dir))) {
		dynfmt(&arr, "%.*s", p->len - len, p->path + len);
		switch (p->info) {
		case C_DIR_FSF:
			/* strip ".dep" */
			c_arr_trunc(&arr, c_arr_bytes(&arr) - 4, sizeof(uchar));
			if (!exist(c_arr_data(&arr))) c_nix_unlink(p->path);
		case C_DIR_FSD:
			break;
		case C_DIR_FSDP:
			c_nix_rmdir(p->path);
		}
		c_arr_trunc(&arr, redo_rootdir_len, sizeof(uchar));
	}
	c_dir_close(&dir);
	c_dyn_free(&arr);
}

/* usage routines */
static void
usage(char *msg)
{
	c_ioq_fmt(ioq2, "usage: %s%s\n", c_std_getprogname(), msg);
	c_std_exit(1);
}

/* main routines */
static ctype_status
redo_ifcreate(int argc, char **argv)
{
	ctype_status r;
	(void)argc;
	if (c_std_noopt(argmain, *argv)) usage(USAGE_DEF1);
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
		mkpath(dir, 0755, 0755);
		c_exc_setenv("PWD", dir);
		c_nix_chdir(dir);
		r |= ifchange(*argv);
		if (!fflag) {
			if (exist(*argv)) {
				depwrite(redo_depfd, *argv);
			} else {
				fdfmt(redo_depfd, "@%s\n", *argv);
			}
		}
		c_std_free(*argv);
	}
	return r;
}

static ctype_status
redo_whichdo(int argc, char **argv)
{
	char **s;
	if (c_std_noopt(argmain, *argv)) usage(USAGE_DEF0);
	argc -= argmain->idx;
	argv += argmain->idx;
	if (argc - 1) usage(USAGE_DEF0);
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
			usage(USAGE_REDO);
		}
	}
	argc -= argmain->idx;
	argv += argmain->idx;

	if (!argc) {
		argc = 1;
		argv = args;
	}
	return redo_ifchange(argc, argv);
}

ctype_status
main(int argc, char **argv)
{
	char *prog;

	c_std_setprogname((prog = argv[0]));
	--argc, ++argv;

	pwd = getpwd();
	/* prepare environment */
	redo_depfd = getnum(REDO_DEPFD);
	if ((redo_depth = getnum(REDO_DEPTH)) < 0) redo_depth = 0;
	/* main process? */
	if (!(redo_rootdir = c_std_getenv(REDO_ROOTDIR))) {
		setenv(REDO_ROOTDIR, pwd);
		redo_rootdir = pwd;
		dbsync();
	}
	redo_rootdir_len = c_str_len(redo_rootdir, -1);
	/* catch flags */
	if ((xflag = getnum(REDO_XFLAG)) < 0) xflag = 0;

	/* main routines */
	c_fmt_install('H', &hex);
	prog = c_gen_basename(prog);
	if (!C_STR_SCMP("redo", prog)) {
		mkpath(getdbpath(), 0755, 0755);
		fflag = 1;
		return redo(argc, argv);
	} else if (!C_STR_SCMP("redo-always", prog)) {
		checkparent();
		noopt(argc, argv, usage(USAGE_DEF2));
		if (argc) usage(USAGE_DEF2);
		always(redo_depfd);
	} else if (!C_STR_SCMP("redo-ifchange", prog)) {
		if (redo_depfd == -1) return redo(argc, argv);
		noopt(argc, argv, usage(USAGE_DEF1))
		return redo_ifchange(argc, argv);
	} else if (!C_STR_SCMP("redo-ifcreate", prog)) {
		checkparent();
		return redo_ifcreate(argc, argv);
	} else if (!C_STR_SCMP("redo-ood", prog)) {
		noopt(argc, argv, usage(USAGE_DEF0));
		walkdeps(targets, *argv);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-sources", prog)) {
		noopt(argc, argv, usage(USAGE_DEF0));
		walkdeps(argc ? sourcefile : sources, *argv);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-stamp", prog)) {
		/* DUMMY */
		noopt(argc, argv, usage(USAGE_DEF2));
		if (argc) usage(USAGE_DEF2);
		return 0;
	} else if (!C_STR_SCMP("redo-targets", prog)) {
		noopt(argc, argv, usage(USAGE_DEF0));
		fflag = 1;
		walkdeps(targets, *argv);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-whichdo", prog)) {
		return redo_whichdo(argc, argv);
	} else {
		c_err_die(1, "invalid progname %s", prog);
	}
	return 0;
}
