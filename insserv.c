/*
 * insserv(.c)
 *
 * Copyright 2000-2009 Werner Fink, 2000 SuSE GmbH Nuernberg, Germany,
 *				    2003 SuSE Linux AG, Germany.
 *				    2004 SuSE LINUX AG, Germany.
 *			       2005-2009 SUSE LINUX Products GmbH, Germany.
 * Copyright 2005,2008,2009 Petter Reinholdtsen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifdef WANT_SYSTEMD
/*
 * Systemd integration
 */
  #ifndef SYSTEMD_SERVICE_PATH
     #define SYSTEMD_SERVICE_PATH	"/lib/systemd/system"
  #endif
  #ifndef SYSTEMD_BINARY_PATH
     #define SYSTEMD_BINARY_PATH	"/bin/systemd"
  #endif
#endif /* WANT_SYSTEMD */

#define MINIMAL_MAKE	1	/* Remove disabled scripts from .depend.boot,
				 * .depend.start, .depend.halt, and .depend.stop */
#define MINIMAL_RULES	1	/* ditto */
#define MINIMAL_DEPEND	1	/* Remove redundant dependencies */

#include <pwd.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <regex.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#if defined(__linux__)
# include <linux/magic.h>
#endif
#if !defined(CGROUP_SUPER_MAGIC)
# define CGROUP_SUPER_MAGIC	0x27e0eb
#endif
#if defined(USE_RPMLIB) && (USE_RPMLIB > 0)
# include <rpm/rpmlib.h>
# include <rpm/rpmmacro.h>
#endif /* USE_RPMLIB */
#ifdef SUSE
# include <sys/mount.h>
#endif /* SUSE */
#ifndef NEED_RUNLEVELS_DEF
#define NEED_RUNLEVELS_DEF
#endif
#include "listing.h"
#include "systemd.h"

#ifdef __m68k__ /* Fix #493637 */
#  define aligned(a)
#endif

#if defined _XOPEN_SOURCE && (_XOPEN_SOURCE - 0) >= 600
# ifndef POSIX_FADV_SEQUENTIAL
#  define posix_fadvise(fd, off, len, adv)  (-1)
# endif
#endif

#ifndef PATH_MAX
# ifdef MAXPATHLEN
#  define PATH_MAX  MAXPATHLEN
# else
#  define PATH_MAX  2048
# endif
#endif

#ifndef MAXSYMLINKS
#define MAXSYMLINKS 20
#endif

#ifdef SUSE
# define DEFAULT_START_LVL	"3 5"
# define DEFAULT_STOP_LVL	"3 5"
# define USE_KILL_IN_BOOT	1
# define USE_COMPAT_EMPTY	1
static inline void oneway(char *restrict stop) attribute((always_inline,nonnull(1)));
static inline void oneway(char *restrict stop)
{
    char * ptr = stop;
    while ((ptr = strpbrk(ptr, "016sS")))
	*ptr++ = ' ';
}
#else /* not SUSE, but Debian */
# define DEFAULT_START_LVL	"2 3 4 5"
# define DEFAULT_STOP_LVL	"0 1 6"
# define DEFAULT_DEPENDENCY	"$remote_fs $syslog"
# undef  USE_KILL_IN_BOOT
# undef  USE_COMPAT_EMPTY
#endif /* not SUSE, but Debian */

#ifndef  INITDIR
# define INITDIR	"/etc/init.d"
#endif
#ifndef  OVERRIDEDIR
# define OVERRIDEDIR	"/etc/insserv/overrides"
#endif
#ifndef  INSCONF
# define INSCONF	"/etc/insserv.conf"
#endif

/* Upstart suport */
static const char *upstartjob_path = "/lib/init/upstart-job";

#ifdef WANT_SYSTEMD
/* Systemd support */
static DBusConnection *sbus;

#endif /* WANT_SYSTEMD */

/*
 * For a description of regular expressions see regex(7).
 */
#define COMM		"^#[[:blank:]]*"
#define VALUE		":[[:blank:]]*([[:print:]]*)"
/* The second substring contains our value (the first is all) */
#define SUBNUM		2
#define SUBNUM_SHD	3
#define START		"[-_]+start"
#define STOP		"[-_]+stop"

/* The main regular search expressions */
#define PROVIDES	COMM "provides" VALUE
#define REQUIRED	COMM "required"
#define SHOULD		COMM "(x[-_]+[a-z0-9_-]*)?should"
#define BEFORE		COMM "(x[-_]+[a-z0-9_-]*)?start[-_]+before"
#define AFTER		COMM "(x[-_]+[a-z0-9_-]*)?stop[-_]+after"
#define DEFAULT		COMM "default"
#define REQUIRED_START  REQUIRED START VALUE
#define REQUIRED_STOP	REQUIRED STOP  VALUE
#define SHOULD_START	SHOULD   START VALUE
#define SHOULD_STOP	SHOULD   STOP  VALUE
#define START_BEFORE	BEFORE   VALUE
#define STOP_AFTER	AFTER    VALUE
#define DEFAULT_START	DEFAULT  START VALUE
#define DEFAULT_STOP	DEFAULT  STOP  VALUE
#define DESCRIPTION	COMM "description" VALUE
#define INTERACTIVE	COMM "(x[-_]+[a-z0-9_-]*)?interactive" VALUE

/* System facility search within /etc/insserv.conf */
#define EQSIGN		"([[:blank:]]*[=:][[:blank:]]*|[[:blank:]]+)"
#define CONFLINE	"^(\\$[a-z0-9_-]+)" EQSIGN "([[:print:]]*)"
#define CONFLINE2	"^(<[a-z0-9_-]+>)"  EQSIGN "([[:print:]]*)"
#define SUBCONF		2
#define SUBCONFNUM	4

/* The root file system */
static char *root;

/* The main line buffer if unique */
static char buf[LINE_MAX];

/* When to be verbose, and what level of verbosity */
static int verbose = 0;
static boolean silent_mode = false;
static boolean dryrun = false;

/* When paths set do not add root if any */
static boolean set_override = false;
static boolean set_insconf = false;

/* Legacy and current location for dependency files */
/* #define DEPENDENCY_PATH "/lib/insserv/" */
#define LEGACY_DEPENDENCY_PATH "/etc/init.d/."
char *dependency_path = LEGACY_DEPENDENCY_PATH;

/* List of custom file extensions we should ignore.
   Loaded from /etc/insserv/file-filters.
*/
#define FILE_FILTER_PATH "/etc/insserv/file-filters"
char **file_filters = NULL;

/* Wether systemd is active or not */
#if WANT_SYSTEMD
static boolean systemd = false;
static boolean is_overridden_by_systemd(const char *);
#endif /* WANT_SYSTEMD */

/* Search results points here */
typedef struct lsb_struct {
    char *provides;
    char *required_start;
    char *required_stop;
    char *should_start;
    char *should_stop;
    char *start_before;
    char *stop_after;
    char *default_start;
    char *default_stop;
    char *description;
    char *interactive;
} attribute((aligned(sizeof(char*)))) lsb_t;

/* Search results points here */
typedef struct reg_struct {
    regex_t prov;
    regex_t req_start;
    regex_t req_stop;
    regex_t shl_start;
    regex_t shl_stop;
    regex_t start_bf;
    regex_t stop_af;
    regex_t def_start;
    regex_t def_stop;
    regex_t desc;
    regex_t interact;
} attribute((aligned(sizeof(regex_t)))) reg_t;

typedef struct creg_struct {
    regex_t isysfaci;
    regex_t isactive;
} attribute((aligned(sizeof(regex_t)))) creg_t;

static lsb_t script_inf;
static reg_t reg;
static creg_t creg;
char empty[1] = "";

/* Delimeters used for spliting results with strsep(3) */
const char *const delimeter = " ,;\t";

/*
 * push and pop directory changes: pushd() and popd()
 */
typedef struct pwd_struct {
    list_t	deep;
    char	*pwd;
} __align pwd_t;
#define getpwd(list)	list_entry((list), struct pwd_struct, deep)

static list_t pwd = { &pwd, &pwd }, * topd = &pwd;

static void pushd(const char *restrict const path) attribute((nonnull(1)));
static void pushd(const char *restrict const path)
{
    pwd_t *restrict dir;

    if (posix_memalign((void*)&dir, sizeof(void*), alignof(pwd_t)) == 0) {
	if (!(dir->pwd = getcwd((char*)0, 0)))
           fprintf(stderr, "pushd() cannot save current working directory.\n");
	else
           insert(&dir->deep, topd->prev);
	if (chdir(path) < 0)
           error("pushd() cannot change to directory %s: %s\n", path, strerror(errno) );
    }
}

static void popd(void)
{
    pwd_t * dir;

    if (list_empty(topd))
	return;
    if (topd->prev)
    {
       dir = getpwd(topd->prev);
       if (! dir->pwd)
       {
          fprintf(stderr, "popd() previous directory does not exist in memory.\n");
          return;
       }
       if (chdir(dir->pwd) < 0)
       {
   	   fprintf(stderr, "popd() can not change directory %s: %s\n", dir->pwd, strerror(errno) );
           return;
       }
       delete(topd->prev);
       free(dir->pwd);
       free(dir);
   }
}


/*
This function loads a list of newline-separated extensions from
the FILE_FILTER_PATH text file.
The function returns an array of strings (extensions) on success
and NULL on failure.
*/
char **Load_File_Filters()
{
  FILE *my_file;
  char **temp_strings;
  char line[32];
  char *status;
  int read_lines = 0;
  int buffer_size = 10;
  
  my_file = fopen(FILE_FILTER_PATH, "r");
  if (! my_file)
     return NULL;

  temp_strings = (char **) calloc(buffer_size, sizeof(char *));
  if (! temp_strings)
  {
     fclose(my_file);
     return NULL;
  }
  status = fgets(line, 32, my_file);
  while (status)
  {
     /* check to see if line has any contents */
     if ( (line[0]) && (line[0] != '\n') && (line[0] != '\r') )
     {
        int string_length;
        /* trim newline */
        string_length = strlen(line);
        if ( line[string_length - 1] == '\n' )
           line[string_length - 1] = '\0';

        temp_strings[read_lines] = strdup(line);
        read_lines++;
        /* buffer may be full, extend it */
        if (read_lines >= 1000)     /* too big, bail out */
        {
           warn("warning: too many file extensions listed in %s\n", FILE_FILTER_PATH);
           break;
        } 
        if (read_lines >= buffer_size) 
        {
           int index;
           buffer_size += 10;    /* extend the buffer */
           temp_strings = (char **) realloc(temp_strings, buffer_size * sizeof(char *));
           for (index = read_lines; index < buffer_size; index++)
             temp_strings[index] = NULL;   /* init memory since realloc does not */
        }
     }
     status = fgets(line, 32, my_file);
  }
  fclose(my_file);
  return temp_strings;
}



/*
 * Linked list of system facilities services and their replacment
 */
typedef struct string {
    list_t     s_list;
    int		  ref;
    char	*name;
} __align string_t;
#define getfstr(arg)	list_entry((arg), struct string, s_list)

typedef struct repl {
    list_t     r_list;
    struct {
	string_t   *addr;
	const char *name;
    };
    ushort	flags;
} __align repl_t;
#define getrepl(arg)	list_entry((arg), struct repl, r_list)

typedef struct faci {
    list_t	 list;
    list_t    replace;
    char	*name;
} __align faci_t;
#define getfaci(arg)	list_entry((arg), struct faci, list)

static list_t facistr = { &facistr, &facistr }, *facistr_start = &facistr;
static list_t sysfaci = { &sysfaci, &sysfaci }, *sysfaci_start = &sysfaci;

/*
 * Remember requests for required or should services and expand `$' token
 */
static void rememberreq(service_t *restrict serv, uint bit,
		        const char *restrict required) attribute((noinline,nonnull(1,3)));
static void rememberreq(service_t * restrict serv, uint bit, const char * restrict required)
{
    const char type = (bit & REQ_KILL) ? 'K' : 'S';
    const char * token;
    char * tmp = strdupa(required);
    list_t * ptr, * list;
    ushort old = bit;
    boolean can_expand_name = false;

    if (!tmp)
	error("%s", strerror(errno));

    while ((token = strsep(&tmp, delimeter)) && *token) {
	service_t * req, * here, * need;
	boolean found = false;

	bit = old;

	switch(*token) {
	case '+':
	    /* This is an optional token */
	    token++;
	    bit &= ~REQ_MUST;
	    bit |=  REQ_SHLD;
	    /* fall through */
	default:
	    req = addservice(token);
	    if (bit & REQ_KILL) {
		req  = getorig(req);
		list = &req->sort.rev;
		here = req;
		need = serv;
	    } else {
		serv = getorig(serv);
		list = &serv->sort.req;
		here = serv;
		need = req;
	    }
	    np_list_for_each(ptr, list) {
		if (!strcmp(getreq(ptr)->serv->name, need->name)) {
		    getreq(ptr)->flags |= bit;
		    found = true;
		    break;
		}
	    }
	    if (!found) {
		req_t *restrict this;
		if (posix_memalign((void*)&this, sizeof(void*), alignof(req_t)) != 0)
		    error("%s", strerror(errno));
		memset(this, 0, alignof(req_t));
		insert(&this->list, list->prev);
		this->flags = bit;
		this->serv = need;
	    }
	    /* Expand requested services for sorting */
	    requires(here, need, type);
	    break;
	case '$':
	    if (strcasecmp(token, "$null") == 0)
		break;
	    if (strcasecmp(token, "$all") == 0) {
		if (bit & REQ_KILL)
		    serv->attr.flags |= SERV_FIRST;
		else
		    serv->attr.flags |= SERV_ALL;
		break;
	    }
	    /* Expand the `$' token recursively down */
            can_expand_name = false;
	    list_for_each(ptr, sysfaci_start) {
		if (!strcmp(token, getfaci(ptr)->name)) {
		    list_t * lst;
                    can_expand_name = true;
		    np_list_for_each(lst, &getfaci(ptr)->replace)
			rememberreq(serv, bit, getrepl(lst)->name);
		    break;
		}
	    }
            if (! can_expand_name)
               warn("warning: could not find all dependencies for %s\n", token);

	    break;
	}
    }
}

static void reversereq(service_t *restrict serv, uint bit,
		       const char *restrict list) attribute((noinline,nonnull(1,3)));
static void reversereq(service_t *restrict serv, uint bit, const char *restrict list)
{
    const char * token;
    char * tmp = strdupa(list);
    ushort old = bit;

    if (!tmp)
	error("%s", strerror(errno));

    while ((token = strsep(&tmp, delimeter)) && *token) {
	service_t * rev;
	list_t * ptr;

	bit = old;

	switch (*token) {
	case '+':
	    token++;
	    bit &= ~REQ_MUST;
	    bit |=  REQ_SHLD;
	    /* fall through */
	default:
	    rev = addservice(token);
	    rememberreq(rev, bit, serv->name);
	    break;
	case '$':
	    list_for_each(ptr, sysfaci_start) {
		if (!strcmp(token, getfaci(ptr)->name)) {
		    list_t * lst;
		    np_list_for_each(lst, &getfaci(ptr)->replace)
			reversereq(serv, bit, getrepl(lst)->name);
		    break;
		}
	    }
	    break;
	}
    }
}

/*
 * Check required services for name
 */
static boolean chkrequired(service_t *restrict serv, const boolean recursive) attribute((nonnull(1)));
static boolean chkrequired(service_t *restrict serv, const boolean recursive)
{
    boolean ret = true;
    list_t * pos;

    /* Technically, it is not possible for this function to be called if serv is
       NULL, which makes the compiler complain about this check. Commenting it
       out since neither of these conditions is likely to change given program's
       mature/legacy nature. -- Jesse
    if (!serv)
	return ret;
    */
    serv = getorig(serv);

    np_list_for_each(pos, &serv->sort.req) {
	req_t *req = getreq(pos);
	service_t * must;

	if ((req->flags & REQ_MUST) == 0)
	    continue;
	must = req->serv;
	must = getorig(must);

	if ((must->attr.flags & (SERV_CMDLINE|SERV_ENABLED)) == 0) {
	    if (recursive) {
		must->attr.flags |= SERV_ENFORCE;
		continue;	/* Enabled this later even if not on command line */
	    }
	    if ((must->attr.flags & SERV_WARNED) == 0) {
		warn("FATAL: service %s has to be enabled to use service %s\n",
		     req->serv->name, serv->name);
		must->attr.flags |= SERV_WARNED;
	    }
	    ret = false;
	}
    }
#if 0
    /*
     * Once we may use REQ_MUST for X-Start-Before and/or
     * X-Stop-After we may enable this, see reversereq()
     */
    if (serv->attr.flags & (SERV_CMDLINE|SERV_ENABLED))
	goto out;
    np_list_for_each(pos, &serv->sort.rev) {
	req_t *rev = getreq(pos);
	service_t * must;

	if ((rev->flags & REQ_MUST) == 0)
	    continue;
	must = rev->serv;
	must = getorig(must);
	if (must->attr.flags & (SERV_CMDLINE|SERV_ENABLED)) {
	    warn("FATAL: service %s has to be enabled to use service %s\n",
		 serv->name, rev->serv->name);
	    ret = false;
	}
    }
#endif
    return ret;
}

/*
 * Check dependencies for name as a service
 */
static boolean chkdependencies(service_t *restrict serv) attribute((nonnull(1)));
static boolean chkdependencies(service_t *restrict serv)
{
    const char * const name = serv->name;
    boolean ret = true;
    list_t * ptr;

    list_for_each(ptr, s_start) {
	service_t * cur = getservice(ptr);
	list_t * pos;

	if (!cur)
	    continue;

	if ((cur->attr.flags & SERV_ENABLED) == 0)
	    continue;

	if (cur->attr.flags & SERV_DUPLET)
	    continue;

	if (list_empty(&cur->sort.req))
	    continue;

	np_list_for_each(pos, &cur->sort.req) {
	    req_t *req = getreq(pos);
	    const ushort flags = req->serv->attr.flags;

	    if (!(req->flags & REQ_MUST))
		continue;

	    if (strcmp(req->serv->name, name) != 0)
		continue;

	    if ((cur->attr.flags & SERV_CMDLINE) && (flags & SERV_CMDLINE))
		continue;

	    warn("FATAL: service %s has to be enabled to use service %s\n",
		 name, cur->name);
	    ret = false;
	}
    }
    return ret;
}

/*
 * This helps us to work out the current symbolic link structure
 */
static inline service_t * current_structure(const char *const restrict this, const char order,
				     const int runlvl, const char type) attribute((always_inline,nonnull(1)));
static inline service_t * current_structure(const char *const this, const char order,
				     const int runlvl, const char type)
{
    service_t * serv = addservice(this);
    level_t * run;
    ushort here = map_runlevel_to_lvl(runlvl);

    if (type == 'K') {
	run = serv->stopp;
	if (!serv->attr.korder)
	    serv->attr.korder = 99;
	if (serv->attr.korder > order)
	    serv->attr.korder = order;
#ifdef SUSE
	/* This because SuSE boot script concept uses a differential link scheme. */
	here &= ~LVL_ONEWAY;
#endif /* SUSE */
    } else {
	run = serv->start;
	if (serv->attr.sorder < order)
	    serv->attr.sorder = order;
    }
    run->lvl |= here;

    return serv;
}

static void setlsb(const char *restrict const name) attribute((unused));
static void setlsb(const char *restrict const name)
{
    service_t * serv = findservice(name);
    if (serv)
	serv->attr.flags &= ~SERV_NOTLSB;
}

/*
 * This helps us to set none LSB conform scripts to required
 * max order, therefore we set a dependency to the first
 * lsb conform service found in current link scheme.
 */
static inline void nonlsb_script(void) attribute((always_inline));
static inline void nonlsb_script(void)
{
    list_t * pos;

    list_for_each(pos, s_start) {
	if (getservice(pos)->attr.flags & SERV_NOTLSB) {
	    service_t * req, * srv = getservice(pos);
	    list_t * tmp;
	    uchar max;

	    max = 0;
	    req = (service_t*)0;
	    list_for_each(tmp, s_start) {
		service_t * cur = getservice(tmp);
		if (cur->attr.flags & SERV_NOTLSB)
		    continue;
		if ((cur->attr.flags & SERV_ENABLED) == 0)
		    continue;
		if (!cur->attr.sorder)
		    continue;
		if ((srv->start->lvl & cur->start->lvl) == 0)
		    continue;
		if (cur->attr.sorder >= srv->attr.sorder)
		    continue;
		if (max < cur->attr.sorder) {
		    max = cur->attr.sorder;
		    req = cur;
		}
	    }

	    if (req)
		requires(srv, req, 'S');

	    max = 99;
	    req = (service_t*)0;
	    list_for_each(tmp, s_start) {
		service_t * cur = getservice(tmp);
		if (cur->attr.flags & SERV_NOTLSB)
		    continue;
		if ((cur->attr.flags & SERV_ENABLED) == 0)
		    continue;
		if (!cur->attr.korder)
		    continue;
		if ((srv->stopp->lvl & cur->stopp->lvl) == 0)
		    continue;
		if (cur->attr.korder <= srv->attr.korder)
		    continue;
		if (max > cur->attr.korder) {
		    max = cur->attr.korder;
		    req = cur;
		}
	    }

	    if (req)
		requires(req, srv, 'K');
	}
    }
}

/*
 * This helps us to get interactive scripts to be the only service
 * within on start or stop service group. Remaining problem is that
 * if required scripts are missed the order can be wrong.
 */
static inline void active_script(void) attribute((always_inline));
static inline void active_script(void)
{
    list_t * pos;
    int deep = 1;

    for (deep = 0; deep < 100; deep++) {
	list_for_each(pos, s_start) {
	    service_t * serv = getservice(pos);
	    list_t * tmp;

	    if (serv->attr.script == (char*)0)
		continue;

	    if ((serv->attr.flags & SERV_INTRACT) == 0)
		continue;

	    serv->attr.sorder = getorder(serv->attr.script, 'S');

	    if (serv->attr.sorder != deep)
		continue;

	    if (serv->attr.flags & SERV_DUPLET)
		continue;		/* Duplet */

	    list_for_each(tmp, s_start) {
		service_t * cur = getservice(tmp);
		const char * script;

		if (getorig(cur) == serv)
		    continue;

		if ((serv->start->lvl & cur->start->lvl) == 0)
		    continue;

		/*
		 * Use real script name for getorder()/setorder()
		 */
		if (cur->attr.script == (char*)0)
		    continue;
		script = cur->attr.script;

		cur->attr.sorder = getorder(script, 'S');

		if (cur->attr.sorder != deep)
		    continue;
		/*
		 * Increase order of members of the same start
		 * group and recalculate dependency order (`true')
		 */
		setorder(script, 'S', ++cur->attr.sorder, true);
	    }
	}
    }
}

/*
 * Last but not least the `$all' scripts will be set to the
 * beginning of the future stop order respectivly to the
 * end of the future start order.
 */
static inline void all_script(void) attribute((always_inline));
static inline void all_script(void)
{
    list_t * pos;

    list_for_each(pos, s_start) {
	service_t * serv = getservice(pos);
	list_t * tmp;

	if (serv->attr.flags & SERV_DUPLET)
	    continue;			/* Duplet */

	if (!(serv->attr.flags & SERV_FIRST))
	    continue;

	if (serv->attr.script == (char*)0)
	    continue;

	list_for_each(tmp, s_start) {
	    service_t * cur = getservice(tmp);

	    if (cur->attr.flags & SERV_DUPLET)
		continue;		/* Duplet */

	    if ((serv->stopp->lvl & cur->stopp->lvl) == 0)
		continue;

	    if (cur == serv)
		continue;

	    if (cur->attr.flags & SERV_FIRST)
		continue;

	    rememberreq(serv, REQ_SHLD|REQ_KILL, cur->name);
	}

	setorder(serv->attr.script, 'K', 1, false);
    }

    list_for_each(pos, s_start) {
	service_t * serv = getservice(pos);
	list_t * tmp;

	if (serv->attr.flags & SERV_DUPLET)
	    continue;			/* Duplet */

	if (!(serv->attr.flags & SERV_ALL))
	    continue;

	if (serv->attr.script == (char*)0)
	    continue;

	list_for_each(tmp, s_start) {
	    service_t * cur = getservice(tmp);

	    if (cur->attr.flags & SERV_DUPLET)
		continue;		/* Duplet */

	    if ((serv->start->lvl & cur->start->lvl) == 0)
		continue;

	    if (cur == serv)
		continue;

	    if (cur->attr.flags & SERV_ALL)
		continue;

	    rememberreq(serv, REQ_SHLD, cur->name);
	}
    }
}

/*
 * Make the dependency files
 */
static inline void makedep(void) attribute((always_inline));
static inline void makedep(void)
{
    FILE *boot, *start, *stop, *out;
#ifdef USE_KILL_IN_BOOT
    FILE *halt;
#endif /* USE_KILL_IN_BOOT */
    const char *target;
    const service_t *serv;
    char current_path[PATH_MAX];

    if (dryrun) {
#ifdef USE_KILL_IN_BOOT
	info(1, "dryrun, not creating depend.boot, depend.start, depend.halt, and depend.stop in %s\n", dependency_path);
#else  /* not USE_KILL_IN_BOOT */
	info(1, "dryrun, not creating depend.boot, depend.start, and depend.stop in %s\n", dependency_path);
#endif /* not USE_KILL_IN_BOOT */
	return;
    }
    snprintf(current_path, PATH_MAX, "%sdepend.boot", dependency_path);
    if (!(boot  = fopen(current_path,  "w"))) {
	warn("fopen(%s): %s\n", current_path, strerror(errno));
	return;
    }

    snprintf(current_path, PATH_MAX, "%sdepend.start", dependency_path);
    if (!(start = fopen(current_path, "w"))) {
	warn("fopen(%s): %s\n", current_path, strerror(errno));
	fclose(boot);
	return;
    }

    info(1, "creating %sdepend.boot\n", dependency_path);
    info(1, "creating %sdepend.start\n", dependency_path);

    lsort('S');				/* Sort into start order, set new sorder */

    target = (char*)0;
    fprintf(boot, "TARGETS =");
    while ((serv = listscripts(&target, 'S', LVL_BOOT))) {
#if defined(MINIMAL_MAKE) && (MINIMAL_MAKE != 0)
	if (serv->attr.ref <= 0)
	    continue;
#endif /* MINIMAL_MAKE */
	fprintf(boot, " %s", target);
    }
    fputc('\n', boot);

    target = (char*)0;
    fprintf(start, "TARGETS =");
    while ((serv = listscripts(&target, 'S', LVL_ALL))) {	/* LVL_ALL: nearly all but not BOOT */
#if defined(MINIMAL_MAKE) && (MINIMAL_MAKE != 0)
	if (serv->attr.ref <= 0)
	    continue;
#endif /* MINIMAL_MAKE */
	fprintf(start, " %s", target);
    }
    fputc('\n', start);

    fprintf(boot,  "INTERACTIVE =");
    fprintf(start, "INTERACTIVE =");

    target = (char*)0;
    while ((serv = listscripts(&target, 'S', LVL_BOOT|LVL_ALL))) {
#if defined(MINIMAL_MAKE) && (MINIMAL_MAKE != 0)
	if (serv->attr.ref <= 0)
	    continue;
#endif /* not MINIMAL_MAKE */

	if (list_empty(&serv->sort.req))
	    continue;

	if (serv->start->lvl & LVL_BOOT)
	    out = boot;
	else
	    out = start;

	if (serv->attr.flags & SERV_INTRACT)
	    fprintf(out, " %s", target);
    }
    fputc('\n', boot);
    fputc('\n', start);

    target = (char*)0;
    while ((serv = listscripts(&target, 'S', LVL_BOOT|LVL_ALL))) {
#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	const service_t * lserv[100] = {0};
	unsigned long lcnt = 0;
#endif /* not MINIMAL_DEPEND */
	boolean mark;
	list_t * pos;

#if defined(MINIMAL_RULES) && (MINIMAL_RULES != 0)
	if (serv->attr.ref <= 0)
	    continue;
#endif /* not MINIMAL_RULES */

	if (serv->start->lvl & LVL_BOOT)
	    out = boot;
	else
	    out = start;

	if (list_empty(&serv->sort.req))
	    continue;

	mark = false;

	np_list_for_each(pos, &serv->sort.req) {
	    req_t * req = getreq(pos);
	    service_t * dep = req->serv;
#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	    boolean shadow = false;
	    unsigned long n;
#endif /* not MINIMAL_DEPEND */
	    const char * name;

	    if (!dep)
		continue;

	    if (dep->attr.flags & SERV_DUPLET)
		continue;

#if defined(MINIMAL_RULES) && (MINIMAL_RULES != 0)
	    if (dep->attr.ref <= 0)
		continue;
#endif /* not MINIMAL_RULES */

	    /*
	     * No self dependcies or from the last
	     */
	    if (dep == serv || (dep->attr.flags & SERV_ALL))
		continue;

	    if ((serv->start->lvl & dep->start->lvl) == 0)
		continue;

	    if ((name = dep->attr.script) == (char*)0)
		continue;

	    if (!mark) {
		fprintf(out, "%s:", target);
		mark = true;
	    }
#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	    for (n = 0; n < lcnt && lserv[n] ; n++) {
		list_t * red;
		if (lserv[n]->attr.sorder <= dep->attr.sorder)
		    break;
		np_list_for_each(red, &(lserv[n])->sort.req) {
		    req_t * other = getreq(red);
		    if (other->serv->attr.flags & SERV_DUPLET)
			continue;
		    if (other->serv->attr.ref <= 0)
			continue;
		    if ((serv->start->lvl & other->serv->start->lvl) == 0)
			continue;
		    if (!other->serv->attr.script)
			continue;
		    if (other->serv != dep)
			continue;
		    shadow = true;
		}
	    }
	    if (shadow)
		continue;
#endif /* not MINIMAL_DEPEND */
	    fprintf(out, " %s", name);

#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	    if (lcnt >= sizeof(lserv)/sizeof(lserv[0]))
		continue;
	    lserv[lcnt++] = dep;
#endif /* not MINIMAL_DEPEND */
	}

	if (mark) fputc('\n', out);
    }

    fclose(boot);
    fclose(start);

    snprintf(current_path, PATH_MAX, "%sdepend.stop", dependency_path);
    if (!(stop  = fopen(current_path,  "w"))) {
	warn("fopen(%s): %s\n", current_path, strerror(errno));
	return;
    }

#ifdef USE_KILL_IN_BOOT
    snprintf(current_path, PATH_MAX, "%sdepend.halt", dependency_path);
    if (!(halt = fopen(current_path, "w"))) {
	warn("fopen(%s): %s\n", current_path, strerror(errno));
	fclose(stop);
	return;
    }

    info(1, "creating %sdepend.halt\n", dependency_path);
#endif /* USE_KILL_IN_BOOT */
    info(1, "creating %sdepend.stop\n", dependency_path);

    lsort('K');				/* Sort into stop order, set new korder */

    target = (char*)0;
    fprintf(stop, "TARGETS =");
    while ((serv = listscripts(&target, 'K', LVL_NORM))) {	/* LVL_NORM: nearly all but not BOOT and not SINGLE */
#if defined(MINIMAL_MAKE) && (MINIMAL_MAKE != 0)
	if (serv->attr.ref <= 0)
	    continue;
#endif /* MINIMAL_MAKE */
	fprintf(stop, " %s", target);
    }
    fputc('\n', stop);

#ifdef USE_KILL_IN_BOOT
    target = (char*)0;
    fprintf(halt, "TARGETS =");
    while ((serv = listscripts(&target, 'K', LVL_BOOT))) {
# if defined(MINIMAL_MAKE) && (MINIMAL_MAKE != 0)
	if (serv->attr.ref <= 0)
	    continue;
# endif /* MINIMAL_MAKE */
	fprintf(halt, " %s", target);
    }
    fputc('\n', halt);
#endif /* USE_KILL_IN_BOOT */

    target = (char*)0;
    while ((serv = listscripts(&target, 'K', (LVL_NORM|LVL_BOOT)))) {
#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	const service_t * lserv[100] = {0};
	unsigned long lcnt = 0;
#endif /* not MINIMAL_DEPEND */
	boolean mark;
	list_t * pos;

#if defined(MINIMAL_RULES) && (MINIMAL_RULES != 0)
	if (serv->attr.ref <= 0)
	    continue;
#endif /* not MINIMAL_RULES */

	if (list_empty(&serv->sort.rev))
	    continue;

	if (serv->stopp->lvl & LVL_BOOT)
#ifdef USE_KILL_IN_BOOT
	    out = halt;
	else
#else  /* not USE_KILL_IN_BOOT */
	    continue;
#endif /* not USE_KILL_IN_BOOT */
	out = stop;

	mark = false;
	np_list_for_each(pos, &serv->sort.rev) {
	    req_t * rev = getreq(pos);
	    service_t * dep = rev->serv;
#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	    boolean shadow = false;
	    unsigned long n;
#endif /* not MINIMAL_DEPEND */
	    const char * name;

	    if (!dep)
		continue;

	    if (dep->attr.flags & (SERV_DUPLET|SERV_NOSTOP))
		continue;			/* Duplet or no stop link */

#if defined(MINIMAL_RULES) && (MINIMAL_RULES != 0)
	    if (dep->attr.ref <= 0)
		continue;
#endif /* not MINIMAL_RULES */

	    if ((serv->stopp->lvl & dep->stopp->lvl) == 0)
		continue;

	    if ((name = dep->attr.script) == (char*)0)
		continue;

	    if (!mark) {
		fprintf(out, "%s:", target);
		mark = true;
	    }
#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	    for (n = 0; n < lcnt && lserv[n]; n++) {
		list_t * red;
		if (lserv[n]->attr.korder <= dep->attr.korder)
		    break;
		np_list_for_each(red, &(lserv[n])->sort.rev) {
		    req_t * other = getreq(red);
		    if (other->serv->attr.flags & SERV_DUPLET)
			continue;
		    if (other->serv->attr.ref <= 0)
			continue;
		    if ((serv->start->lvl & other->serv->start->lvl) == 0)
			continue;
		    if (!other->serv->attr.script)
			continue;
		    if (other->serv != dep)
			continue;
		    shadow = true;
		}
	    }
	    if (shadow)
		continue;
#endif /* not MINIMAL_DEPEND */
	    fprintf(out, " %s", name);

#if defined(MINIMAL_DEPEND) && (MINIMAL_DEPEND != 0)
	    if (lcnt >= sizeof(lserv)/sizeof(lserv[0]))
		continue;
	    lserv[lcnt++] = dep;
#endif /* not MINIMAL_DEPEND */
	}
	if (mark) fputc('\n', out);
    }

#ifdef USE_KILL_IN_BOOT
    fclose(halt);
#endif /* USE_KILL_IN_BOOT */
    fclose(stop);
}

/*
 * Internal logger
 */
char *myname = (char*)0;
static void _logger (const char *restrict const fmt, va_list ap);
static void _logger (const char *restrict const fmt, va_list ap)
{
    char buf[strlen(myname)+2+strlen(fmt)+1];
    strcat(strcat(strcpy(buf, myname), ": "), fmt);
    vfprintf(stderr, buf, ap);
    return;
}

/*
 * Cry and exit.
 */
void error (const char *restrict const fmt, ...)
{
    static char called;
    va_list ap;
    if (called++)
	exit (1);
    va_start(ap, fmt);
    _logger(fmt, ap);
    va_end(ap);
    popd();
    exit (1);
}

/*
 * Warn the user.
 */
void warn (const char *restrict const fmt, ...)
{
    if (silent_mode)
        return;        /* do not print warnings in silent mode */

    va_list ap;
    va_start(ap, fmt);
    _logger(fmt, ap);
    va_end(ap);
    return;
}

/*
 * Print message when verbose is enabled
 */
void info(int level, const char *fmt, ...) {
    va_list ap;
    if (level > verbose)
	goto out;
    va_start(ap, fmt);
    _logger(fmt, ap);
    va_end(ap);
out:
    return;
}

/*
 *  Check for script in list.
 */
static int curr_argc = -1;
static inline boolean chkfor(const char *restrict const script,
			     char **restrict const list, const int cnt) attribute((nonnull(1,2)));
static inline boolean chkfor(const char *restrict const script, char **restrict const list, const int cnt)
{
    boolean isinc = false;
    register int c = cnt;

    curr_argc = -1;
    while (c--) {
	if (*script != *list[c])
	    continue;
	if (!strcmp(script, list[c])) {
	    isinc = true;
	    curr_argc = c;
	    break;
	}
    }
    return isinc;
}

/*
 * Open a runlevel directory, if it not
 * exists than create one.
 */
static DIR * openrcdir(const char *restrict const rcpath) attribute((nonnull(1)));
static DIR * openrcdir(const char *restrict const rcpath)
{
   DIR * rcdir;
   struct stat st;
   int dfd;

    if (stat(rcpath, &st) < 0) {
	if (errno == ENOENT) {
	    info(1, "creating directory '%s'\n", rcpath);
	    if (!dryrun)
	        if (0 > mkdir(rcpath, (S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)))
		    error("mkdir(%s, ...) failed: %s", rcpath, strerror(errno));
	} else
	    error("can not stat(%s): %s\n", rcpath, strerror(errno));
    }

    if ((rcdir = opendir(rcpath)) == (DIR*)0) {
	if (dryrun)
	    warn ("can not opendir(%s): %s\n", rcpath, strerror(errno));
	else
	    error("can not opendir(%s): %s\n", rcpath, strerror(errno));
    }
#if defined _XOPEN_SOURCE && (_XOPEN_SOURCE - 0) >= 600
    else if ((dfd = dirfd(rcdir)) != 0) {
	(void)posix_fadvise(dfd, 0, 0, POSIX_FADV_WILLNEED);
	(void)posix_fadvise(dfd, 0, 0, POSIX_FADV_SEQUENTIAL);
    }
#endif
    return rcdir;
}

/*
 * Wrapper for regcomp(3)
 */
static inline void regcompiler(regex_t *restrict preg,
			       const char *restrict regex,
			       int cflags) attribute((always_inline,nonnull(1,2)));
static inline void regcompiler(regex_t *restrict preg, const char *restrict regex, int cflags)
{
    register int ret = regcomp(preg, regex, cflags);
    if (ret) {
	regerror(ret, preg, buf, sizeof (buf));
	regfree (preg);
	error("%s\n", buf);
    }
    return;
}

/*
 * Wrapper for regexec(3)
 */
static inline boolean regexecutor(regex_t *restrict preg,
				  const char *restrict string,
				  size_t nmatch, regmatch_t pmatch[], int eflags) attribute((nonnull(1,2)));
static inline boolean regexecutor(regex_t *preg, const char *string,
	size_t nmatch, regmatch_t pmatch[], int eflags)
{
    register int ret = regexec(preg, string, nmatch, pmatch, eflags);
    if (ret > REG_NOMATCH) {
	regerror(ret, preg, buf, sizeof (buf));
	regfree (preg);
	warn("%s\n", buf);
    }
    return (ret ? false : true);
}

/*
 * The script scanning engine.
 * We have to alloc the regular expressions first before
 * calling scan_script_defaults().  After the last call
 * of scan_script_defaults() we may free the expressions.
 */
static inline void scan_script_regalloc(void) attribute((always_inline));
static inline void scan_script_regalloc(void)
{
    regcompiler(&reg.prov,      PROVIDES,       REG_EXTENDED|REG_ICASE);
    regcompiler(&reg.req_start, REQUIRED_START, REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.req_stop,  REQUIRED_STOP,  REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.shl_start, SHOULD_START,   REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.shl_stop,  SHOULD_STOP,    REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.start_bf,  START_BEFORE,   REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.stop_af,   STOP_AFTER,     REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.def_start, DEFAULT_START,  REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.def_stop,  DEFAULT_STOP,   REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.desc,      DESCRIPTION,    REG_EXTENDED|REG_ICASE|REG_NEWLINE);
    regcompiler(&reg.interact,  INTERACTIVE,    REG_EXTENDED|REG_ICASE|REG_NEWLINE);
}

static inline void scan_script_reset(void) attribute((always_inline));
static inline void scan_script_reset(void)
{
    xreset(script_inf.provides);
    xreset(script_inf.required_start);
    xreset(script_inf.required_stop);
    xreset(script_inf.should_start);
    xreset(script_inf.should_stop);
    xreset(script_inf.start_before);
    xreset(script_inf.stop_after);
    xreset(script_inf.default_start);
    xreset(script_inf.default_stop);
    xreset(script_inf.description);
    xreset(script_inf.interactive);
}

/*
 * Return TRUE if the script is an OpenRC script.
 */

int is_openrc_job(const char *path)
{
    char buf[64];
    FILE *script = NULL;
    char *p = NULL;

    script = fopen(path, "r");
    if (script == NULL) {
        warn("Can not open script %s: %s\n", path, strerror(errno));
        return 0;
    }

    if (fgets(buf, 64, script) == NULL) {
        warn("Could not read script %s: %s\n", path, strerror(errno));
        fclose(script);
        return 0;
    }
    fclose(script);

    p = buf;
    while (*p) {
       if (isspace(*p)) {
             memmove(p , p + 1, strlen(p + 1) + 1);
	  }
          p++;
   }

    if (! strncmp(buf, "#!/sbin/openrc-run", 18))
        return 1;

    return 0;
}

/*
 * return name of upstart job if the script is a symlink to
 * /lib/init/upstart-job, or NULL if path do not point to an
 * upstart job.
 */
static char *is_upstart_job(const char *path)
{
    uint deep = 0;
    char buf[PATH_MAX+1];
    char *script = xstrdup(path);
    char *retval = basename(path);	/* GNU basename */

    buf[PATH_MAX] = '\0';

    do {
	struct stat statbuf;
	int len;

	if (deep++ > MAXSYMLINKS) {
	    errno = ELOOP;
	    warn("Can not determine upstart job name for %s: %s\n", path, strerror(errno));
	    break;
	}

	if (lstat(script, &statbuf) < 0) {
	    warn("Can not stat %s: %s\n", path, strerror(errno));
	    break;
	}

	if (!S_ISLNK(statbuf.st_mode))
	    break;

	if ((len = readlink(script, buf, sizeof(buf)-1)) < 0)
	    break;
	buf[len] = '\0';

	if (buf[0] != '/') {		/* restore relative links */
	    const char *lastslash;

	    if ((lastslash = strrchr(script, '/'))) {
		size_t dirlen = lastslash - script + 1;

		if (dirlen + len > PATH_MAX)
		    len = PATH_MAX - dirlen;

		memmove(&buf[dirlen], &buf[0], len + 1);
		memcpy(&buf[0], script, dirlen);
	    }
	}

	free(script);

	if (strcmp(buf, upstartjob_path) == 0) {
	    info(2, "script '%s' is upstart job\n", retval);
	    return strdup(retval);
	}

	script = xstrdup(buf);

    } while (1);

    free(script);

    return (char*)0;
}

#define FOUND_LSB_HEADER   0x01
#define FOUND_LSB_DEFAULT  0x02
#define FOUND_LSB_OVERRIDE 0x04
#define FOUND_LSB_UPSTART  0x08
#define FOUND_LSB_SYSTEMD  0x10
#define FOUND_LSB_OPENRC   0x20

static int o_flags = O_RDONLY;

static uchar scan_lsb_headers(const int dfd, const char *restrict const path,
			      const boolean cache, const boolean ignore) attribute((nonnull(2)));
static uchar scan_lsb_headers(const int dfd, const char *restrict const path,
			      const boolean cache, const boolean ignore)
{
    regmatch_t subloc[SUBNUM_SHD+1], *val = &subloc[SUBNUM-1], *shl = &subloc[SUBNUM_SHD-1];
    char *upstart_job = (char*)0;
    char *begin = (char*)0, *end = (char*)0;
    char *pbuf = buf;
    FILE *script;
    uchar ret = 0;
    int fd = -1;

#define provides	script_inf.provides
#define required_start	script_inf.required_start
#define required_stop	script_inf.required_stop
#define should_start	script_inf.should_start
#define should_stop	script_inf.should_stop
#define start_before	script_inf.start_before
#define stop_after	script_inf.stop_after
#define default_start	script_inf.default_start
#define default_stop	script_inf.default_stop
#define description	script_inf.description
#define interactive	script_inf.interactive

    info(2, "Loading %s\n", path);

    if (NULL != (upstart_job = is_upstart_job(path))) {
	char cmd[PATH_MAX];
	int len;
	len = snprintf(cmd, sizeof(cmd), "%s %s lsb-header", upstartjob_path, upstart_job);
	if (len < 0 || sizeof(cmd) == len)
	    error("snprintf: insufficient buffer for %s\n", path);
	if ((script = popen(cmd, "r")) == (FILE*)0)
	    error("popen(%s): %s\n", path, strerror(errno));
	ret |= FOUND_LSB_UPSTART;
    } else {
	if ((fd = xopen(dfd, path, o_flags)) < 0 || (script = fdopen(fd, "r")) == (FILE*)0)
	    error("fopen(%s): %s\n", path, strerror(errno));

#if defined _XOPEN_SOURCE && (_XOPEN_SOURCE - 0) >= 600
	(void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    }

#define COMMON_ARGS	buf, SUBNUM, subloc, 0
#define COMMON_SHD_ARGS	buf, SUBNUM_SHD, subloc, 0
    while (fgets(buf, sizeof(buf), script)) {

	/* Skip scanning above from LSB magic start */
	if (!begin) {
	    if ( (begin = strstr(buf, "### BEGIN INIT INFO")) ) {
	        /* Let the latest LSB header override the one found earlier */
	        scan_script_reset();
	    }
	    continue;
	}

	if (!provides       && regexecutor(&reg.prov,      COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
	        provides = xstrdup(pbuf+val->rm_so);
	    } else
		provides = empty;
	}
	if (!required_start && regexecutor(&reg.req_start, COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		required_start = xstrdup(pbuf+val->rm_so);
	    } else
		required_start = empty;
	}
	if (!required_stop  && regexecutor(&reg.req_stop,  COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		required_stop = xstrdup(pbuf+val->rm_so);
	    } else
		required_stop = empty;
	}
	if (!should_start && regexecutor(&reg.shl_start,   COMMON_SHD_ARGS) == true) {
	    if (shl->rm_so < shl->rm_eo) {
		*(pbuf+shl->rm_eo) = '\0';
		should_start = xstrdup(pbuf+shl->rm_so);
	    } else
		should_start = empty;
	}
	if (!should_stop  && regexecutor(&reg.shl_stop,    COMMON_SHD_ARGS) == true) {
	    if (shl->rm_so < shl->rm_eo) {
		*(pbuf+shl->rm_eo) = '\0';
		should_stop = xstrdup(pbuf+shl->rm_so);
	    } else
		should_stop = empty;
	}
	if (!start_before && regexecutor(&reg.start_bf,    COMMON_SHD_ARGS) == true) {
	    if (shl->rm_so < shl->rm_eo) {
		*(pbuf+shl->rm_eo) = '\0';
		start_before = xstrdup(pbuf+shl->rm_so);
	    } else
		start_before = empty;
	}
	if (!stop_after  && regexecutor(&reg.stop_af,      COMMON_SHD_ARGS) == true) {
	    if (shl->rm_so < shl->rm_eo) {
		*(pbuf+shl->rm_eo) = '\0';
		stop_after = xstrdup(pbuf+shl->rm_so);
	    } else
		stop_after = empty;
	}
	if (!default_start  && regexecutor(&reg.def_start, COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		default_start = xstrdup(pbuf+val->rm_so);
	    } else
		default_start = empty;
	}
#ifndef SUSE
	if (!default_stop   && regexecutor(&reg.def_stop,  COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		default_stop = xstrdup(pbuf+val->rm_so);
	    } else
		default_stop = empty;
	}
#endif
	if (!description    && regexecutor(&reg.desc,      COMMON_ARGS) == true) {
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		description = xstrdup(pbuf+val->rm_so);
	    } else
		description = empty;
	}

	if (!interactive    && regexecutor(&reg.interact,  COMMON_SHD_ARGS) == true) {
	    if (shl->rm_so < shl->rm_eo) {
		*(pbuf+shl->rm_eo) = '\0';
		interactive = xstrdup(pbuf+shl->rm_so);
	    } else
		interactive = empty;
	}

	/* Skip scanning below from LSB magic end */
	if ((end = strstr(buf, "### END INIT INFO")))
	    break;
    }
#undef COMMON_ARGS
#undef COMMON_SHD_ARGS

    if (upstart_job) {
	pclose(script);
	xreset(upstart_job);
    } else {
#if defined _XOPEN_SOURCE && (_XOPEN_SOURCE - 0) >= 600
	if (cache) {
	    off_t deep = ftello(script);
	    if (-1 != deep) {
	        (void)posix_fadvise(fd, 0, deep, POSIX_FADV_WILLNEED);
		(void)posix_fadvise(fd, deep, 0, POSIX_FADV_DONTNEED);
	    } else
	        warn("ftello(%s) failed: %s\n", path, strerror(errno));
	} else
	    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE);
#endif
	fclose(script);
    }

    if (begin && end)
	ret |= FOUND_LSB_HEADER;

    if (begin && !end) {
	char *name = basename(path);
	if (*name == 'S' || *name == 'K')
	    name += 3;
	warn("%sscript %s is broken: missing end of LSB comment.\n", ignore ? "" : "FATAL: ", name);
	if (!ignore)
	    error("exiting now!\n");
    }

    if (begin && end && (!provides || (provides == empty) ||
#ifdef SUSE
			 !required_start || !required_stop || !default_start
#else  /* not SUSE */
			 !required_start || !required_stop || !default_start || !default_stop
#endif /* not SUSE */
	))
    {
	char *name = basename(path);
	if (*name == 'S' || *name == 'K')
	    name += 3;
	warn("script %s is broken: incomplete LSB comment.\n", name);
	if (!provides)
	    warn("missing `Provides:' entry: please add.\n");
	if (provides == empty)
	    warn("missing valid name for `Provides:' please add.\n");
	if (!required_start)
	    warn("missing `Required-Start:' entry: please add even if empty.\n");
	if (!required_stop)
	    warn("missing `Required-Stop:'  entry: please add even if empty.\n");
	if (!default_start)
	    warn("missing `Default-Start:'  entry: please add even if empty.\n");
#ifndef SUSE
	if (!default_stop)
	    warn("missing `Default-Stop:'   entry: please add even if empty.\n");
#endif
    }

#undef provides
#undef required_start
#undef required_stop
#undef should_start
#undef should_stop
#undef start_before
#undef stop_after
#undef default_start
#undef default_stop
#undef description
#undef interactive
    return ret;
}

/*
 * Follow symlinks, return the basename of the file pointed to by
 * symlinks or the basename of the current path if no symlink.
 */
static char * scriptname(int dfd, const char *restrict const path, char **restrict first) attribute((malloc,nonnull(2)));
static char * scriptname(int dfd, const char *restrict const path, char **restrict first)
{
    uint deep = 0;
    char linkbuf[PATH_MAX+1];
    char *script = xstrdup(path);

    strncpy(linkbuf, script, sizeof(linkbuf)-1);
    linkbuf[PATH_MAX] = '\0';

    do {
	struct stat st;
	int linklen;

	if (deep++ > MAXSYMLINKS) {
	    errno = ELOOP;
	    warn("Can not determine script name for %s: %s\n", path, strerror(errno));
	    break;
	}

	if (xlstat(dfd, script, &st) < 0) {
	    warn("Can not stat %s: %s\n", script, strerror(errno));
	    break;
	}

	if (!S_ISLNK(st.st_mode))
	    break;

	if ((linklen = xreadlink(dfd, script, linkbuf, sizeof(linkbuf)-1)) < 0)
	    break;
	linkbuf[linklen] = '\0';

	if (linkbuf[0] != '/') {	/* restore relative links */
	    const char *lastslash;

	    if ((lastslash = strrchr(script, '/'))) {
		size_t dirname_len = lastslash - script + 1;

		if (dirname_len + linklen > PATH_MAX)
		    linklen = PATH_MAX - dirname_len;

		memmove(&linkbuf[dirname_len], &linkbuf[0], linklen + 1);
		memcpy(&linkbuf[0], script, dirname_len);
	    }
	}

	free(script);
	script = xstrdup(linkbuf);

	if (deep == 1 && first)
	    *first = xstrdup(basename(linkbuf));

    } while (1);

    free(script);
    script = xstrdup(basename(linkbuf));

    return script;
}

static uchar load_overrides(const char *restrict const dir,
			    const char *restrict const name,
			    const boolean cache, const boolean ignore) attribute((nonnull(1,2)));
static uchar load_overrides(const char *restrict const dir,
			    const char *restrict const name,
			    const boolean cache, const boolean ignore)
{
    uchar ret = 0;
    char fullpath[PATH_MAX+1];
    struct stat statbuf;
    int n;

    n = snprintf(&fullpath[0], sizeof(fullpath), "%s%s/%s", (root && !set_override) ? root : "", dir, name);
    if (n >= (int)sizeof(fullpath) || n < 0)
	error("snprintf(): %s\n", strerror(errno));

    if (stat(fullpath, &statbuf) == 0 && S_ISREG(statbuf.st_mode))
	ret = scan_lsb_headers(-1, fullpath, cache, ignore);
    if (ret & FOUND_LSB_HEADER)
	ret |= FOUND_LSB_OVERRIDE;
    return ret;
}

static uchar scan_script_defaults(int dfd, const char *const restrict path,
				  const char *const restrict override_path,
				  char **restrict first,
				  const boolean cache, const boolean ignore) attribute((nonnull(2,3)));
static uchar scan_script_defaults(int dfd, const char *restrict const path,
				  const char *restrict const override_path,
				  char **restrict first,
				  const boolean cache, const boolean ignore)
{
    char * name = scriptname(dfd, path, first);
    uchar ret = 0;
    char *upstart_job = (char*)0;

    if (!name)
	return ret;

    /* Reset old results */
    scan_script_reset();

#ifdef SUSE
    /* Common script ... */
    if (!strcmp(name, "halt")) {
	ret |= (FOUND_LSB_HEADER|FOUND_LSB_DEFAULT);
	goto out;
    }

    /* ... and its link */
    if (!strcmp(name, "reboot")) {
	ret |= (FOUND_LSB_HEADER|FOUND_LSB_DEFAULT);
	goto out;
    }

    /* Common script for single mode */
    if (!strcmp(name, "single")) {
	ret |= (FOUND_LSB_HEADER|FOUND_LSB_DEFAULT);
	goto out;
    }
#endif /* SUSE */

#ifdef WANT_SYSTEMD
    if (systemd) {
	const char *serv;
	serv = path;
	if (strncmp("boot.", serv, 5) == 0)
	    serv += 5;
	if (is_overridden_by_systemd(serv)) {
	    ret |= FOUND_LSB_SYSTEMD;
	}
    }
#endif /* WANT_SYSTEMD */

    if (is_openrc_job(path)) {
        ret |= FOUND_LSB_OPENRC;
        goto out;
    }

    if (NULL != (upstart_job = is_upstart_job(path))) {
	xreset(upstart_job);
	/*
	 * Do not override the upstarts defaults, if we allow this
	 * we have to change name to the link name otherwise the
	 * name is always "upstart-job"
	 */
	ret |= scan_lsb_headers(dfd, path, cache, ignore);
	if (ret & FOUND_LSB_UPSTART)
	    goto out;
    }

    /*
     * Allow host-specific overrides to replace the content in the
     * init.d scripts
     */
    ret |= load_overrides(override_path, name, cache, ignore);
    if (ret & FOUND_LSB_OVERRIDE)
	goto out;

    /*
     * Load third-party-specific values if the override file exist
     */
    ret |= load_overrides("/usr/share/insserv/overrides", name, cache, ignore);
    if (ret & FOUND_LSB_OVERRIDE)
	goto out;

    /*
     * Replace with headers from the script itself
     */
    ret |= scan_lsb_headers(dfd, path, cache, ignore);
out:
    ret |= FOUND_LSB_DEFAULT;
    free(name);
    return ret;
}

static inline void scan_script_regfree() attribute((always_inline));
static inline void scan_script_regfree()
{
    regfree(&reg.prov);
    regfree(&reg.req_start);
    regfree(&reg.req_stop);
    regfree(&reg.shl_start);
    regfree(&reg.shl_stop);
    regfree(&reg.start_bf);
    regfree(&reg.stop_af);
    regfree(&reg.def_start);
    regfree(&reg.def_stop);
    regfree(&reg.desc);
    regfree(&reg.interact);
}

/*
 * Two helpers for runlevel bits and strings.
 */
ushort str2lvl(const char *restrict lvl)
{
    char * token, *tmp = strdupa(lvl);
    ushort ret = 0;

    if (!tmp)
	error("%s", strerror(errno));

    while ((token = strsep(&tmp, delimeter))) {
	if (!*token || strlen(token) != 1)
	    continue;
	if (!strpbrk(token, "0123456sSbB"))
	    continue;

	ret |= map_key_to_lvl(*token);
    }

    return ret;
}

char * lvl2str(const ushort lvl)
{
    char * ptr, * last;
    char str[20];
    int num;
    uint bit = 0x001;

    last = ptr = &str[0];
    memset(ptr, '\0', sizeof(str));
    for (num = 0; num < RUNLEVELS; num++) {
	if (bit & lvl) {
	    if (ptr > last)
		*ptr++ = ' ';
	    last = ptr;
	    if (LVL_NORM & bit)
		*ptr++ = num + 48;
#ifdef SUSE
	    else if (LVL_SINGLE & bit)
		*ptr++ = 'S';
	    else if (LVL_BOOT & bit)
		*ptr++ = 'B';
#else  /* not SUSE */
	    else if (LVL_BOOT & bit)
		*ptr++ = 'S';
#endif /* not SUSE */
	    else
		error("Wrong runlevel %d\n", num);
	}
	bit <<= 1;
    }
    if (strlen(str) == 0)
	return (char*)0;
    return xstrdup(str);
}

/*
 * Scan current service structure
 */
static void scan_script_locations(const char *const restrict path,
				  const char *const restrict override_path,
				  const boolean ignore) attribute((nonnull(1,2)));
static void scan_script_locations(const char *const path, const char *const override_path,
				  const boolean ignore)
{
    int runlevel;

    pushd(path);
    for (runlevel = 0; runlevel < RUNLEVELS; runlevel++) {
	const char * rcd = (char*)0;
	struct stat st_script;
	struct dirent *d;
	DIR  * rcdir;
	char * token;
	int dfd;

	rcd = map_runlevel_to_location(runlevel);

	rcdir = openrcdir(rcd);		/* Creates runlevel directory if necessary */
	if (rcdir == (DIR*)0)
	    break;
	if ((dfd = dirfd(rcdir)) < 0) {
	    closedir(rcdir);
	    break;
	}
	pushd(rcd);

	while ((d = readdir(rcdir)) != (struct dirent*)0) {
	    char * name = (char *)0;
	    char * ptr = d->d_name;
	    service_t * first;
	    char * begin;	/* Remember address of ptr handled by strsep() */
	    char order;
	    uchar lsb;
	    char type;

	    if (*ptr != 'S' && *ptr != 'K')
		continue;
	    type = *ptr;
	    ptr++;

	    if (strspn(ptr, "0123456789") < 2)
		continue;
	    order = atoi(ptr);
	    ptr += 2;

	    if (xstat(dfd, d->d_name, &st_script) < 0) {
		xremove(dfd, d->d_name);	/* dangling sym link */
		continue;
	    }

	    lsb = scan_script_defaults(dfd, d->d_name, override_path, &name, false, ignore);
	    if (!name) {
		warn("warning: script is corrupt or invalid: %s/%s%s\n", path, rcd, d->d_name);
		continue;
	    }

	    if (!script_inf.provides || script_inf.provides == empty)
		script_inf.provides = xstrdup(ptr);

#ifndef SUSE
	    if (!lsb) {
	        script_inf.required_start = xstrdup(DEFAULT_DEPENDENCY);
		script_inf.required_stop  = xstrdup(DEFAULT_DEPENDENCY);
	    }
#endif /* not SUSE */

	    first = (service_t*)0;
	    begin = script_inf.provides;
	    while ((token = strsep(&begin, delimeter)) && *token) {
		service_t * service;

		if (*token == '$') {
		    warn("script %s provides system facility %s, skipped!\n", d->d_name, token);
		    continue;
		}
		if (*token == '#') {
		    warn("script %s provides facility %s with comment sign, skipped!\n", d->d_name, token);
		    continue;
		}

		service = current_structure(token, order, runlevel, type);

		if (first)
		    nickservice(first, service);
		else
		    first = service;

		if (!makeprov(service, name))
		    continue;

		++service->attr.ref;		/* May enabled in several levels */

		if (service->attr.flags & SERV_KNOWN)
		    continue;
		service->attr.flags |= (SERV_KNOWN|SERV_ENABLED);

		if (!lsb)
		    service->attr.flags |= SERV_NOTLSB;

		if ((lsb & (FOUND_LSB_HEADER|FOUND_LSB_OPENRC)) == 0) {
		    if ((lsb & (FOUND_LSB_DEFAULT|FOUND_LSB_OVERRIDE)) == 0)
		        warn("warning: script '%s' missing LSB tags and overrides\n", d->d_name);
		    else
  		        warn("warning: script '%s' missing LSB tags\n", d->d_name);
		}

		if (script_inf.required_start && script_inf.required_start != empty) {
		    rememberreq(service, REQ_MUST, script_inf.required_start);
#ifdef USE_COMPAT_EMPTY
		    if (!script_inf.required_stop || script_inf.required_stop == empty)
			script_inf.required_stop = xstrdup(script_inf.required_start);
#endif /* USE_COMPAT_EMPTY */
		}
		if (script_inf.should_start && script_inf.should_start != empty) {
		    rememberreq(service, REQ_SHLD, script_inf.should_start);
#ifdef USE_COMPAT_EMPTY
		    if (!script_inf.should_stop || script_inf.should_stop == empty)
			script_inf.should_stop = xstrdup(script_inf.should_start);
#endif /* USE_COMPAT_EMPTY */
		}
		if (script_inf.start_before && script_inf.start_before != empty) {
		    reversereq(service, REQ_SHLD, script_inf.start_before);
#ifdef USE_COMPAT_EMPTY
		    if (!script_inf.stop_after || script_inf.stop_after == empty)
			script_inf.stop_after = xstrdup(script_inf.start_before);
#endif /* USE_COMPAT_EMPTY */
		}
		if (script_inf.required_stop && script_inf.required_stop != empty) {
		    rememberreq(service, REQ_MUST|REQ_KILL, script_inf.required_stop);
		}
		if (script_inf.should_stop && script_inf.should_stop != empty) {
		    rememberreq(service, REQ_SHLD|REQ_KILL, script_inf.should_stop);
		}
		if (script_inf.stop_after && script_inf.stop_after != empty) {
		    reversereq(service, REQ_SHLD|REQ_KILL, script_inf.stop_after);
		}
		if (script_inf.interactive && 0 == strcmp(script_inf.interactive, "true")) {
		    service->attr.flags |= SERV_INTRACT;
		}
	    }

	    if (name) 
		xreset(name);

	    scan_script_reset();

	}	/* while ((token = strsep(&begin, delimeter)) && *token) */

	popd();
	closedir(rcdir);
    }
    popd();
    return;
}

/*
 * The /etc/insserv.conf scanning engine.
 */
static void scan_conf_file(const char *restrict file) attribute((nonnull(1)));
static void scan_conf_file(const char *restrict file)
{
    regmatch_t subloc[SUBCONFNUM], *val = (regmatch_t*)0;
    FILE *conf;

    info(2, "Loading %s\n", file);

    do {
	const char * fptr = file;
	if (*fptr == '/')
	    fptr++;
	/* Try relativ location first */
	if ((conf = fopen(fptr, "r")))
	    break;
	/* Try absolute location */
	if ((conf = fopen(file, "r")))
	    break;
	goto err;
    } while (1);

    while (fgets(buf, sizeof(buf), conf)) {
	char *pbuf = &buf[0];
	if (*pbuf == '#')
	    continue;
	if (*pbuf == '\n')
	    continue;
	if (regexecutor(&creg.isysfaci, buf, SUBCONFNUM, subloc, 0) == true) {
	    char * virt = (char*)0, * real = (char*)0;
	    val = &subloc[SUBCONF - 1];
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		virt = pbuf+val->rm_so;
	    }
	    val = &subloc[SUBCONFNUM - 1];
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		real = pbuf+val->rm_so;
	    }
	    if (virt) {
		list_t *ptr;
		list_t *r_list = (list_t*)0;
		list_for_each(ptr, sysfaci_start) {
		    if (!strcmp(getfaci(ptr)->name, virt)) {
			r_list = &getfaci(ptr)->replace;
			break;
		    }
		}
		if (!r_list) {
		    faci_t *restrict this;
		    if (posix_memalign((void*)&this, sizeof(void*), alignof(faci_t)) != 0)
			error("%s", strerror(errno));
		    else {
			r_list = &this->replace;
			initial(r_list);
			insert(&this->list, sysfaci_start->prev);
			this->name = xstrdup(virt);
		    }
		}
		if(real) {
		    char *token;
		    while ((token = strsep(&real, delimeter))) {
			repl_t *restrict subst;
			string_t *r = (string_t*)0;
			if (posix_memalign((void*)&subst, sizeof(void*), alignof(repl_t)) != 0)
			    error("%s", strerror(errno));
			insert(&subst->r_list, r_list->prev);
			subst->flags = 0;
			list_for_each(ptr, facistr_start) {
			    if (strcmp(getfstr(ptr)->name, token) == 0) {
				r = getfstr(ptr);
				break;
			    }
			}
			if (!r) {
			    if (posix_memalign((void*)&r, sizeof(void*), alignof(string_t)+strsize(token)) != 0)
				error("%s", strerror(errno));
			    r->ref = 1;
			    insert(&r->s_list, facistr_start);
			    r->name = ((char*)r)+alignof(string_t);
			    strcpy(r->name, token);
			} else
			    r->ref++;
			subst->addr = r;
			subst->name = r->name;
		    }
		}
	    }
	}
	if (regexecutor(&creg.isactive, buf, SUBCONFNUM, subloc, 0) == true) {
	    char * key = (char*)0, * servs = (char*)0;
	    val = &subloc[SUBCONF - 1];
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		key = pbuf+val->rm_so;
	    }
	    val = &subloc[SUBCONFNUM - 1];
	    if (val->rm_so < val->rm_eo) {
		*(pbuf+val->rm_eo) = '\0';
		servs = pbuf+val->rm_so;
	    }
	    if (key && *key == '<' && servs && *servs) {
		if (!strncmp("<interactive>", key, strlen(key))) {
		    char * token;
		    while ((token = strsep(&servs, delimeter))) {
			service_t *service = addservice(token);
			service = getorig(service);
			service->attr.flags |= SERV_INTRACT;
		    }
		}
	    }
	}
    }

    fclose(conf);
    return;
err:
    warn("fopen(%s): %s\n", file, strerror(errno));
}

static int cfgfile_filter(const struct dirent *restrict d) attribute((nonnull(1)));
static int cfgfile_filter(const struct dirent *restrict d)
{
    boolean ret = false;
    const char * name = d->d_name;
    const char * end;

    if (!name || (*name == '\0'))
	goto out;
    if (*name == '.')
	goto out;
    if ((end = strrchr(name, '.'))) {
	end++;
	if (!strncmp(end, "rpm", 3)	|| /* .rpmorig, .rpmnew, .rmpsave, ... */
	    !strncmp(end, "ba", 2)	|| /* .bak, .backup, ... */
#ifdef SUSE
	    !strcmp(end,  "local")	|| /* .local are sourced by the basename */
#endif /* not SUSE */
	    !strcmp(end,  "old")	||
	    !strcmp(end,  "new")	||
	    !strcmp(end,  "org")	||
	    !strcmp(end,  "orig")	||
	    !strncmp(end, "dpkg", 4)	|| /* .dpkg-old, .dpkg-new ... */
            !strncmp(end, "ucf", 3)     || /* .ucf-old, .ucf-dist ... */
	    !strcmp(end,  "save")	||
	    !strcmp(end,  "swp")	|| /* Used by vi like editors */
	    !strcmp(end,  "core"))	   /* modern core dump */
	{
	    goto out;
	}
        /* check loaded filters */
        else if (file_filters)
        {
            boolean found = false;
            int index = 0;
            while ( (file_filters[index]) && (! found) )
            {
                if (! strcmp(end, file_filters[index]) )
                   found = true;
                else
                   index++;
            }
            if (found)
               goto out;
        }
    }
    if ((end = strrchr(name, ','))) {
	end++;
	if (!strcmp(end,  "v"))		  /* rcs-files */
	    goto out;
    }
    ret = true;
out:
    return (int)ret;
}

static void scan_conf(const char *restrict file) attribute((nonnull(1)));
static void scan_conf(const char *restrict file)
{
    struct dirent** namelist = (struct dirent**)0;
    char path[PATH_MAX+1];
    int n;

    regcompiler(&creg.isysfaci,  CONFLINE, REG_EXTENDED|REG_ICASE);
    regcompiler(&creg.isactive, CONFLINE2, REG_EXTENDED|REG_ICASE);

    n = snprintf(&path[0], sizeof(path), "%s%s",   (root && !set_insconf) ? root : "", file);
    if (n >= (int)sizeof(path) || n < 0)
	error("snprintf(): %s\n", strerror(errno));

    scan_conf_file(path);

    n = snprintf(&path[0], sizeof(path), "%s%s.d", (root && !set_insconf) ? root : "", file);
    if (n >= (int)sizeof(path) || n < 0)
	error("snprintf(): %s\n", strerror(errno));

    n = scandir(path, &namelist, cfgfile_filter, alphasort);
    if(n > 0) {
	while(n--) {
	    struct stat st;
	    char buf[PATH_MAX+1];
	    int r;

	    r = snprintf(&buf[0], sizeof(buf), "%s/%s", path, namelist[n]->d_name);
	    if (r >= (int)sizeof(buf) || r < 0)
		error("snprintf(): %s\n", strerror(errno));

	    if ((stat(buf, &st) < 0) || !S_ISREG(st.st_mode))
	        continue;

	    scan_conf_file(buf);

	    free(namelist[n]);
	}
    }

    if (namelist)
	free(namelist);

    regfree(&creg.isysfaci);
    regfree(&creg.isactive);
}

#ifdef WANT_SYSTEMD
/*
 * Maps between systemd and SystemV
 */
static const char* sdmap[] = {
    "$local-fs",	"$local_fs",
    "$remote-fs",	"$remote_fs",
    "cryptsetup",	"boot.crypto-early",
    "udev",		"boot.udev",
    "multipathd",	"boot.multipath",
    "loadmodules",	"boot.loadmodules",
    "device-mapper",	"boot.device-mapper",
    "sysctl",		"boot.sysctl",
    "fsck-root",	"boot.rootfsck",
    "localfs",		"boot.localfs"
};

#endif /* WANT_SYSTEMD */

#ifdef WANT_SYSTEMD
/*
 *  Here the systemd targets are imported as system facilities 
 */
static void import_systemd_facilities(void)
{
    list_t *ptr;
    list_for_each(ptr, &sdservs) {
	sdserv_t *sdserv = list_entry(ptr, sdserv_t, s_list);
	const char *facilitiy;
	list_t *r_list;
	list_t *iptr;
	int n;

	if (*sdserv->name != '$')
	    continue;

	facilitiy = sdserv->name;
	for (n = 0; n < (int)(sizeof(sdmap)/sizeof(sdmap[0])); n += 2) {
	    if (strcmp(sdmap[n], facilitiy) == 0) {
		facilitiy = sdmap[n+1];
		break;
	    }
	}

	r_list = (list_t*)0;
	np_list_for_each(iptr, sysfaci_start) {
	    if (strcmp(getfaci(iptr)->name, facilitiy) == 0) {
		r_list = &getfaci(iptr)->replace;
		break;
	    }
	}
	if (!r_list) {
	    faci_t *restrict this;
	    if (posix_memalign((void*)&this, sizeof(void*), alignof(faci_t)) != 0)
		error("%s", strerror(errno));
	    else {
		r_list = &this->replace;
		initial(r_list);
		insert(&this->list, sysfaci_start->prev);
		this->name = xstrdup(facilitiy);
	    }
	}

	np_list_for_each(iptr, &sdserv->a_list) {
	    ally_t *ally = list_entry(iptr, ally_t, a_list);
	    repl_t *restrict subst;
	    string_t *r = (string_t*)0;
	    const char *token;
	    list_t *fptr;

	    if (ally->flags & SDREL_CONFLICTS)
		continue;
	    if (ally->flags & SDREL_BEFORE)
		continue;

	    token = ally->serv->name;
	    for (n = 0; n < (int)(sizeof(sdmap)/sizeof(sdmap[0])); n += 2) {
		if (strcmp(sdmap[n], token) == 0) {
		    token = sdmap[n+1];
		    break;
		}
	    }

	    if (posix_memalign((void*)&subst, sizeof(void*), alignof(repl_t)) != 0)
		error("%s", strerror(errno));
	    insert(&subst->r_list, r_list->prev);
	    subst->flags = 0;
	    np_list_for_each(fptr, facistr_start) {
		if (strcmp(getfstr(fptr)->name, token) == 0) {
		    r = getfstr(fptr);
		    break;
		}
	    }
	    if (!r) {
		if (posix_memalign((void*)&r, sizeof(void*), alignof(string_t)+strsize(token)) != 0)
		    error("%s", strerror(errno));
		r->ref = 1;
		insert(&r->s_list, facistr_start);
		r->name = ((char*)r)+alignof(string_t);
		strcpy(r->name, token);
	    } else
		    r->ref++;
	    subst->addr = r;
	    subst->name = r->name;
	}
    }
}

/*
 *  Here the systemd servies are imported as services
 */
static void import_systemd_services(void)
{
    list_t *ptr;
    list_for_each(ptr, &sdservs) {
	sdserv_t *sdserv = list_entry(ptr, sdserv_t, s_list);
	const char *this;
	list_t *aptr;
	int n;

	if (*sdserv->name == '$')
	    continue;

	this = sdserv->name;
	for (n = 0; n < (int)(sizeof(sdmap)/sizeof(sdmap[0])); n += 2) {
	    if (strcmp(sdmap[n], this) == 0) {
		this = sdmap[n+1];
		break;
	    }
	}

	np_list_for_each(aptr, &sdserv->a_list) {
	    ally_t *ally = list_entry(aptr, ally_t, a_list);
	    service_t * service;
	    const char *token;

	    if (ally->flags & SDREL_CONFLICTS)
		continue;
	    if (ally->flags & SDREL_BEFORE)
		continue;

	    token = ally->serv->name;
	    for (n = 0; n < (int)(sizeof(sdmap)/sizeof(sdmap[0])); n += 2) {
		if (strcmp(sdmap[n], token) == 0) {
		    token = sdmap[n+1];
		    break;
		}
	    }
	    service = addservice(this);
	    rememberreq(service, (ally->flags & SDREL_WANTS) ? REQ_SHLD : REQ_MUST, token);
	    service->attr.flags |= SERV_SYSTEMD;
	}
    }
}

#endif /* WANT_SYSTEMD */

static void expand_faci(list_t *restrict rlist, list_t *restrict head,
			int *restrict deep) attribute((noinline,nonnull(1,2,3)));
static void expand_faci(list_t *restrict rlist, list_t *restrict head, int *restrict deep)
{
    repl_t *rent = getrepl(rlist);
    list_t *tmp, *safe, *ptr = (list_t*)0;

    list_for_each(tmp, sysfaci_start) {
	if (!strcmp(getfaci(tmp)->name, rent->name)) {
	    ptr = &getfaci(tmp)->replace;
	    break;
	}
    }

    if (!ptr || list_empty(ptr))
	goto out;

    list_for_each_safe(tmp, safe, ptr) {
	repl_t *rnxt = getrepl(tmp);
	if (rnxt->flags & 0x0001) {
	    error("Loop detected during expanding system facilities in the insserv.conf file(s): %s %s\n",
		  rnxt->name, getrepl(head->prev)->name);
	}
	if (*rnxt->name == '$') {
	    if (*deep > 10) {
		warn("The nested level of the system facilities in the insserv.conf file(s) is to large\n");
		goto out;
	    }
	    (*deep)++;
	    rnxt->flags |= 0x0001;
	    expand_faci(tmp, head, deep);
	    rnxt->flags &= ~0x0001;
	    (*deep)--;
	} else if (*deep >= 0) {
	    repl_t *restrict subst;
	    if (posix_memalign((void*)&subst, sizeof(void*), alignof(repl_t)) != 0)
		error("%s", strerror(errno));
	    insert(&subst->r_list, head->prev);
	    subst->addr = rnxt->addr;
	    subst->name = rnxt->name;
	    subst->addr->ref++;
	}
    }
out:
    return;
}

static inline void expand_conf(void)
{
    list_t *ptr;
    list_for_each(ptr, sysfaci_start) {
	list_t *rlist, *safe, *head = &getfaci(ptr)->replace;
	list_for_each_safe(rlist, safe, head) {
	    repl_t *tmp = getrepl(rlist);
	    if (*tmp->name == '$') {
		int deep = 0;
		expand_faci(rlist, head, &deep);
		delete(rlist);
		if (--(tmp->addr->ref) <= 0)
		    free(tmp->addr);
		free(tmp);
	    }
	}
    }
}

/*
 * Scan for a Start or Kill script within a runlevel directory.
 * We start where we leave the directory, the upper level
 * has to call rewinddir(3) if necessary.
 */
static inline char * scan_for(DIR *const restrict rcdir,
			      const char *const restrict script,
			      const char type) attribute((always_inline,nonnull(1,2)));
static inline char * scan_for(DIR *const rcdir,
			      const char *const script, const char type)
{
    struct dirent *d;
    char * ret = (char*)0;

    while ((d = readdir(rcdir)) != (struct dirent*)0) {
	char * ptr = d->d_name;

	if (*ptr != type)
	    continue;
	ptr++;

	if (strspn(ptr, "0123456789") < 2)
	    continue;
	ptr += 2;

	if (!strcmp(ptr, script)) {
	    ret = d->d_name;
	    break;
	}
    }
    return ret;
}

#ifdef SUSE
/*
 * A simple command line checker of the parent process to determine if this is
 * a sub process "/bin/sh" forked off for executing a temporary file for %preun,
 * %postun, %pre, or %post scriptlet.
 */
static inline boolean underrpm(void)
{
    boolean ret = false;
    boolean mnt = true;
    const pid_t pp = getppid();
    char buf[PATH_MAX], *argv[3], *ptr;
# if defined(USE_RPMLIB) && (USE_RPMLIB > 0)
    char *tmppath, *shell;
# endif /* USE_RPMLIB */
    int argc, fd;
    ssize_t len;

    snprintf(buf, sizeof(buf)-1, "/proc/%lu/cmdline", (unsigned long)pp);
    do {
	if ((fd = open(buf, O_NOCTTY|O_RDONLY)) >= 0)
	    break;
	if (!mnt || (errno != ENOENT))
	    goto out;
	if (mount("proc", "/proc", "proc", 0, NULL) < 0)
	    error ("underrpm() can not mount /proc: %s\n", strerror(errno));
	mnt = false;
    } while (1);

    memset(buf, '\0', sizeof(buf));
    if ((len = read(fd , buf, sizeof(buf)-1)) < 0)
	goto out;

    ptr = &buf[0];
    argc = 0;
    do {
	argv[argc++] = ptr;
	if (argc > 2)
	    break;
	if ((len = len - (ssize_t)(ptr - &buf[0])) < 0)
	    break;
    } while ((ptr = memchr(ptr, '\0', len)) && *(++ptr));

    if (argc != 3)
	goto out;

# if defined(USE_RPMLIB) && (USE_RPMLIB > 0)
    rpmReadConfigFiles(NULL, NULL);
    rpmFreeRpmrc();

    if ((shell = rpmExpand("%_buildshell", NULL)) == NULL)
	shell = xstrdup("/bin/sh");

    if (strncmp(argv[0], shell, strlen(shell)) != 0) {
	free(shell);
	goto out;
    }
    free(shell);

    if ((tmppath = rpmExpand("%_tmppath", NULL)) == NULL)
	tmppath = xstrdup("/var/tmp");

    if (strncmp(argv[1], tmppath, strlen(tmppath)) != 0) {
	free(tmppath);
	goto out;
    }

    len = strlen(tmppath);
    free(tmppath);

    ptr = argv[1];
    if (strncmp(ptr + len, "/rpm-tmp.", 9) != 0)
	goto out;
# else  /* not USE_RPMLIB */
    if ((strcmp(argv[0], "/bin/sh") != 0) &&
	(strcmp(argv[0], "/bin/bash") != 0))
	goto out;

    if ((strncmp(argv[1], "/var/tmp/rpm-tmp.", 17) != 0) &&
	(strncmp(argv[1], "/usr/tmp/rpm-tmp.", 17) != 0) &&
	(strncmp(argv[1], "/tmp/rpm-tmp.", 13) != 0))
	goto out;
# endif /* not USE_RPMLIB */
    if ((argc = atoi(argv[2])) >= 0 && argc <= 2)
	ret = true;
out:
    if (fd >= 0)
	close(fd);
    if (!mnt)
	umount("/proc");

    return ret;
}
#endif /* SUSE */

#ifdef WANT_SYSTEMD

/*
 * Systemd integration
 */
static boolean is_overridden_by_systemd(const char *service) {
    char *p;
    boolean ret = false;

    if (asprintf(&p, SYSTEMD_SERVICE_PATH "/%s.service", service) < 0)
	error("asprintf(): %s\n", strerror(errno));

    if (access(p, F_OK) >= 0)
	ret = true;
    free(p);
    return ret;
}

static void forward_to_systemd (const char *initscript, const char *verb, boolean alternative_root) {
    const char *name;

    if (initscript == NULL)
	return;

    if (strncmp("boot.",initscript,5) == 0)
	name = initscript+5;
    else
	name = initscript;

    if (is_overridden_by_systemd (name)) {
	char *p;
	int err = 0;

	if (alternative_root && root)
	    err = asprintf (&p, "/bin/systemctl --quiet --no-reload --root %s %s %s.service", root, verb, name);
	else {
	    struct statfs stfs;
	    if (statfs("/sys/fs/cgroup/systemd", &stfs) < 0 && errno != ENOENT)
		error("statfs(): %s\n", strerror(errno));
	    if (errno == 0 && stfs.f_type == CGROUP_SUPER_MAGIC)
		err = asprintf (&p, "/bin/systemctl --quiet %s %s.service", verb, name);
	    else
		err = asprintf (&p, "/bin/systemctl --quiet --no-reload %s %s.service", verb, name);
	}
	if (err < 0)
	    error("asprintf(): %s\n", strerror(errno));

	warn("Note: sysvinit service %s is shadowed by systemd %s.service,\nForwarding request to '%s'.\n", initscript, name, p);
	if (!dryrun)
	    err = system(p);
	if (err < 0)
	    warn("Failed to forward service request to systemctl: %m\n");
	else if (err > 0)
	    warn("Forward service request to systemctl returned error status : %d\n",err);
	free (p);
    }
}

#endif /* WANT_SYSTEMD */

/* See if the start and stop runlevels of a script use the same
   stop or stop levels.
   Returns true if overlap is found and false is none is found.
*/
boolean Start_Stop_Overlap(char *start_levels, char *stop_levels)
{
   boolean found_overlap = false;
   int string_index = 0;
   char *found;

   if (!start_levels || !stop_levels) return false;

   while (start_levels[string_index])   /* go to end of string */
   {
       if (start_levels[string_index] != ' ') /* skip spaces */
       {
           found = strchr(stop_levels, start_levels[string_index]);
           if (found)
              found_overlap = true;
       }
       string_index++;
   }
   return found_overlap;
}


static struct option long_options[] =
{
    {"verbose",	    0, (int*)0, 'v'},
    {"config",	    1, (int*)0, 'c'},
    {"dryrun",	    0, (int*)0, 'n'},
    {"dry-run",	    0, (int*)0, 'n'},
    {"default",	    0, (int*)0, 'd'},
    {"remove",	    0, (int*)0, 'r'},
    {"force",	    0, (int*)0, 'f'},
    {"insserv-dir", 1, (int*)0, 'i'},
    /* {"legacy-path", 0, (int*)0, 'l'}, */
    {"path",	    1, (int*)0, 'p'},
    {"override",    1, (int*)0, 'o'},
    {"upstart-job", 1, (int*)0, 'u'},
    {"silent",      0, (int*)0, 'q'},
    {"recursive",   0, (int*)0, 'e'},
    {"showall",	    0, (int*)0, 's'},
    {"show-all",    0, (int*)0, 's'},
    {"help",	    0, (int*)0, 'h'},
    { 0,	    0, (int*)0,  0 },
};

static void help(const char *restrict const name) attribute((nonnull(1)));
static void help(const char *restrict const  name)
{
    printf("Usage: %s [<options>] [init_script|init_directory]\n", name);
    printf("Available options:\n");
    printf("  -h, --help       This help.\n");
    printf("  -r, --remove     Remove the listed scripts from all runlevels.\n");
    printf("  -f, --force      Ignore if a required service is missed.\n");
    printf("  -v, --verbose    Provide information on what is being done.\n");
    printf("  -q, --silent     Do not print warnings, only fatal errors.\n");
    /* printf("  -l, --legacy-path  Place dependency files in /etc/init.d instead of /lib/insserv.\n"); */
    printf("  -i, --insserv-dir  Place dependency files in a location other than /lib/insserv\n");
    printf("  -p <path>, --path <path>  Path to replace " INITDIR ".\n");
    printf("  -o <path>, --override <path> Path to replace " OVERRIDEDIR ".\n");
    printf("  -c <config>, --config <config>  Path to config file.\n");
    printf("  -n, --dry-run     Do not change the system, only talk about it.\n");
    printf("  -s, --show-all    Output runlevel and sequence information.\n");
    printf("  -u <path>, --upstart-job <path> Path to replace existing upstart job path.\n");
    printf("  -e, --recursive  Expand and enable all required services.\n");
    printf("  -d, --default    Use default runlevels a defined in the scripts\n");
}


/*
 * Do the job.
 */
int main (int argc, char *argv[])
{
    DIR * initdir;
    struct dirent *d;
    struct stat st_script;
    /* extension char * argr[argc]; */
    char * argr[argc]; 
    char * path = INITDIR;
    char * override_path = OVERRIDEDIR;
    char * insconf = INSCONF;
    const char *const ipath = path;
    int runlevel, c, dfd;
    boolean del = false;
    boolean defaults = false;
    boolean ignore = false;
    boolean loadarg = false;
    boolean recursive = false;
    boolean showall = false;
    boolean waserr = false;
    /* boolean legacy_path = false; */
    boolean free_dependency_path = false;
    boolean overlap;

    myname = basename(*argv);

#ifdef SUSE
    if (underrpm())
	ignore = true;
#endif /* SUSE */

    if (getuid() == (uid_t)0)
	o_flags |= O_NOATIME;

    for (c = 0; c < argc; c++)
	argr[c] = (char*)0;

    while ((c = getopt_long(argc, argv, "c:dfrhqvni:o:p:u:es", long_options, (int *)0)) != -1) {
	size_t l;
	switch (c) {
	    case 'c':
		if (optarg == (char*)0 || *optarg == '\0')
		    goto err;
		insconf = optarg;
		set_insconf = true;
		break;
	    case 'd':
		defaults = true;
		break;
	    case 'r':
		del = true;
		break;
	    case 'f':
		ignore = true;
		break;
            case 'q':
                silent_mode = true;
                break;
	    case 'v':
		verbose ++;
		break;
            /*
            case 'l':
                dependency_path = LEGACY_DEPENDENCY_PATH;
                legacy_path = true;
                break;
            */
            case 'i':
                if ( (! optarg) || (! optarg[0]) )
                {
                    fprintf(stderr, "Please provide a valid path\n");
                    goto err;
                }
                if (optarg[0] == '/')    // absolute path
                   asprintf(&dependency_path, "%s/.", optarg);
                else                     // relative
                {
                   char current_dir[PATH_MAX];
                   getcwd(current_dir, PATH_MAX);
                   asprintf(&dependency_path, "%s/%s/.", current_dir, optarg);
                }
                free_dependency_path = true;
                break;
	    case 'n':
		verbose ++;
		dryrun = true;
		break;
	    case 's':
		showall = true;
		dryrun = true;
		break;
	    case 'p':
		if (optarg == (char*)0 || *optarg == '\0')
		    goto err;
		if (path != ipath) free(path);
		l = strlen(optarg) - 1;
		path = xstrdup(optarg);
		if (*(path+l) == '/')
		    *(path+l) = '\0';
		break;
	    case 'o':
		if (optarg == (char*)0 || *optarg == '\0')
		    goto err;
		override_path = optarg;
		set_override = true;
		break;
	    case 'u':
		if (optarg == (char*)0 || *optarg == '\0')
		    goto err;
		upstartjob_path = optarg;
		break;
	    case 'e':
		recursive = true;
		break;
	    case '?':
	    err:
		error("For help use: %s -h\n", myname);
	    case 'h':
		help(myname);
		exit(0);
	    default:
		break;
	}
    }
    argv += optind;
    argc -= optind;

    if (argc)
	loadarg = true;
    else if (del)
	error("usage: %s [[-r] init_script|init_directory]\n", myname);

    /* Make sure the target directory exists */
    /*
    if ( (! dryrun) && (! legacy_path) )
       mkdir(DEPENDENCY_PATH, 0755);
    */

    /* load options from /etc/inssserv/ directory */
    file_filters = Load_File_Filters();

    if (*argv) {
	char * token = strpbrk(*argv, delimeter);

	/*
	 * Let us separate the script/service name from the additional arguments.
	 */
	if (token && *token) {
	    *token = '\0';
	    *argr = ++token;
	}

	/* Catch `/path/script', `./script', and `path/script' */
	if (strchr(*argv, '/')) {
	    if (stat(*argv, &st_script) < 0)
		error("%s: %s\n", *argv, strerror(errno));
	} else {
	    pushd(path);
	    if (stat(*argv, &st_script) < 0)
		error("%s: %s\n", *argv, strerror(errno));
	    popd();
	}

	if (S_ISDIR(st_script.st_mode)) {
	    const size_t l = strlen(*argv) - 1;

	    if (path != ipath) free(path);
	    path = xstrdup(*argv);
	    if (*(path+l) == '/')
		*(path+l) = '\0';

	    argv++;
	    argc--;
	    if (argc || del)
		error("usage: %s [[-r] init_script|init_directory]\n", myname);

	} else {
	    char * base, * ptr = xstrdup(*argv);

	    if ((base = strrchr(ptr, '/'))) {
		if (path != ipath) free(path);
		*base = '\0';
		path  = ptr;
	    } else
		free(ptr);
	}
    }

    if (strcmp(path, INITDIR) != 0) {
	char * tmp;
	if (*path != '/') {
	    char * pwd = getcwd((char*)0, 0);
	    size_t len;
	    if (NULL == pwd)
	      error("unable to find current working directory: %s",
		    strerror(errno));
	    len = strlen(pwd)+2+strlen(path);
	    root = (char*)malloc(len);
	    if (!root)
		error("%s", strerror(errno));
	    strcpy(root, pwd);
	    if (pwd[1])
		strcat(root, "/");
	    strcat(root, path);
	    free(pwd);
	} else
	    root = xstrdup(path);
	if ((tmp = strstr(root, INITDIR))) {
	    *tmp = '\0';
	} else {
	    free(root);
	    root = (char*)0;
	}
    }

    c = argc;
    while (c--) {
	char * base;
	char * token = strpbrk(argv[c], delimeter);

	/*
	 * Let us separate the script/service name from the additional arguments.
	 */
	if (token && *token) {
	    *token = '\0';
	    argr[c] = ++token;
	}

	if (stat(argv[c], &st_script) < 0) {
	    if (errno != ENOENT)
		error("%s: %s\n", argv[c], strerror(errno));
	    pushd(path);
	    if (stat(argv[c], &st_script) < 0)
		error("%s: %s\n", argv[c], strerror(errno));
	    popd();
	}
	if ((base = strrchr(argv[c], '/'))) {
	    base++;
	    argv[c] = base;
	}
    }

#if defined(DEBUG) && (DEBUG > 0)
    for (c = 0; c < argc; c++)
	if (argr[c])
	    printf("Overwrite argument for %s is %s\n", argv[c], argr[c]);
#endif /* DEBUG */

#ifdef SUSE
    if (!underrpm())
#endif
    /*
     * Systemd support
     */
#ifdef WANT_SYSTEMD
    if (access(SYSTEMD_BINARY_PATH, F_OK) == 0 && (sbus = systemd_open_conn())) {

	for (c = 0; c < argc; c++)
	    forward_to_systemd (argv[c], del ? "disable": "enable", path != ipath);

	(void)systemd_get_tree(sbus);
	systemd_close_conn(sbus);
	systemd = true;
    }
#endif /* WANT_SYSTEMD */

    /*
     * Scan and set our configuration for virtual services.
     */
    scan_conf(insconf);

#ifdef WANT_SYSTEMD
    /*
     * Handle Systemd target as system facilities (<name>.target -> $<name>)
     */
    if (systemd)
	import_systemd_facilities();
#endif

    /*
     * Expand system facilities to real services
     */
    expand_conf();

#ifdef WANT_SYSTEMD
    /*
     * Handle Systemd services (<name>.service -> <name>)
     */
    if (systemd) {
	import_systemd_services();
	systemd_free();		/* Not used anymore */
    }
#endif

    /*
     * Initialize the regular scanner for the scripts.
     */
    scan_script_regalloc();

    /*
     * Scan always for the runlevel links to see the current
     * link scheme of the services.
     */
    scan_script_locations(path, override_path, ignore);

    /*
     * Clear out aliases found for scripts found up to this point.
     */
    clear_all();

    /*
     * Open the script directory
     */
    if ((initdir = opendir(path)) == (DIR*)0 || (dfd = dirfd(initdir)) < 0)
	error("can not opendir(%s): %s\n", path, strerror(errno));

#if defined _XOPEN_SOURCE && (_XOPEN_SOURCE - 0) >= 600
    (void)posix_fadvise(dfd, 0, 0, POSIX_FADV_WILLNEED);
    (void)posix_fadvise(dfd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    /*
     * Now scan for the service scripts and their LSB comments.
     */
    pushd(path);

    /*
     * Scan scripts found in the command line to be able to resolve
     * all dependcies given within those scripts.
     */
    for (c = 0; c < argc; c++) {
	const char *const name = argv[c];
	service_t * first = (service_t*)0;
	char * provides, * begin, * token;
	const uchar lsb = scan_script_defaults(dfd, name, override_path, (char**)0, false, ignore);

	if ((lsb & (FOUND_LSB_HEADER|FOUND_LSB_OPENRC)) == 0) {
	    if ((lsb & (FOUND_LSB_DEFAULT|FOUND_LSB_OVERRIDE)) == 0)
	        warn("warning: script '%s' missing LSB tags and overrides\n", name);
	    else
	        warn("warning: script '%s' missing LSB tags\n", name);
	}

	if (!script_inf.provides || script_inf.provides == empty)
	    script_inf.provides = xstrdup(name);

	provides = xstrdup(script_inf.provides);
	begin = provides;
	while ((token = strsep(&begin, delimeter)) && *token) {
	    service_t * service;

	    if (*token == '$') {
		warn("script %s provides system facility %s, skipped!\n", name, token);
		continue;
	    }
	    if (*token == '#') {
		warn("script %s provides facility %s with comment sign, skipped!\n", name, token);
		continue;
	    }

	    service = addservice(token);

	    if (first)
		nickservice(first, service);
	    else
		first = service;

	    service->attr.flags |= SERV_CMDLINE;
	}
	free(provides);
    }

    /*
     * Scan now all scripts found in the init.d/ directory
     */
    for (;;) {
	service_t * service = (service_t*)0;
	char * token;
	char * begin = (char*)0;	/* hold start pointer of strings handled by strsep() */
	boolean hard = false;
	boolean isarg = false;
	uchar lsb = 0;
#if defined(DEBUG) && (DEBUG > 0)
	int nobug = 0;
#endif

	if ((d = readdir(initdir)) == (struct dirent*)0) {
	    /*
	     * If first script in argument list was loaded in advance, then
	     * rewind the init.d/ directory stream and attempt to load all
	     * other scripts.
	     */
	    if (loadarg) {
		loadarg = false;
		rewinddir(initdir);
		continue;
	    }
	    break;
	}

	isarg = chkfor(d->d_name, argv, argc);

	/*
	 * Load first script in argument list before all other scripts. This
	 * avoids problems with loading scripts in underterministic sequence
	 * returned by readdir(3).
	 */
	if (loadarg && !isarg)
	    continue;
	if (loadarg  && isarg && (curr_argc != 0))
	    continue;
	if (!loadarg && isarg && (curr_argc == 0))
	    continue;

	if (*d->d_name == '.')
	    continue;
	errno = 0;

	/* d_type seems not to work, therefore use (l)stat(2) */
	if (xlstat(dfd, d->d_name, &st_script) < 0) {
	    warn("can not stat(%s)\n", d->d_name);
	    continue;
	}
	if ((!S_ISREG(st_script.st_mode) && !S_ISLNK(st_script.st_mode)) ||
	    !(S_IXUSR & st_script.st_mode))
	{
	    if (S_ISDIR(st_script.st_mode))
		continue;
	    if (isarg)
		warn("script %s is not an executable file, will be skipped in boot sequence!\n", d->d_name);
	    continue;
	}

	/*
	 * Do extra sanity checking of symlinks in init.d/ dir, except if it
	 * is named reboot, as that is a special case on SUSE
	 */
	if (S_ISLNK(st_script.st_mode) && ((strcmp(d->d_name, "reboot") != 0)))
	{
	    char * base;
	    char linkbuf[PATH_MAX+1];
	    int  linklen;

	    linklen = xreadlink(dfd, d->d_name, linkbuf, sizeof(linkbuf)-1);
	    if (linklen < 0)
		continue;
	    linkbuf[linklen] = '\0';

	    /* skip symbolic links to other scripts in this relative path */
	    if (!(base = strrchr(linkbuf, '/'))) {
		if (isarg)
		    warn("script %s is a symlink to another script, skipped!\n",
			 d->d_name);
		continue;
	    }

	    /* stat the symlink target and make sure it is a valid script */
	    if (xstat(dfd, d->d_name, &st_script) < 0)
		continue;

	    if (!S_ISREG(st_script.st_mode) || !(S_IXUSR & st_script.st_mode)) {
		if (S_ISDIR(st_script.st_mode))
		    continue;
		if (isarg)
		    warn("script %s is not an executable regular file, will be skipped in boot sequence!\n",
			 d->d_name);
		continue;
	    }
	}

	if (!strncmp(d->d_name, "README", strlen("README"))) {
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

	if (!strncmp(d->d_name, "Makefile", strlen("Makefile"))) {
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

	if (!strcmp(d->d_name, "core")) {
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

	/* Common scripts not used within runlevels */
	if (!strcmp(d->d_name, "rx")	   ||
	    !strncmp(d->d_name, "skeleton", 8) ||
	    !strncmp(d->d_name, "powerfail", 9))
	{
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

#ifdef SUSE
	if (!strcmp(d->d_name, "boot") || !strcmp(d->d_name, "rc"))
#else  /* not SUSE */
	if (!strcmp(d->d_name, "rcS") || !strcmp(d->d_name, "rc"))
#endif /* not SUSE */
	{
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

	if (cfgfile_filter(d) == 0) {
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

	/* left by emacs like editors */
	if (d->d_name[strlen(d->d_name)-1] == '~') {
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

	if (strspn(d->d_name, "$.#%_+-\\*[]^:()~")) {
	    if (isarg)
		warn("script name %s is not valid, skipped!\n", d->d_name);
	    continue;
	}

	/* main scanner for LSB comment in current script */
	lsb = scan_script_defaults(dfd, d->d_name, override_path, (char**)0, false, ignore);

	if ((lsb & (FOUND_LSB_HEADER|FOUND_LSB_OPENRC)) == 0) {
	    if ((lsb & (FOUND_LSB_DEFAULT|FOUND_LSB_OVERRIDE)) == 0)
	        warn("warning: script '%s' missing LSB tags and overrides\n", d->d_name);
	    else
	        warn("warning: script '%s' missing LSB tags\n", d->d_name);
	}

#ifdef SUSE
	/* Common script ... */
	if (!strcmp(d->d_name, "halt")) {
	    service_t *serv = addservice("halt");
	    serv = getorig(serv);
	    makeprov(serv,   d->d_name);
	    runlevels(serv, 'S', "0");
	    serv->attr.flags |= (SERV_ALL|SERV_NOSTOP|SERV_INTRACT);
	    continue;
	}

	/* ... and its link */
	if (!strcmp(d->d_name, "reboot")) {
	    service_t *serv = addservice("reboot");
	    serv = getorig(serv);
	    makeprov(serv,   d->d_name);
	    runlevels(serv, 'S', "6");
	    serv->attr.flags |= (SERV_ALL|SERV_NOSTOP|SERV_INTRACT);
	    continue;
	}

	/* Common script for single mode */
	if (!strcmp(d->d_name, "single")) {
	    service_t *serv = addservice("single");
	    serv = getorig(serv);
	    makeprov(serv,   d->d_name);
	    runlevels(serv, 'S', "1 S");
	    serv->attr.flags |= (SERV_ALL|SERV_NOSTOP|SERV_INTRACT);
	    rememberreq(serv, REQ_SHLD, "kbd");
	    continue;
	}
#endif /* SUSE */

#ifndef SUSE
	if (!lsb || (lsb & FOUND_LSB_OPENRC)) {
	    script_inf.required_start = xstrdup(DEFAULT_DEPENDENCY);
	    script_inf.required_stop = xstrdup(DEFAULT_DEPENDENCY);
	    script_inf.default_start = xstrdup(DEFAULT_START_LVL);
	    script_inf.default_stop = xstrdup(DEFAULT_STOP_LVL);
	}
#endif /* not SUSE */

	/*
	 * Oops, no comment found, guess one
	 */
	if (!script_inf.provides || script_inf.provides == empty) {
	    service_t * guess;
	    script_inf.provides = xstrdup(d->d_name);

	    /*
	     * Use guessed service to find it within the the runlevels
	     * (by using the list from the first scan for script locations).
	     */
	    if ((guess = findservice(script_inf.provides))) {
		/*
		 * Try to guess required services out from current scheme.
		 * Note, this means that all services are required.
		 */
		if (!script_inf.required_start || script_inf.required_start == empty) {
		    list_t * ptr;
		    list_for_each_prev(ptr, s_start) {
			service_t * tmp = getservice(ptr);
			tmp = getorig(tmp);
			if (!tmp->attr.sorder)
			    continue;
			if (tmp->attr.sorder >= guess->attr.sorder)
			    continue;
			if (tmp->start->lvl & guess->start->lvl) {
			    script_inf.required_start = xstrdup(tmp->name);
			    break;
			}
		    }
		}
		if (!script_inf.required_stop || script_inf.required_stop == empty) {
		    list_t * ptr;
		    list_for_each_prev(ptr, s_start) {
			service_t * tmp = getservice(ptr);
			tmp = getorig(tmp);
			if (!tmp->attr.korder)
			    continue;
			if (tmp->attr.korder <= guess->attr.korder)
			    continue;
			if (tmp->stopp->lvl & guess->stopp->lvl) {
			    script_inf.required_stop = xstrdup(tmp->name);
			    break;
			}
		    }
		}
		if (!script_inf.default_start || script_inf.default_start == empty) {
		    if (guess->start->lvl)
			script_inf.default_start = lvl2str(guess->start->lvl);
		}
		if (!script_inf.default_stop || script_inf.default_stop == empty) {
		    if (guess->stopp->lvl)
			script_inf.default_stop = lvl2str(guess->stopp->lvl);
		}

	    } else {	/* !findservice(&guess, script_inf.provides) */

		list_t * ptr;
		/*
		 * Find out which levels this service may have out from current scheme.
		 * Note, this means that the first requiring service wins.
		 */
		list_for_each(ptr, s_start) {
		    service_t * cur;
		    list_t * req;

		    if (script_inf.default_start && script_inf.default_start != empty)
			   break;
		    cur = getservice(ptr);
		    cur = getorig(cur);

		    if (list_empty(&cur->sort.req) || !(cur->attr.flags & SERV_ENABLED))
			continue;

		    np_list_for_each(req, &cur->sort.req) {
			if (!strcmp(getreq(req)->serv->name, script_inf.provides)) {
			    script_inf.default_start = lvl2str(getservice(ptr)->start->lvl);
			    break;
			}
		    }
		}
		list_for_each(ptr, s_start) {
		    service_t * cur;
		    list_t * rev;

		    if (script_inf.default_stop && script_inf.default_stop != empty)
			   break;
		    cur = getservice(ptr);
		    cur = getorig(cur);

		    if (list_empty(&cur->sort.rev) || !(cur->attr.flags & SERV_ENABLED))
			continue;

		    np_list_for_each(rev, &cur->sort.rev) {
			if (!strcmp(getreq(rev)->serv->name, script_inf.provides)) {
			    script_inf.default_stop = lvl2str(getservice(ptr)->stopp->lvl);
			    break;
			}
		    }
		}
	    }		/* !findservice(&guess, script_inf.provides) */
	}

	/*
	 * Use guessed service to find it within the the runlevels
	 * (by using the list from the first scan for script locations).
	 */
	if (!service) {
	    char * provides = xstrdup(script_inf.provides);
	    service_t * first = (service_t*)0;

	    begin = provides;
	    while ((token = strsep(&begin, delimeter)) && *token) {

		if (*token == '$') {
		    warn("script %s provides system facility %s, skipped!\n", d->d_name, token);
		    continue;
		}
		if (*token == '#') {
		    warn("script %s provides facility %s with comment sign, skipped!\n", d->d_name, token);
		    continue;
		}

		service = addservice(token);
 
		if (first)
		    nickservice(first, service);
		else
		    first = service;

#if defined(DEBUG) && (DEBUG > 0)
		nobug++;
#endif
		if (!makeprov(service, d->d_name)) {

		    if (!del || (del && !isarg))
			warn("script %s: service %s already provided!\n", d->d_name, token);

		    if (!del && !ignore && isarg) {
			waserr = true;
			continue;
		    }

		    if (!del || (del && !ignore && !isarg))
			continue;

		    /* Provide this service with an other name to be able to delete it */
		    service = addservice(d->d_name);
		    service = getorig(service);
		    service->attr.flags |= SERV_ALREADY;
		    (void)makeprov(service, d->d_name);

		    continue;
	    	}

		if (service) {
		    boolean known = (service->attr.flags & SERV_KNOWN);
		    service->attr.flags |= SERV_KNOWN;

		    if (!known) {
			if (script_inf.required_start && script_inf.required_start != empty) {
			    rememberreq(service, REQ_MUST, script_inf.required_start);
#ifdef USE_COMPAT_EMPTY
			    if (!script_inf.required_stop || script_inf.required_stop == empty)
				script_inf.required_stop = xstrdup(script_inf.required_start);
#endif /* USE_COMPAT_EMPTY */
			}
			if (script_inf.should_start && script_inf.should_start != empty) {
			    rememberreq(service, REQ_SHLD, script_inf.should_start);
#ifdef USE_COMPAT_EMPTY
			    if (!script_inf.should_stop || script_inf.should_stop == empty)
				script_inf.should_stop = xstrdup(script_inf.should_start);
#endif /* USE_COMPAT_EMPTY */
			}
			if (script_inf.required_stop && script_inf.required_stop != empty) {
			    rememberreq(service, REQ_MUST|REQ_KILL, script_inf.required_stop);
			}
			if (script_inf.should_stop && script_inf.should_stop != empty) {
			    rememberreq(service, REQ_SHLD|REQ_KILL, script_inf.should_stop);
			}
			if (script_inf.interactive && 0 == strcmp(script_inf.interactive, "true")) {
			    service->attr.flags |= SERV_INTRACT;
			}
		    }

		    if (script_inf.start_before && script_inf.start_before != empty) {
			reversereq(service, REQ_SHLD, script_inf.start_before);
#ifdef USE_COMPAT_EMPTY
			if (!script_inf.stop_after || script_inf.stop_after == empty)
			    script_inf.stop_after = xstrdup(script_inf.start_before);
#endif /* USE_COMPAT_EMPTY */
		    }
		    if (script_inf.stop_after && script_inf.stop_after != empty) {
			reversereq(service, REQ_SHLD|REQ_KILL, script_inf.stop_after);
		    }

                    overlap = Start_Stop_Overlap(script_inf.default_start, script_inf.default_stop);
                    if (overlap)
                    {
                        warn("Script `%s' has overlapping Default-Start and Default-Stop runlevels (%s) and (%s). This should be fixed.\n",
                              d->d_name, script_inf.default_start, script_inf.default_stop);
                    }

		    /*
		     * Use information from symbolic link structure to
		     * check if all services are around for this script.
		     */
		    if (isarg && !ignore) {
			boolean ok = true;
			if (del)
			    ok = chkdependencies(service);
			else
			    ok = chkrequired(service, recursive);
			if (!ok && !ignore)
			    waserr = true;
		    }

		    if (script_inf.default_start && script_inf.default_start != empty) {
		 	ushort deflvls = str2lvl(script_inf.default_start);

			if (service->attr.flags & SERV_ENABLED) {
			    /*
			     * Currently linked into service runlevel scheme, check
			     * if the defaults are overwriten. Compare all bits,
			     * which means `==' and not `&' and overwrite the defaults
			     * of the current script.
			     */
			    if (!defaults && (deflvls != service->start->lvl)) {
				if (!del && isarg && !(argr[curr_argc]))
                                {
				    warn("warning: current start runlevel(s) (%s) of script `%s' overrides LSB defaults (%s).\n",
                                           service->start->lvl ? lvl2str(service->start->lvl) :
                                           "empty", d->d_name, lvl2str(deflvls));
                                }
			    }
			} else
			    /*
			     * Currently not linked into service runlevel scheme, info
			     * needed for enabling interactive services at first time.
			     */
			    service->start->lvl = deflvls;

		    } else if (script_inf.default_start == empty) {
			if (service->attr.flags & SERV_ENABLED) {
			    /*
			     * Currently linked into service runlevel scheme, check
			     * if the defaults are overwriten. Compare all bits,
			     * which means `==' and not `&' and overwrite the defaults
			     * of the current script.
			     */
			    if (!defaults && service->start->lvl != 0) {
				if (!del && isarg && !(argr[curr_argc]))
				    warn("warning: current start runlevel(s) (%s) of script `%s' overrides LSB defaults (empty).\n",
					 lvl2str(service->start->lvl), d->d_name);
				script_inf.default_start = lvl2str(service->start->lvl);
			    }
			}
		    } else if (!script_inf.default_start && (service->attr.flags & SERV_NOTLSB)) {
#ifdef SUSE
			/*
			 * Could be a none LSB script, use info from current link scheme.
			 * If not found use default.
			 */
			if (service->attr.flags & SERV_ENABLED)
			    script_inf.default_start = lvl2str(service->start->lvl);
			else
			    script_inf.default_start = xstrdup(DEFAULT_START_LVL);
#endif /* SUSE */
		    }
#ifdef SUSE
		    /*
		     * This because SuSE boot script concept uses a differential link scheme.
		     * Therefore default_stop is ignored and overwriten by default_start.
		     */
		    xreset(script_inf.default_stop);
		    if (script_inf.default_start && script_inf.default_start != empty)
			script_inf.default_stop = xstrdup(script_inf.default_start);
		    else
			script_inf.default_stop = empty;
		    oneway(script_inf.default_stop);
#endif /* SUSE */
		    if (script_inf.default_stop && script_inf.default_stop != empty) {
		 	ushort deflvlk = str2lvl(script_inf.default_stop);

			/*
			 * Compare all bits, which means `==' and not `&' and overwrite
			 * the defaults of the current script.
			 */
			if (service->attr.flags & SERV_ENABLED) {
			    /*
			     * Currently linked into service runlevel scheme, check
			     * if the defaults are overwriten.
			     */
			    if (!defaults && (deflvlk != service->stopp->lvl)) {
				if (!del && isarg && !(argr[curr_argc]))
				    warn("warning: current stop runlevel(s) (%s) of script `%s' overrides LSB defaults (%s).\n",
					 service->stopp->lvl ? lvl2str(service->stopp->lvl) : "empty", d->d_name, lvl2str(deflvlk));
			    }
			} else
			    /*
			     * Currently not linked into service runlevel scheme, info
			     * needed for enabling interactive services at first time.
			     */
			    service->stopp->lvl = deflvlk;

		    } else if (script_inf.default_stop == empty) {
			if (service->attr.flags & SERV_ENABLED) {
			    /*
			     * Currently linked into service runlevel scheme, check
			     * if the defaults are overwriten. Compare all bits,
			     * which means `==' and not `&' and overwrite the defaults
			     * of the current script.
			     */
			    if (!defaults && service->stopp->lvl != 0) {
				if (!del && isarg && !(argr[curr_argc]))
				    warn("warning: current stop runlevel(s) (%s) of script `%s' overrides LSB defaults (empty).\n",
					 lvl2str(service->stopp->lvl), d->d_name);
				script_inf.default_stop = lvl2str(service->stopp->lvl);
			    }
			}
		    } else if (!script_inf.default_stop && (service->attr.flags & SERV_NOTLSB)) {
#ifdef SUSE
			/*
			 * Could be a none LSB script, use info from current link scheme.
			 * If not found use default.
			 */
			if (service->attr.flags & SERV_ENABLED)
			    script_inf.default_stop = lvl2str(service->stopp->lvl);
			else
			    script_inf.default_stop = xstrdup(DEFAULT_STOP_LVL);
#endif /* SUSE */
		    }
		}
	    }
	    free(provides);
	}

#ifdef SUSE
	/* Ahh ... set default multiuser with network */
	if (!script_inf.default_start || script_inf.default_start == empty) {
	    if (!script_inf.default_start)
		warn("Default-Start undefined, assuming default start runlevel(s) for script `%s'\n", d->d_name);
	    script_inf.default_start = xstrdup(DEFAULT_START_LVL);
	    xreset(script_inf.default_stop);
	    script_inf.default_stop = xstrdup(script_inf.default_start);
	    oneway(script_inf.default_stop);
	}
#else  /* not SUSE */
	if (!script_inf.default_start) {
	    warn("Default-Start undefined, assuming empty start runlevel(s) for script `%s'\n", d->d_name);
	    script_inf.default_start = empty;
	}
#endif /* not SUSE */

#ifdef SUSE
	if (!script_inf.default_stop || script_inf.default_stop == empty) {
	    if (script_inf.default_start && script_inf.default_start != empty)
		script_inf.default_stop = xstrdup(script_inf.default_start);
	    else
		script_inf.default_stop = xstrdup(DEFAULT_STOP_LVL);
	    oneway(script_inf.default_stop);
	}
#else  /* not SUSE */
	if (!script_inf.default_stop) {
	    warn("Default-Stop  undefined, assuming empty stop runlevel(s) for script `%s'\n", d->d_name);
	    script_inf.default_stop = empty;
	}
#endif /* not SUSE */

	if (isarg && !defaults && !del) {
	    if (argr[curr_argc]) {
		char * ptr = argr[curr_argc];
		struct _mark {
		    const char * wrd;
		    const boolean sk;
		    char * order;
		    char ** str;
		} mark[] = {
		    {"start=",	  true,  (char*)0, &script_inf.default_start},
		    {"stop=",	  false, (char*)0, &script_inf.default_stop },
#if 0
		    {"reqstart=", false, (char*)0, &script_inf.required_start},
		    {"reqstop=",  false, (char*)0, &script_inf.required_stop },
#endif
		    {(char*)0,	  false, (char*)0, (char**)0}
		};

		for (c = 0; mark[c].wrd; c++) {
		    char * order = strstr(ptr, mark[c].wrd);
		    if (order)
			mark[c].order = order;
		}

		for (c = 0; mark[c].wrd; c++)
		    if (mark[c].order) {
			*(mark[c].order) = '\0';
			mark[c].order += strlen(mark[c].wrd);
		    }

		for (c = 0; mark[c].wrd; c++)
		    if (mark[c].order) {
			size_t len = strlen(mark[c].order);
			if (len > 0) {
			    char * ptr = mark[c].order + len - 1;
			    if (*ptr == ',') *ptr = '\0';
			}
			if (ignore) {
			    service_t *arg = findservice(getprovides(d->d_name));
			    arg = getorig(arg);
			    if (mark[c].sk)
				arg->start->lvl = 0;
			    else
				arg->stopp->lvl = 0;
			}
			xreset(*(mark[c].str));
			*(mark[c].str) = xstrdup(mark[c].order);
		    }
		hard = true;
#ifdef SUSE
		/*
		 * This because SuSE boot script concept uses a differential link scheme.
		 * Therefore default_stop is ignored and overwriten by default_start.
		 */
		if (strcmp(script_inf.default_stop, script_inf.default_start) != 0) {
		    xreset(script_inf.default_stop);
		    script_inf.default_stop = xstrdup(script_inf.default_start);
		    oneway(script_inf.default_stop);
		}
#endif /* SUSE */
	    }
	}

#if defined(DEBUG) && (DEBUG > 0)
	if (!nobug) {
	    fprintf(stderr, "internal BUG at line %d with script %s\n", __LINE__, d->d_name);
	    exit(1);
	}
#endif

	begin = script_inf.provides;
	while ((token = strsep(&script_inf.provides, delimeter)) && *token) {
	    if (*token == '$')
		continue;
	    if (*token == '#')
		continue;
	    if (!service)
		service = addservice(token);
	    service = getorig(service);

	    if ((service->attr.flags & SERV_ENABLED) && !hard) {
		if (del)
		    continue;
		if (!defaults)
		    continue;
	    }

	    if (script_inf.default_start && script_inf.default_start != empty)
		runlevels(service, 'S', script_inf.default_start);
	    if (script_inf.default_stop && script_inf.default_stop != empty)
		runlevels(service, 'K', script_inf.default_stop);
	}
	script_inf.provides = begin;

	/* Remember if not LSB conform script */
	if (!lsb && service) {
	    service = getorig(service);
	    service->attr.flags |= SERV_NOTLSB;
	}
    }
    /* Reset remaining pointers */
    scan_script_reset();

    /*
     * Free the regular scanner for the scripts.
     */
    scan_script_regfree();

    /* back */
    popd();
    closedir(initdir);

    /*
     * Clear out aliases found for all scripts.
     */
    clear_all();

    /*
     * Set virtual dependencies for already enabled none LSB scripts.
     */
    nonlsb_script();

    /*
     * Handle the `$all' scripts
     */
    all_script();

    /*
     * Now generate for all scripts the dependencies
     */
    follow_all();
    if (is_loop_detected() && !ignore)
	error("exiting now without changing boot order!\n");

    /*
     * Be sure that interactive scripts are the only member of
     * a start group (for parallel start only).
     */
    active_script();

    /*
     * Check if runlevels of required scripts are a real subset
     * of the services handled here.
     */
    if (!del && !ignore) {
	list_t * ptr;
	list_for_each(ptr, s_start) {
	    service_t * cur = getservice(ptr);
	    ushort clvl = cur->start->lvl & ~LVL_SINGLE;
	    list_t * pos;

	    cur = getorig(cur);
	    if (list_empty(&cur->sort.req))
		continue;

	    if (cur->attr.flags & SERV_SYSTEMD)
		continue;

	    np_list_for_each(pos, &cur->sort.req) {
		req_t *req = getreq(pos);
		service_t * must;

		if ((req->flags & REQ_MUST) == 0)
		    continue;
		must = req->serv;
		must = getorig(must);

		if (must->attr.flags & SERV_SYSTEMD)
		    continue;

		/*
		 * Check for recursive mode the existence of the required services
		 */
		if (cur->attr.flags & SERV_CMDLINE) {

		    if (must->attr.flags & SERV_ENABLED) {
			ushort mlvl = must->start->lvl & ~LVL_SINGLE;

			if ((mlvl & LVL_BOOT) && (clvl & LVL_BOOT) == 0)
			    continue;

			if ((mlvl & clvl) == clvl)
			    continue;
			if (recursive) {
			    must->start->lvl |= clvl;
			    must->stopp->lvl |= clvl;
			    continue;
			}
			clvl &= ~mlvl;
			if ((must->attr.flags & SERV_WARNED) == 0)
#ifdef OSCBUILD
			    warn("Service %s is missed in the runlevels %s to use service %s\n",
				must->name, lvl2str(clvl), cur->name);
#else
			    warn("FATAL: service %s is missed in the runlevels %s to use service %s\n",
				must->name, lvl2str(clvl), cur->name);
			waserr = true;
#endif
			must->attr.flags |= SERV_WARNED;
			continue;
		    }
		    if ((must->attr.flags & (SERV_ENFORCE|SERV_KNOWN)) == SERV_ENFORCE) {
			if ((must->attr.flags & SERV_WARNED) == 0)
#ifdef OSCBUILD
			    warn("Service %s has to exist for service %s\n",
				must->name, cur->name);
#else
			    warn("FATAL: service %s has to exist for service %s\n",
				must->name, cur->name);
			waserr = true;
#endif
			must->attr.flags |= SERV_WARNED;
			continue;
		    }
		    if (recursive) {
			must->start->lvl |= clvl;
			must->stopp->lvl |= clvl;
			continue;
		    }
		    continue;
		}
		if ((cur->attr.flags & SERV_ENABLED) == 0)
		    continue;
		if ((must->attr.flags & (SERV_CMDLINE|SERV_KNOWN)) == 0) {
		    if ((must->attr.flags & SERV_WARNED) == 0)
#ifdef OSCBUILD
			warn("Service %s has to exist for service %s\n",
			    must->name, cur->name);
#else
			warn("FATAL: service %s has to exist for service %s\n",
			    must->name, cur->name);
		    waserr = true;
#endif
		    must->attr.flags |= SERV_WARNED;
		    continue;
		}
		if (must->attr.flags & SERV_ENABLED) {
		    ushort mlvl = must->start->lvl & ~LVL_SINGLE;

		    if ((mlvl & LVL_BOOT) && (clvl & LVL_BOOT) == 0)
			continue;

		    if ((mlvl & clvl) == clvl)
			continue;
		    clvl &= ~mlvl;
		    if ((must->attr.flags & SERV_WARNED) == 0)
#ifdef OSCBUILD
			warn("Service %s is missed in the runlevels %s to use service %s\n",
			    must->name, lvl2str(clvl), cur->name);
#else
			warn("FATAL: service %s is missed in the runlevels %s to use service %s\n",
			    must->name, lvl2str(clvl), cur->name);
		    waserr = true;
#endif
		    must->attr.flags |= SERV_WARNED;
		}
	    }
	}
    }

    if (waserr)
	error("exiting now!\n");

    /*
     * Sorry but we support only [KS][0-9][0-9]<name>
     */
    if (maxstart > MAX_DEEP || maxstop > MAX_DEEP)
	error("Maximum of %u in ordering reached\n", MAX_DEEP);

    if (showall)
	show_all();

#if defined(DEBUG) && (DEBUG > 0)
    printf("Maxorder %d/%d\n", maxstart, maxstop);
    show_all();
#else
# ifdef SUSE	/* SuSE's SystemV link scheme */
    pushd(path);
    for (runlevel = 0; runlevel < RUNLEVELS; runlevel++) {
	const ushort lvl = map_runlevel_to_lvl(runlevel);
	char nlink[PATH_MAX+1], olink[PATH_MAX+1];
	const char * rcd = (char*)0;
	const char * script;
	service_t *serv;
	DIR  * rcdir;

	if ((rcd = map_runlevel_to_location(runlevel)) == (char*)0)
	    continue;

	rcdir = openrcdir(rcd);		/* Creates runlevel directory if necessary */
	if (rcdir == (DIR*)0)
	    break;
	if ((dfd = dirfd(rcdir)) < 0) {
	    closedir(rcdir);
	    break;
	}
	pushd(rcd);

	/*
	 * See if we found scripts which should not be
	 * included within this runlevel directory.
	 */
	while ((d = readdir(rcdir)) != (struct dirent*)0) {
	    const char * ptr = d->d_name;
	    char type;

	    if (*ptr != 'S' && *ptr != 'K')
		continue;
	    type = *ptr;
	    ptr++;

	    if (strspn(ptr, "0123456789") != 2)
		continue;
	    ptr += 2;

	    if (xstat(dfd, d->d_name, &st_script) < 0)
		xremove(dfd, d->d_name);	/* dangling sym link */

	    if (notincluded(ptr, type, runlevel)) {
		serv = findservice(getprovides(ptr));
		if (defaults) {
		    xremove(dfd, d->d_name);
		    if (serv && --serv->attr.ref <= 0)
			serv->attr.flags &= ~SERV_ENABLED;
		} else if (lvl & LVL_ONEWAY) {
		    xremove(dfd, d->d_name);
		    if (serv && --serv->attr.ref <= 0)
			serv->attr.flags &= ~SERV_ENABLED;
		} else if (del && ignore) {
		    if (serv && (serv->attr.flags & SERV_ALREADY)) {
			xremove(dfd, d->d_name);
			if (--serv->attr.ref <= 0)
			    serv->attr.flags &= ~SERV_ENABLED;
		    }
		}
	    }
	}

	/*
	 * Seek for scripts which are included, link or
	 * correct order number if necessary.
	 */

	script = (char*)0;
	while ((serv = listscripts(&script, 'X', lvl))) {
	    boolean this = chkfor(script, argv, argc);
	    boolean found, slink;
	    char * clink;

	    if (*script == '$')		/* Do not link in virtual dependencies */
		continue;

	    if ((serv->attr.flags & (SERV_ENFORCE|SERV_ENABLED)) == SERV_ENFORCE)
		this = true;

	    slink = false;
	    if ((serv->start->lvl & lvl) == 0)
		goto stop;

	    sprintf(olink, "../%s",   script);
	    sprintf(nlink, "S%.2d%s", serv->attr.sorder, script);

	    found = false;
	    rewinddir(rcdir);
	    while ((clink = scan_for(rcdir, script, 'S'))) {
		found = true;
		if (strcmp(clink, nlink)) {
		    xremove(dfd, clink);		/* Wrong order, remove link */
		    if (--serv->attr.ref <= 0)
			serv->attr.flags &= ~SERV_ENABLED;
		    if (!this) {
			xsymlink(dfd, olink, nlink);	/* Not ours, but correct order */
			if (++serv->attr.ref)
			    serv->attr.flags |= SERV_ENABLED;
		    }
		    if (this && !del) {
			xsymlink(dfd, olink, nlink);	/* Restore, with correct order */
			if (++serv->attr.ref)
			    serv->attr.flags |= SERV_ENABLED;
		    }
		} else {
		    if (del && this) {
			xremove(dfd, clink);		/* Found it, remove link */
			if (--serv->attr.ref <= 0)
			    serv->attr.flags &= ~SERV_ENABLED;
		    }
		}
	    }

	    if (this) {
		/*
		 * If we haven't found it and we shouldn't delete it
		 * we try to add it.
		 */
		if (!del && !found) {
		    xsymlink(dfd, olink, nlink);
		    if (++serv->attr.ref)
			serv->attr.flags |= SERV_ENABLED;
		    found = true;
		}
	    }

	    /* Start links done, now do Kill links */

	    slink = found;
	stop:
	    if ((serv->stopp->lvl & lvl) == 0)
		continue;

	    sprintf(olink, "../%s",   script);
	    sprintf(nlink, "K%.2d%s", serv->attr.korder, script);

	    found = false;
	    rewinddir(rcdir);
	    while ((clink = scan_for(rcdir, script, 'K'))) {
		found = true;
		if (strcmp(clink, nlink)) {
		    xremove(dfd, clink);		/* Wrong order, remove link */
		    if (--serv->attr.ref <= 0)
			serv->attr.flags &= ~SERV_ENABLED;
		    if (!this) {
			xsymlink(dfd, olink, nlink);	/* Not ours, but correct order */
			if (++serv->attr.ref)
			    serv->attr.flags |= SERV_ENABLED;
		    }
		    if (this && !del) {
			xsymlink(dfd, olink, nlink);	/* Restore, with correct order */
			if (++serv->attr.ref)
			    serv->attr.flags |= SERV_ENABLED;
		    }
		} else {
		    if (del && this) {
			xremove(dfd, clink);		/* Found it, remove link */
			if (--serv->attr.ref <= 0)
			    serv->attr.flags &= ~SERV_ENABLED;
		    }
		}
	    }

	    if (this && slink) {
		/*
		 * If we haven't found it and we shouldn't delete it
		 * we try to add it.
		 */
		if (!del && !found) {
		    xsymlink(dfd, olink, nlink);
		    if (++serv->attr.ref)
			serv->attr.flags |= SERV_ENABLED;
		}
	    }
	}
	popd();
	closedir(rcdir);
    }
# else  /* not SUSE but Debian SystemV link scheme */
   /*
    * Remark: At SuSE we use boot scripts for system initialization which
    * will be executed by /etc/init.d/boot (which is equal to rc.sysinit).
    * At system reboot or system halt the stop links of those boot scripts
    * will be executed by /etc/init.d/halt.  Don't know how todo this for
    * a traditional standard SystemV link scheme.  Maybe for such an
    * approach a new directory halt.d/ whould be an idea.
    */
    pushd(path);
    for (runlevel = 0; runlevel < RUNLEVELS; runlevel++) {
	char nlink[PATH_MAX+1], olink[PATH_MAX+1];
	const char * rcd = (char*)0;
	const char * script;
	service_t * serv;
	ushort lvl, seek;
	DIR  * rcdir;

	if ((rcd = map_runlevel_to_location(runlevel)) == (char*)0)
	    continue;
	lvl  = map_runlevel_to_lvl(runlevel);
	seek = map_runlevel_to_seek(runlevel);

	rcdir = openrcdir(rcd);		/* Creates runlevel directory if necessary */
	if (rcdir == (DIR*)0)
	    break;
	if ((dfd = dirfd(rcdir)) < 0) {
	    closedir(rcdir);
	    break;
	}
	pushd(rcd);

	/*
	 * See if we found scripts which should not be
	 * included within this runlevel directory.
	 */
	while ((d = readdir(rcdir)) != (struct dirent*)0) {
	    const char * ptr = d->d_name;
	    char type;

	    if (*ptr != 'S' && *ptr != 'K')
		continue;
	    type = *ptr;
	    ptr++;

	    if (strspn(ptr, "0123456789") != 2)
		continue;
	    ptr += 2;

	    if (xstat(dfd, d->d_name, &st_script) < 0)
		xremove(dfd, d->d_name);	/* dangling sym link */

	    if (notincluded(ptr, type, runlevel)) {
		serv = findservice(getprovides(ptr));
		if (defaults) {
		    xremove(dfd, d->d_name);
		    if (serv && --serv->attr.ref <= 0)
			serv->attr.flags &= ~SERV_ENABLED;
#  ifndef USE_KILL_IN_BOOT
		} else if (lvl & LVL_BOOT) {
		    xremove(dfd, d->d_name);
		    if (serv && --serv->attr.ref <= 0)
			serv->attr.flags &= ~SERV_ENABLED;
#  endif /* USE_KILL_IN_BOOT */
		} else if (del && ignore) {
		    if (serv && (serv->attr.flags & SERV_ALREADY)) {
			xremove(dfd, d->d_name);
			if (--serv->attr.ref <= 0)
			    serv->attr.flags &= ~SERV_ENABLED;
		    }
		}
	    }
	}

	script = (char*)0;
	while ((serv = listscripts(&script, 'X', seek))) {
	    boolean this = chkfor(script, argv, argc);
	    boolean found;
	    char * clink;
	    char mode;

	    if (*script == '$')		/* Do not link in virtual dependencies */
		continue;

	    if ((serv->attr.flags & (SERV_ENFORCE|SERV_ENABLED)) == SERV_ENFORCE) 
		this = true;

	    sprintf(olink, "../init.d/%s", script);
	    if (serv->stopp->lvl & lvl) {
#  ifndef USE_KILL_IN_BOOT
		if (lvl & LVL_BOOT)			/* No kill links in rcS.d */
			continue;
#  endif /* USE_KILL_IN_BOOT */
		sprintf(nlink, "K%.2d%s", serv->attr.korder, script);
		mode = 'K';
	    } else if (serv->start->lvl & lvl) {
		sprintf(nlink, "S%.2d%s", serv->attr.sorder, script);
		mode = 'S';
	    } else
		continue;		/* We aren't suppose to be on this runlevel */

	    found = false;

	    rewinddir(rcdir);
	    while ((clink = scan_for(rcdir, script, mode))) {
		found = true;
		if (strcmp(clink, nlink)) {
		    xremove(dfd, clink);		/* Wrong order, remove link */
		    if (--serv->attr.ref <= 0)
			serv->attr.flags &= ~SERV_ENABLED;
		    if (!this) {
			xsymlink(dfd, olink, nlink);	/* Not ours, but correct order */
			if (++serv->attr.ref)
			    serv->attr.flags |= SERV_ENABLED;
		    }
		    if (this && !del) {
			xsymlink(dfd, olink, nlink);	/* Restore, with correct order */
			if (++serv->attr.ref)
			    serv->attr.flags |= SERV_ENABLED;
		    }
		} else {
		    if (del && this) {
			xremove(dfd, clink);		/* Found it, remove link */
			if (--serv->attr.ref <= 0)
			    serv->attr.flags &= ~SERV_ENABLED;
		    }
		}
	    }

	    if (this) {
		/*
		 * If we haven't found it and we shouldn't delete it
		 * we try to add it.
		 */
		if (!del && !found) {
		    xsymlink(dfd, olink, nlink);
		    if (++serv->attr.ref)
			serv->attr.flags |= SERV_ENABLED;
		    found = true;
		}
	    }
	}

	popd();
	closedir(rcdir);
    }
# endif /* !SUSE, standard SystemV link scheme */
#endif  /* !DEBUG */

    /*
     * Do the makedep
     */
    makedep();

    /*
     * Back to the root(s)
     */
    popd();

    /*
     * Make valgrind happy
     */
    if (path != ipath) free(path);
    if (root) free(root);
    if ( (free_dependency_path) && (dependency_path) )
        free(dependency_path);
    if (file_filters)
    {
       int count = 0;
       while (file_filters[count])
       {
           free(file_filters[count]);
           count++;
       }
       free(file_filters);
    }
    return 0;
}
