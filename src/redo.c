#include <tertium/cpu.h>
#include <tertium/std.h>

enum {
	FFLAG = 1 << 0,
	XFLAG = 1 << 1,
};

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

#define TMPNAME ".eltaninos_redo_tmp"
#define TMPEXT  "XXXXXXXXX"
#define TMPBASE TMPNAME "."
#define TMPFILE TMPBASE TMPEXT

#define REDO_XFLAG "_ELTANINOS_REDO_XFLAG"
#define REDO_ROOTDIR "_ELTANINOS_REDO_ROOTDIR"
#define REDO_ROOTFD "_ELTANINOS_REDO_ROOTFD"
#define REDO_TARGET "_ELTANINOS_REDO_TARGET"
#define REDO_DEPFD "_ELTANINOS_REDO_DEPFD"
#define REDO_DEPTH "_ELTANINOS_REDO_DEPTH"

#define HDEC(x) ((x <= '9') ? x - '0' : (((uchar)x | 32) - 'a') + 10)

struct rebuild {
	ctype_fd fd;
	char *target;
};

struct sources {
	ctype_kvtree t;
	char *file;
};

/* variables */
static ctype_node *tmpfiles;
static usize rootdlen;
static char *pwd;
static char *rootdir;
static ctype_fd parentfd;
static int depth;

static uint opts;

/* func prototypes */
static int depcheck(char *);
static void sources(char *, usize, char *);

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
	while ((p = c_adt_lpop(&tmpfiles))) c_adt_lfree(p, c_std_free_);
}

static void
trackfile(char *s, usize n)
{
	if (c_adt_lpush(&tmpfiles, c_adt_lnew(s, n)) < 0) c_err_diex(1, nil);
}

/* fail routines */
static char **
arglist_(char *prog, ...)
{
	char **av;
	va_list ap;
	va_start(ap, prog);
	if (!(av = c_exc_vsplit(prog, ap))) c_err_diex(1, nil);
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

static void
dynfmt(ctype_arr *p, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (c_dyn_vfmt(p, fmt, ap) < 0) c_err_diex(1, nil);
	va_end(ap);
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

static void
setenv(char *k, char *v)
{
	if (c_exc_setenv(k, v) < 0) {
		c_err_die(1, "failed to set environ variable \"%s\"", k);
	}
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
	ctype_status err;
	int x;

	if (!(k = c_std_getenv(k))) return -1;

	err = 0;
	x = c_std_strtovl(k, 10,  C_LIM_INTMIN, C_LIM_INTMAX, nil, &err);
	if (err) return -1;
	return x;
}

static void
setnum(char *k, int x)
{
	ctype_arr arr;
	char buf[12];

	c_arr_init(&arr, buf, sizeof(buf));
	arrfmt(&arr, "%d", x);

	if (c_exc_setenv(k, c_arr_data(&arr)) < 0) {
		c_err_die(1, "failed to set environ variable");
	}
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


/* path routines */
static char *
absolute(char *s)
{
	static ctype_arr arr; /* "memory leak" */

	if (s[0] == '/') return c_nix_normalizepath(s, c_str_len(s, -1));

	c_arr_trunc(&arr, 0, sizeof(uchar));
	dynfmt(&arr, "%s/%s", pwd, s);
	return c_nix_normalizepath(c_arr_data(&arr), c_arr_bytes(&arr));
}

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
	c_str_cpy(buf, sizeof(buf), s);
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
		c_err_die(1, "failed to obtain info \"%s\"", s);
	}
	return 1;
}

static char *
getpwd(void)
{
	char buf[C_LIM_PATHMAX];
	ctype_stat pwd, dot;
	char *s;

	if (!(s = c_std_getenv("PWD"))) goto fallback;
	s = c_nix_normalizepath(c_str_dup(s, -1), -1);
	if (s[0] != '/') goto fallback;

	if (c_str_str(s, -1, "/../")) goto fallback;

	if (c_nix_stat(&pwd, s) < 0 || c_nix_stat(&dot, ".") < 0) goto fallback;
	if (pwd.dev == dot.dev && pwd.ino == dot.ino) return s;
fallback:
	c_std_free(s);
	if (!(s = c_nix_getcwd(buf, sizeof(buf)))) {
		c_err_die(1, "failed to obtain current dir");
	}
	return c_str_dup(s, -1);
}

static char *
pathshrink(char *s)
{
	if (c_str_cmp(s, rootdlen, rootdir)) return s;
	return s + rootdlen + 1;
}

/* db routines */
static char *
dbgetpath(char *s, char *ext)
{
	static char buf[C_LIM_PATHMAX];
	ctype_arr arr;

	c_arr_init(&arr, buf, sizeof(buf));
	arrfmt(&arr, "%s/.redo", rootdir);
	if (s) arrfmt(&arr, "/%s.%s", s, ext);

	s = c_arr_data(&arr);
	if (c_nix_mkpath(dirname(s), 0755, 0755) < 0) {
		c_err_die(1, "failed to generate path %s", s);
	}
	return s;
}

static void
lineabs(ctype_arr *ap, char *s)
{
	usize pos;

	switch (*s) {
	case '-':
	case '+':
	case '@':
		pos = 1;
		break;
	case '=':
		pos = 67;
		break;
	default:
		return;
	}

	if (c_dyn_idxcat(ap, pos, "/", 1, sizeof(uchar)) < 0) {
		c_err_die(1, nil);
	}
	if (c_dyn_idxcat(ap, pos, rootdir, rootdlen, sizeof(uchar)) < 0) {
		c_err_die(1, nil);
	}
}

static ctype_status
dbgetlines(char *s, ctype_status (*fn)(char *, char *, usize, void *), void *p)
{
	ctype_ioq ioq;
	ctype_arr arr;
	char buf[C_IOQ_SMALLBSIZ];
	size r;
	ctype_status ret;
	ctype_fd fd;

	fd = c_nix_fdopen2(dbgetpath(pathshrink(s), "dep"), C_NIX_OREAD);
	if (fd < 0) return -1;

	c_mem_set(&arr, sizeof(arr), 0);
	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);

	ret = 0;

	while ((r = c_ioq_getln(&arr, &ioq)) > 0) {
		c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar));
		lineabs(&arr, c_arr_data(&arr));
		if (fn(s, c_arr_data(&arr), c_arr_bytes(&arr), p)) {
			ret = 1;
			goto end;
		}
		c_arr_trunc(&arr, 0, sizeof(uchar));
	}

	if (r < 0) {
		c_err_die(1, "failed to read database \"%s\"", s);
	}
end:
	c_dyn_free(&arr);
	c_nix_fdclose(fd);
	return ret;
}

static void
sync(void)
{
	ctype_dir dir;
	ctype_dent *p;
	usize n;
	char *args[2];
	char *s;

	args[0] = dbgetpath(nil, nil);
	args[1] = nil;
	if (c_dir_open(&dir, args, 0, nil) < 0) {
		c_err_die(1, "failed to open directory \"%s\"", *args);
	}
	while ((p = c_dir_read(&dir))) {
		switch (p->info) {
		case C_DIR_FSF:
			s = p->name + (p->nlen - 4);
			if (!C_STR_SCMP(".dep", s)) {
				n = c_str_len((s = absolute(p->path)), -1);
				s[n - 4] = 0; /* strip .dep */
				if (!exist(s)) c_nix_unlink(p->path);
			} else if (!C_STR_SCMP(".src", s)) {
				n = c_str_len((s = absolute(p->path)), -1);
				c_str_cpy(s + (n - 4), 4, ".dep");
				if (exist(s)) c_nix_unlink(p->path);
			}
			p->parent->num++;
		case C_DIR_FSD:
			break;
		case C_DIR_FSDP:
			if (p->num) c_nix_rmdir(p->path);
		}
	}
	c_dir_close(&dir);
}

/* hash routines */
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
linkname(char *s)
{
	static char buf[C_LIM_PATHMAX];
	if (c_nix_readlink(buf, sizeof(buf), s) < 0) {
		if (errno == C_ERR_EINVAL) return nil;
		c_err_die(1, "failed to read symlink \"%s\"", s);
	}
	return buf;
}

static int
cmp(void *va, void *vb)
{
	ctype_dent *a, *b;
	a = va;
	b = vb;
	return c_str_cmp(a->name, C_STD_MIN(a->nlen, b->nlen) + 1, b->name);
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
		s = linkname(s);
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
			s = linkname(p->path);
			c_hsh_md5->update(h, s, c_str_len(s, -1));
			break;
		default:
			c_hsh_putfile(h, c_hsh_md5, p->path);
		}
	}
	c_dir_close(&dir);
}

/* exec routines */
static char *
progname(char *dofile, int *toexec)
{
	static ctype_arr arr; /* "memory leak" */
	ctype_stat sta, stb;
	ctype_ioq ioq;
	ctype_fd fd;
	char buf[C_IOQ_BSIZ];
	char *s;

	/* check executability */
	if (c_nix_stat(&sta, dofile) < 0) {
		c_err_die(1, "failed to obtain info \"%s\"", dofile);
	}
	if (sta.mode & C_NIX_IXUSR) return (*toexec = 0, dofile);
	*toexec = 1;

	/* init io queue and check race condition */
	if ((fd = c_nix_fdopen2(dofile, C_NIX_OREAD)) < 0) {
		if (errno == C_ERR_ENOENT) return progname(dofile, toexec);
		c_err_die(1, "failed to open \"%s\"", dofile);
	}
	if (c_nix_fdstat(&stb, fd) < 0) {
		c_err_die(1, "failed to obtain fd info \"%s\"", dofile);
	}
	if (!(sta.dev == stb.dev && sta.ino == stb.ino)) {
		c_nix_fdclose(fd);
		return progname(dofile, toexec);
	}
	c_ioq_init(&ioq, fd, buf, sizeof(buf), &c_nix_fdread);

	/* get first line */
	c_arr_trunc(&arr, 0, sizeof(uchar));
	switch (c_ioq_getln(&arr, &ioq)) {
	case -1:
		c_err_die(1, "failed to read \"%s\"", dofile);
	case 0:
		c_nix_fdclose(fd);
		goto shfallback;
	}
	c_nix_fdclose(fd);

	/* check for shellbang */
	c_arr_trunc(&arr, c_arr_bytes(&arr) - 1, sizeof(uchar)); /* linefeed */
	s = c_arr_data(&arr);
	if (s[0] == '#' && s[1] == '!') return s + 2;
shfallback:
	c_arr_trunc(&arr, 0, sizeof(uchar));
	dynfmt(&arr, "/bin/sh -e%s", (opts & XFLAG) ? "x" : "");
	return c_arr_data(&arr);
}

static char **
getargs(char *dofile, char *target, char *out)
{
	int toexec;
	char *base, *prog;
	prog = progname(dofile, &toexec);
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

/* Thank fs/kern APIs for forcing workarounds to replace a dir reliably:
 * 1. symlink (chosen): dir is not a dir anymore + orphans
 * 2. mount dest + change orig state + umount: require privileges
 * 3. readat2: not portable nor reliable
 */
static ctype_status
replace(char *d, char *s)
{
	ctype_stat sta, stb;
	char sym[sizeof(TMPFILE)];
	char tmp[sizeof(TMPFILE)];
	char *path;

	if (c_nix_lstat(&sta, s) < 0) {
		c_err_die(1, "failed to obtain info \"%s\"", s);
	}
	if (C_NIX_ISDIR(sta.mode)) {
		c_str_cpy(sym, sizeof(sym), TMPFILE);
		c_nix_fdclose(c_nix_mktemp3(sym, sizeof(sym), C_NIX_OTMPANON));
		if (c_nix_symlink(sym, c_gen_basename(s)) < 0) {
			c_err_die(1, "failed to create symlink \"%s\"", sym);
		}
		s = sym;
	}
	if (c_nix_lstat(&sta, d) < 0) {
		if (errno == C_ERR_ENOENT) goto end;
		c_err_die(1, "failed to obtain info \"%s\"", d);
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
		trackfile(tmp, c_str_len(tmp, -1));
		return 0;
	} else if (C_NIX_ISLNK(sta.mode)) {
		if (c_nix_stat(&stb, d) < 0) {
			if (errno == C_ERR_ENOENT) goto end;
			c_err_die(1, "failed to obtain info \"%s\"", d);
		}
		if (C_NIX_ISDIR(stb.mode)) {
			path = c_gen_basename(linkname(d));
			if (!C_STR_CMP(TMPBASE, path)) {
				if (c_nix_rename(d, s) < 0) return -1;
				trackfile(path, c_str_len(path, -1));
				return 0;
			}
		}
	}
end:
	if (c_nix_rename(d, s) < 0) return -1;
	return 0;
}

static int
rundo(char *dofile, char *target)
{
	ctype_arr arr;
	ctype_status r;
	char out[C_LIM_PATHMAX];
	char **args;

	/* dir */
	char *dir = dirname(dofile);
	c_exc_setenv("PWD", dir);
	c_nix_chdir(dir);
	target += c_str_len(dir, -1) + 1; /* matching(dofile, target) + "/" */

	/* redirection */
	c_arr_init(&arr, out, sizeof(out));
	arrfmt(&arr, "%s/" TMPFILE, dirname(target)); /* $3 */
	c_nix_fdclose(c_nix_mktemp3(out, c_arr_bytes(&arr), C_NIX_OTMPANON));

	/* redo env */
	setenv(REDO_TARGET, target);
	setnum(REDO_DEPTH, depth + 1);

	/* exec */
	args = getargs(dofile, target, out);
	execout(args);
	c_std_free(args);

	/* target */
	if (exist(out)) {
		if ((r = replace(target, out)) < 0) {
			c_nix_unlink(out);
			c_err_die(1, "failed to replace %s", target);
		}
		return 1;
	}
	return 0;
}

/* dep routines */
static ctype_status
_rebuild_func(char *file, char *s, usize n, void *data)
{
	ctype_stat st;
	ctype_taia now;
	struct rebuild *r;
	char buf[C_TAIA_PACK];

	(void)file;
	r = data;
	/* TYPE(+0),TAIA(+1),MD5(+34),NAME(+67) */
	if (s[0] == '=' && !c_str_cmp(s + 67, n - 67, r->target)) {
		if (c_nix_lstat(&st, s + 67) < 0) return 1;
		c_taia_fromtime(&now, &st.ctim);
		c_taia_pack(buf, &now);
		return c_nix_fdfmt(r->fd,
		    "%c%H%s\n", *s, sizeof(buf), buf, s + 33);
	}

	c_nix_fdfmt(r->fd, "%s\n", s);
	return 0;
}

static int
rebuild(char *s, char *target)
{
	struct rebuild r;
	ctype_status ret;
	usize len;
	char *deptmp;

	r.target = target;
	deptmp = dbgetpath(TMPNAME, TMPEXT);
	len = c_str_len(deptmp, -1);
	if (!(deptmp = c_str_dup(deptmp, len))) c_err_die(1, nil);
	r.fd = mktemp(deptmp, len, 0);

	if (!(ret = dbgetlines(s, &_rebuild_func, &r))) {
		ret = c_nix_rename(dbgetpath(pathshrink(s), "dep"), deptmp);
	}
	c_nix_fdclose(r.fd);
	c_std_free(deptmp);
	return ret;

}

static int
modified(char *s)
{
	ctype_hst h;
	ctype_stat st;
	ctype_taia t;
	char buf[C_HSH_MD5DIG];

	if (c_nix_lstat(&st, s + 67) < 0) {
		if (errno == C_ERR_ENOENT) return 1;
		c_err_die(1, "failed to obtain info \"%s\"", s + 67);
	}

	/* time stamp */
	c_taia_fromtime(&t, &st.ctim);
	c_taia_pack(buf, &t);
	if (!hexcmp(s + 1, C_TAIA_PACK, buf)) return 0;

	/* hash */
	c_hsh_md5->init(&h);
	hashfd(&h, s + 67, &st);
	c_hsh_md5->end(&h, buf);
	if (!hexcmp(s + 34, C_HSH_MD5DIG, buf)) return -1;
	return 1;
}

static ctype_status
_depcheck_func(char *file, char *s, usize n, void *data)
{
	if (opts & FFLAG) return 1;
	(void)data;
	switch (*s) {
	case '-':
		return exist(s + 1);
	case '=':
		/* TYPE(+0),TAIA(+1),MD5(+34),NAME(+67) */
		switch (modified(s)) {
		case -1:
			if (rebuild(file, s + 67)) return 0;
			/* FALLTHROUGH */
		case 0:
			if (c_str_cmp(file, -1, s + 67) &&
			    C_STR_SCMP(".do", s + (n - 3))) {
				return depcheck(s + 67);
			}
			return 0;
		case 1:
			return 1;
		}
		break;
	case '+':
		return !exist(s + 1);
	case '@':
		return depcheck(s + 1);
	}
	return 1;
}

static int
depcheck(char *s)
{
	ctype_status ret;
	if ((ret = dbgetlines(s, &_depcheck_func, nil)) < 0) return !exist(s);
	return ret;
}

static void
depwrite(ctype_fd fd, char *dep)
{
	ctype_hst h;
	ctype_taia now;
	ctype_stat st;
	char hash[C_HSH_MD5DIG];
	char time[C_TAIA_PACK];

	if (fd < 0) return;
	if (c_nix_lstat(&st, dep) < 0) c_err_die(1, nil);
	/* timestamp */
	c_taia_fromtime(&now, &st.ctim);
	c_taia_pack(time, &now);
	/* hash */
	c_hsh_md5->init(&h);
	hashfd(&h, dep, &st);
	c_hsh_md5->end(&h, hash);
	/* write */
	c_nix_fdfmt(fd, "=%H %H %s\n",
	    sizeof(time), time, sizeof(hash), hash, pathshrink(dep));
}

/* do routines */
static void *
_whichdo(char *target)
{
	ctype_arr arr;
	usize len;
	char *ext, *s, *tmp;
	char **ptr;

	if (!(target = tmp = c_str_dup(target, -1))) c_err_die(1, nil);
	ext = c_str_chr(c_gen_basename(tmp), -1, '.');

	c_mem_set(&arr, sizeof(arr), 0);
	if (!(ptr = c_dyn_alloc(&arr, (len = 0), sizeof(void *)))) {
		c_err_die(1, nil);
	}

	if (c_str_fmt(ptr, "%s.do", tmp) < 0) c_err_die(1, nil);

	for (;;) {
		tmp = c_gen_dirname(tmp);
		s = ext;
		while (s) {
			if (!(ptr = c_dyn_alloc(&arr, ++len, sizeof(void *)))) {
				c_err_die(1, nil);
			}
			if (c_str_fmt(ptr, "%s/default%s.do", tmp, s) < 0) {
				c_err_die(1, nil);
			}
			s = c_str_chr(s + 1, -1, '.');
		}
		if (!(ptr = c_dyn_alloc(&arr, ++len, sizeof(void *)))) {
			c_err_die(1, nil);
		}
		if (c_str_fmt(ptr, "%s/default.do", tmp) < 0) {
			c_err_die(1, nil);
		}
		if (!c_str_cmp(tmp, -1, rootdir)) break;
		if (!c_str_cmp(tmp, -1, "/")) break;
	}

	c_std_free(target);
	if (!(ptr = c_dyn_alloc(&arr, ++len, sizeof(void *)))) {
		c_err_die(1, nil);
	}
	*ptr = nil;
	return c_arr_data(&arr);
}

static ctype_status
always(ctype_fd fd)
{
	c_nix_fdfmt(fd, "!\n");
	return 0;
}

static ctype_status
ifcreate(char *s)
{
	c_nix_fdfmt(parentfd, "-%s\n", pathshrink(s));
	return exist(s);
}

static ctype_status
_ifchange(char *s)
{
	ctype_stat st;
	ctype_fd depfd;
	usize len;
	char *dep, *deptmp;
	char **dofiles;

	if (!depcheck(s)) return 0;

	deptmp = dbgetpath(TMPNAME, TMPEXT);
	len = c_str_len(deptmp, -1);
	if (!(deptmp = c_str_dup(deptmp, len))) c_err_die(1, nil);
	depfd = mktemp(deptmp, len, 0);
	setnum(REDO_DEPFD, depfd);

	dep = dbgetpath(pathshrink(s), "dep");

	dofiles = _whichdo(s);
	for (; *dofiles; ++dofiles) {
		if (exist(*dofiles)) break;
		c_nix_fdfmt(depfd, "-%s\n", pathshrink(*dofiles));
	}
	if (!*dofiles) {
		if (c_nix_lstat(&st, s)) {
			c_nix_unlink(dep);
			if (C_NIX_ISDIR(st.mode)) {
				len = c_str_len(dep, -1);
				c_str_cpy(dep + (len - 4), len, ".src");
				c_nix_fdclose(c_nix_mktemp3(dep, len, 0));
			}
			c_nix_unlink(deptmp);
			goto end;
		} else {
			c_err_diex(1, "%s: no .do file.\n", pathshrink(s));
		}
	}

	len = depth << 1;
	c_err_warnx("%*.*s %s", (int)len, (int)len, " ", pathshrink(s));

	if (rundo(*dofiles, s)) {
		c_nix_fdfmt(depfd, "+%s\n", pathshrink(s));
	}
	if (c_nix_rename(dep, deptmp) < 0) {
		c_err_die(1, "failed to update dep \"%s\"", dep);
	}
	depwrite(depfd, *dofiles);
	for (; *dofiles; ++dofiles) c_std_free(*dofiles);
end:
	c_std_free(dep);
	c_std_free(deptmp);
	c_std_free(dofiles);
	c_nix_fdclose(depfd);
	return 0;
}

static ctype_status
ifchange(char *s)
{
	ctype_status ret;

	ret = _ifchange(s);
	if (opts & FFLAG) return ret;

	if (exist(s)) {
		depwrite(parentfd, s);
	} else {
		c_nix_fdfmt(parentfd, "@%s\n", pathshrink(s));
	}
	return ret;
}

static ctype_status
whichdo(char *s)
{
	char **dofiles;
	dofiles = _whichdo(s);
	for (; *dofiles; ++dofiles) {
		c_ioq_fmt(ioq1, "%s\n", pathshrink(*dofiles));
		if (exist(*dofiles)) break;
		c_std_free(*dofiles);
	}
	c_ioq_flush(ioq1);
	for (; *dofiles; ++dofiles) c_std_free(*dofiles);
	c_std_free(dofiles);
	return -1;
}

/* do dep routines */
static ctype_status
_sources_func(char *file, char *s, usize n, void *data)
{
	struct sources *sp;

	sp = data;

	if (*s != '=') return 0;
	s += 67;
	if (opts & FFLAG) {
		if (c_str_cmp(sp->file, n, s)) return 0;
		c_ioq_fmt(ioq1, "%s\n", pathshrink(file));
		return 1;
	}

	if (c_adt_kvadd(&sp->t, s, nil) == 1) return 0;
	c_ioq_fmt(ioq1, "%s\n", pathshrink(s));

	if (sp->file) {
		s = dbgetpath(s, "dep");
		if (exist(s)) sources(s, c_str_len(s, -1), sp->file);
	}
	return 0;
}

static void
sources(char *dep, usize n, char *file)
{
	static struct sources src;
	src.file = file;

	dep[n - 4] = 0;
	dep += rootdlen + 7; /* .redo/ */

	dbgetlines(dep, &_sources_func, &src);
}

static void
sourcefile(char *dep, usize n, char *file)
{
	if (file) {
		if (c_str_cmp(dep, n, dbgetpath(absolute(file), "dep"))) {
			return;
		}
	}
	sources(dep, n, nil);
}

static void
targets(char *dep, usize n, char *file)
{
	if (file && (opts & FFLAG)) {
		sources(dep, n, file);
		return;
	}

	dep[n - 4] = 0; /* strip ".dep" */
	dep += rootdlen + 7; /* strip "${rootdir}/.redo/"" */

	if (opts & FFLAG) {
		if (!exist(dep)) return;
	} else if (!depcheck(dep)) {
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

	args[0] = nil;
	args[1] = nil;
	if (s) {
		dep = dbgetpath(pathshrink(absolute(s)), "dep");
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

	if (!args[0]) args[0] = dbgetpath(nil, nil);

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

/* main routines */
static void
checkparent(void)
{
	if (parentfd == -1) c_err_diex(1, "must be run from inside a .do");
}

static void
usage(char *msg)
{
	c_ioq_fmt(ioq2, "usage: %s%s\n", c_std_getprogname(), msg);
	c_std_exit(1);
}

static ctype_status
mainfunc(int argc, char **argv, ctype_status (*fn)(char *))
{
	(void)argc;
	ctype_status r;
	r = 0;
	for (; *argv; ++argv) {
		switch (fn(absolute(*argv))) {
		case -1:
			return r;
		case 1:
			r = 1;
		}
	}
	return r;
}

static ctype_status
redo(int argc, char **argv)
{
	char *args[2];

	while (c_std_getopt(argmain, argc, argv, "j:x")) {
		switch (argmain->opt) {
		case 'j': /* XXX */
			break;
		case 'x':
			setnum(REDO_XFLAG, 1);
			opts |= XFLAG;
			break;
		default:
			usage(USAGE_REDO);
		}
	}
	argc -= argmain->idx;
	argv += argmain->idx;

	if (!argc) {
		args[0] = "all";
		args[1] = nil;
		argc = 1;
		argv = args;
	}
	return mainfunc(argc, argv, &ifchange);
}

ctype_status
main(int argc, char **argv)
{
	char *prog;

	c_std_setprogname((prog = argv[0]));
	--argc, ++argv;

	/* prepare environment */
	pwd = getpwd();
	parentfd = getnum(REDO_DEPFD);
	depth = getnum(REDO_DEPTH);
	if (depth < 0) depth = 0;

	rootdir = c_std_getenv(REDO_ROOTDIR);
	if (!rootdir) {
		rootdir = pwd;
		setenv(REDO_ROOTDIR, rootdir);
		sync();
	}
	rootdlen = c_str_len(rootdir, -1);

	if (!(getnum(REDO_XFLAG) < 0)) opts |= XFLAG;

	/* prog routines */
	c_fmt_install('H', &hex);
	prog = c_gen_basename(prog);
	if (!C_STR_SCMP("redo", prog)) {
		c_std_atexit(cleantrash);
		opts |= FFLAG;
		return redo(argc, argv);
	} else if (!C_STR_SCMP("redo-always", prog)) {
		checkparent();
		noopt(argc, argv, usage(USAGE_DEF2));
		if (argc) usage(USAGE_DEF2);
		return always(parentfd);
	} else if (!C_STR_SCMP("redo-ifchange", prog)) {
		c_std_atexit(cleantrash);
		if (parentfd == -1) return redo(argc, argv);
		noopt(argc, argv, usage(USAGE_DEF1));
		return mainfunc(argc, argv, &ifchange);
	} else if (!C_STR_SCMP("redo-ifcreate", prog)) {
		checkparent();
		noopt(argc, argv, usage(USAGE_DEF1));
		return mainfunc(argc, argv, &ifcreate);
	} else if (!C_STR_SCMP("redo-ood", prog)) {
		noopt(argc, argv, usage(USAGE_DEF0));
		walkdeps(targets, *argv);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-sources", prog)) {
		noopt(argc, argv, usage(USAGE_DEF0));
		walkdeps(argc ? sourcefile : sources, *argv);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-stamp", prog)) {
		/* dummy */
		noopt(argc, argv, usage(USAGE_DEF2));
		if (argc) usage(USAGE_DEF2);
		return 0;
	} else if (!C_STR_SCMP("redo-targets", prog)) {
		noopt(argc, argv, usage(USAGE_DEF0));
		opts |= FFLAG;
		walkdeps(targets, *argv);
		c_ioq_flush(ioq1);
	} else if (!C_STR_SCMP("redo-whichdo", prog)) {
		noopt(argc, argv, usage(USAGE_DEF0));
		return mainfunc(argc, argv, &whichdo);
	} else {
		c_err_diex(1, "invalid progname \"%s\"", prog);
	}
	return 0;
}
