/* Copyright 1988,1990,1993,1994 by Paul Vixie
 * All rights reserved
 *
 * Distribute freely, except: don't remove my name from the source or
 * documentation (don't take credit for my work), mark your changes (don't
 * get me blamed for your possible bugs), don't alter or remove this
 * notice.  May be sold if buildable source is provided to buyer.  No
 * warrantee of any kind, express or implied, is included with this
 * software; use at your own risk, responsibility for damages (if any) to
 * anyone resulting from the use of this software rests entirely with the
 * user.
 *
 * Send bug reports, bug fixes, enhancements, requests, flames, etc., and
 * I'll try to keep a version up to date.  I can be reached as follows:
 * Paul Vixie          <paul@vix.com>          uunet!decwrl!vixie!paul
 */

#if !defined(lint) && !defined(LINT)
static const char rcsid[] =
  "$FreeBSD: stable/9/usr.sbin/cron/cron/database.c 173412 2007-11-07 10:53:41Z kevlo $";
#endif

/* vix 26jan87 [RCS has the log]
 */


#include "cron.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <libgen.h>


#define TMAX(a,b) ((a)>(b)?(a):(b))

static	char		*includemask = NULL;  

static	void		process_crontab(char *, char *, char *,
					     struct stat *,
					     cron_db *, cron_db *),
			get_included_files(cron_db *, cron_db *, struct stat *);
static	int		find_in_dir(const struct dirent *d);


void
load_database(old_db)
	cron_db		*old_db;
{
	DIR		*dir;
	struct stat	statbuf;
	struct stat	syscron_stat;
	struct stat	include_stat;
	//struct stat	suminclude_stat;
	DIR_T   	*dp;
	cron_db		new_db;
	user		*u, *nu, *inc;
	//cron_inc	*cinc;
	//cron_inc	findinc;

	include_stat.st_mtime = 0;

	Debug(DLOAD, ("[%d] load_database()\n", getpid()))

	/* before we start loading any data, do a stat on SPOOL_DIR
	 * so that if anything changes as of this moment (i.e., before we've
	 * cached any of the database), we'll see the changes next time.
	 */
	if (stat(SPOOL_DIR, &statbuf) < OK) {
		log_it("CRON", getpid(), "STAT FAILED", SPOOL_DIR);
		(void) exit(ERROR_EXIT);
	}

	/* track system crontab file
	 */
	if (stat(SYSCRONTAB, &syscron_stat) < OK)
		syscron_stat.st_mtime = 0;

	/*
	 * track files which included from 
	 * system crontab
	 */ 
	if ((inc = find_user(old_db, SYS_INCLUDE)))
	{
		if (inc->include)
		{
		    if (stat(dirname(inc->include), &include_stat) < OK)
			log_it("CRON", getpid(), "stat include files failed", SYSCRONTAB);
		}
	}
/*	if ((inc = find_user(old_db, SYS_INCLUDE)))
	{
		if (inc->include)
		{
		    cinc=inc->include;    
		    while(cinc!=NULL)
		    {
			log_it("CRON", getpid(), "stat include: ", cinc->pname);
			if (stat(cinc->pname, &include_stat) < OK)
			{
			    log_it("CRON", getpid(), "INCLUDE STAT FAILED remove from list...", cinc->pname);
			    if (inc->include->prev)
				inc->include->prev->next = inc->include->next;
			    if (inc->include->next)
				inc->include->next->prev = inc->include->prev;
			    free(inc->include);
			    include_stat.st_mtime = 0;
			}
			suminclude_stat.st_mtime = TMAX(include_stat.st_mtime, suminclude_stat.st_mtime);
			cinc=cinc->next;
		    }
		}
	};*/
	syscron_stat.st_mtime = TMAX(include_stat.st_mtime, syscron_stat.st_mtime);	

	/* if spooldir's mtime has not changed, we don't need to fiddle with
	 * the database.
	 *
	 * Note that old_db->mtime is initialized to 0 in main(), and
	 * so is guaranteed to be different than the stat() mtime the first
	 * time this function is called.
	 */

	if (old_db->mtime == TMAX(statbuf.st_mtime, syscron_stat.st_mtime)) {
		Debug(DLOAD, ("[%d] spool dir mtime unch, no load needed.\n",
			      getpid()))
		return;
	}

	new_db.mtime = TMAX(statbuf.st_mtime, syscron_stat.st_mtime);
	new_db.head = new_db.tail = NULL;

	/* something's different.  make a new database, moving unchanged
	 * elements from the old database, reloading elements that have
	 * actually changed.  Whatever is left in the old database when
	 * we're done is chaff -- crontabs that disappeared.
	 */

	if (syscron_stat.st_mtime) {
		process_crontab("root", SYS_NAME,
				SYSCRONTAB, &syscron_stat,
				&new_db, old_db);
	}

	get_included_files(&new_db, old_db, &include_stat);

	/* we used to keep this dir open all the time, for the sake of
	 * efficiency.  however, we need to close it in every fork, and
	 * we fork a lot more often than the mtime of the dir changes.
	 */
	if (!(dir = opendir(SPOOL_DIR))) {
		log_it("CRON", getpid(), "OPENDIR FAILED", SPOOL_DIR);
		(void) exit(ERROR_EXIT);
	}

	while (NULL != (dp = readdir(dir))) {
		char	fname[MAXNAMLEN+1],
			tabname[MAXNAMLEN+1];

		/* avoid file names beginning with ".".  this is good
		 * because we would otherwise waste two guaranteed calls
		 * to getpwnam() for . and .., and also because user names
		 * starting with a period are just too nasty to consider.
		 */
		if (dp->d_name[0] == '.')
			continue;

		(void) strncpy(fname, dp->d_name, sizeof(fname));
		fname[sizeof(fname)-1] = '\0';
		(void) snprintf(tabname, sizeof tabname, CRON_TAB(fname));

		process_crontab(fname, fname, tabname,
				&statbuf, &new_db, old_db);
	}
	closedir(dir);

	/* if we don't do this, then when our children eventually call
	 * getpwnam() in do_command.c's child_process to verify MAILTO=,
	 * they will screw us up (and v-v).
	 */
	endpwent();

	/* whatever's left in the old database is now junk.
	 */
	Debug(DLOAD, ("unlinking old database:\n"))
	for (u = old_db->head;  u != NULL;  u = nu) {
		Debug(DLOAD, ("\t%s\n", u->name))
		nu = u->next;
		unlink_user(old_db, u);
		free_user(u);
	}

	/* overwrite the database control block with the new one.
	 */
	*old_db = new_db;
	Debug(DLOAD, ("load_database is done\n"))
}


void
link_user(db, u)
	cron_db	*db;
	user	*u;
{
	if (db->head == NULL)
		db->head = u;
	if (db->tail)
		db->tail->next = u;
	u->prev = db->tail;
	u->next = NULL;
	db->tail = u;
}


void
unlink_user(db, u)
	cron_db	*db;
	user	*u;
{
	if (u->prev == NULL)
		db->head = u->next;
	else
		u->prev->next = u->next;

	if (u->next == NULL)
		db->tail = u->prev;
	else
		u->next->prev = u->prev;
}


user *
find_user(db, name)
	cron_db	*db;
	char	*name;
{
	char	*env_get();
	user	*u;

	for (u = db->head;  u != NULL;  u = u->next)
		if (!strcmp(u->name, name))
			break;
	return u;
}


static void
process_crontab(uname, fname, tabname, statbuf, new_db, old_db)
	char		*uname;
	char		*fname;
	char		*tabname;
	struct stat	*statbuf;
	cron_db		*new_db;
	cron_db		*old_db;
{
	struct passwd	*pw = NULL;
	int		crontab_fd = OK - 1;
	user		*u;
	char		*include;

	if (strcmp(fname, SYS_NAME) && strcmp(fname, SYS_INCLUDE) && !(pw = getpwnam(uname))) {
		/* file doesn't have a user in passwd file.
		 */
		log_it(fname, getpid(), "ORPHAN", "no passwd entry");
		goto next_crontab;
	}

	if ((crontab_fd = open(tabname, O_RDONLY, 0)) < OK) {
		/* crontab not accessible?
		 */
		log_it(fname, getpid(), "CAN'T OPEN", tabname);
		goto next_crontab;
	}

	if (fstat(crontab_fd, statbuf) < OK) {
		log_it(fname, getpid(), "FSTAT FAILED", tabname);
		goto next_crontab;
	}

	Debug(DLOAD, ("\t%s:", fname))
	u = find_user(old_db, fname);
	if (u != NULL) {
		/* if crontab has not changed since we last read it
		 * in, then we can just use our existing entry.
		 */
		if (u->mtime == statbuf->st_mtime) {
			Debug(DLOAD, (" [no change, using old data]"))
			unlink_user(old_db, u);
			link_user(new_db, u);
			goto next_crontab;
		}

		/* before we fall through to the code that will reload
		 * the user, let's deallocate and unlink the user in
		 * the old database.  This is more a point of memory
		 * efficiency than anything else, since all leftover
		 * users will be deleted from the old database when
		 * we finish with the crontab...
		 */
		Debug(DLOAD, (" [delete old data]"))
		if (!strcmp(fname, SYS_INCLUDE))
		{
			free(u->include);
			u->include = NULL;
		}
		unlink_user(old_db, u);
		free_user(u);
		log_it(fname, getpid(), "RELOAD", tabname);
	}
	u = load_user(crontab_fd, pw, fname);
	if (!strcmp(fname, SYS_INCLUDE))
	{
		if (u->include && strcmp(u->include, includemask))
		{
		    free(u->include);	
		    include = malloc(strlen(includemask));
		    u->include = include;
		}
	}
	if (u != NULL) {
		u->mtime = statbuf->st_mtime;
		link_user(new_db, u);
	}

next_crontab:
	if (crontab_fd >= OK) {
		Debug(DLOAD, (" [done]\n"))
		close(crontab_fd);
	}
}

static void
get_included_files(new_db, old_db, statbuf)
	cron_db		*new_db;
	cron_db		*old_db;
	struct stat	*statbuf;
{
    char	*root, 
		*mask;
    int		crontab_fd,
		i,
		is_regular = 0,
    		n;
    FILE	*syscrontab;
    char	crontabline[MAX_COMMAND];
    char	*delim = " \t\n";
    char	*param = NULL;
    char	*checkparam;
    char	*value;
    char	fullpath[MAX_INCLUDENAME];
    struct stat	filestat;
    struct dirent **entry;

    /* track system included crontab file
    */
    Debug(DLOAD, ("Finding includes in %s\n", SYSCRONTAB));
    syscrontab = fopen(SYSCRONTAB, "r");
    while(fgets(crontabline, MAX_COMMAND, syscrontab))
    {
	    value = strtok(crontabline, delim);
	    if (strcmp(value,INCLUDECRON))
		    continue;
	    param = strtok(NULL, delim);
	    checkparam = strtok(NULL, delim);

	    if (checkparam != NULL)
	    {
		    log_it("CRON", getpid(), "Include directives not correct", SYSCRONTAB);
		    break;
	    }
	    Debug(DLOAD, ("Found #include: %s\n", param));
	    if (stat(SYSCRONTAB, statbuf) < OK)
		    statbuf->st_mtime = 0;
    }
    fclose(syscrontab);

    if (!param)
    {
	Debug(DLOAD, ("Include directive not find in crontab\n"));
	return;
    }

    mask = basename(param);

    for(i=0;mask[i];i++)
    {
	    if (mask[i]=='*')
	    {
		is_regular = 1;
		break;
	    }
    }
    if (!is_regular)
    {
	if ((crontab_fd = open(param, O_RDONLY, 0)) < OK) {
		/* crontab not accessible?
		 */
		log_it("get_included_files(): ", getpid(), "can't open include file", param);
	}
	else
	{
		if (stat(param, statbuf) < OK) {
		    log_it("CRON", getpid(), "STAT FAILED", param);
		    statbuf->st_mtime = 0;
		}
		new_db->mtime = TMAX(statbuf->st_mtime, new_db->mtime);
		Debug(DLOAD, ("Processing included crontab: %s\n", param));
		process_crontab("root", SYS_INCLUDE,
		param, statbuf,
		new_db, old_db);
	}
	return;
    }

    root = dirname(param);
    includemask = param;
    n=scandir(root, &entry, find_in_dir, alphasort);
    if (n<0)
    {
	log_it("get_included_files(): ", getpid(), "can't find include file", param);
	return;
    }
    Debug(DLOAD, ("Found: %d files for include\n", n));
    while(n--)
    {
	bzero(fullpath, MAX_INCLUDENAME);
	strcpy(fullpath, root);    
	strcat(fullpath, "/");    
	strcat(fullpath, entry[n]->d_name);    
	if (stat(fullpath, &filestat) < OK) {
	    log_it("CRON", getpid(), "Stat failed for", fullpath);
	    continue;
	}
	if (!(filestat.st_mode & S_IFREG))
	{
	    Debug(DLOAD, ("File for include is not regular %s. Return %d\n", fullpath, filestat.st_mode));
	    continue;	
	}
	statbuf->st_mtime = TMAX(statbuf->st_mtime, filestat.st_mtime);
	new_db->mtime = TMAX(statbuf->st_mtime, new_db->mtime);
	Debug(DLOAD, ("Processing included crontab: %s\n", fullpath));
	process_crontab("root", SYS_INCLUDE,
	    fullpath, statbuf,
	    new_db, old_db);
    }
    return;
}

static int
find_in_dir(const struct dirent *d)
{
	char	*delim = "*";
	char	*first,
		*second,
		*secondorig;
	int	origdlen;
	int	secondlen;
	char	mask[MAX_INCLUDENAME];

	bzero(mask, MAX_INCLUDENAME);
	strcpy(mask, basename(includemask));

	if ( !strcmp(".", d->d_name) || !strcmp("..", d->d_name)) return 0;
	first = strtok(mask, delim);
	second = strtok(NULL, delim);
	origdlen = strlen(d->d_name);
	if (second)
	{
	    secondlen = strlen(second);
	    secondorig = (char *)(d->d_name + (origdlen-secondlen));
	    if (!strncmp(first, d->d_name, strlen(first)) && !strncmp(second, secondorig, strlen(second)))
		return 1;
	}
	else
	    if (!strncmp(first, d->d_name+(origdlen-strlen(first)), strlen(first)))
		return 1;
	return 0;
};
