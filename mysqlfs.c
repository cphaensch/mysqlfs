/*
  mysqlfs - MySQL Filesystem
  Copyright (C) 2006 Tsukasa Hamano <code@cuspy.org>
  $Id$

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <fuse/fuse.h>
#ifdef HAVE_MYSQL_MYSQL_H
#include <mysql/mysql.h>
#endif
#ifdef HAVE_MYSQL_H
#include <mysql.h>
#endif
#include <pthread.h>
#include <sys/stat.h>

#ifdef DEBUG
#include <mcheck.h>
#endif
#include <stddef.h>

#include "mysqlfs.h"
#include "query.h"
#include "pool.h"
#include "log.h"

#ifdef STATUSDIR
/**
 * not sure where we can stash this, if it even has to be a global variable.  This variable is used
 * as-is to find pathnames (because it works in the root of the mysqlfs path) and status_pathname+1
 * for local filenames (a bogus method of basename(status_pathname))
 */
static char *status_pathname = "/"STATUSDIR;
static int len_status_pathname = 8;
#define inode_status_xml -3
#define inode_status_txt -4
static int snprint_status(char *, size_t, struct mysqlfs_opt *, long);
#endif

/**
 * copy to the options setting, global variable.  Used for both STATUSDIR (if configured) and osxnospotlight
 *
 * it's a bit of a kludge to set a pointer to a structure in the mainfunc, but it was breaking (*blush*),
 * need to clean this up later
 */
struct mysqlfs_opt *theopts;

static int mysqlfs_getattr(const char *path, struct stat *stbuf)
{
    int ret;
    MYSQL *dbconn;
#ifdef STATUSDIR
    char buf[8 * 1024];
#endif

    // This is called far too often
    log_printf(LOG_D_CALL, "mysqlfs_getattr(\"%s\")\n", path);

#ifdef STATUSDIR
    if (0 == strncmp (path, status_pathname, len_status_pathname))
    {
        char *a = (char *) path + len_status_pathname;

        log_printf(LOG_D_CALL, "%s(\"%s\")(@%d)\n", __FUNCTION__, path, __LINE__);
        stbuf->st_mode = S_IRUSR|S_IXUSR | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH;
        stbuf->st_nlink = 1;

        if (0 == *a)
            stbuf->st_mode |= S_IFDIR;
        else
        {
            stbuf->st_mode |= S_IFREG;
            if (0 == strcmp ("/txt", a))
                stbuf->st_size = snprint_status (buf, sizeof(buf), theopts, inode_status_txt);
            else if (0 == strcmp ("/xml", a))
                stbuf->st_size = snprint_status (buf, sizeof(buf), theopts, inode_status_xml);
            else
                stbuf->st_size = 0;
        }

        return 0;
    }
#endif

    /* fixme: move memset() above STATUSDIR ? */
    memset(stbuf, 0, sizeof(struct stat));

    if ( (0 < theopts->osxnospotlight) && (0 == strcmp (path, "/.metadata_never_index")))
    {
        stbuf->st_mode = S_IRUSR|S_IRGRP|S_IROTH|S_IFREG;	/* file w/ mode 0444 */
        stbuf->st_nlink = 1;
        stbuf->st_size = sizeof(PACKAGE_STRING);	/* I need some bogus content for a read() operation */

        return 0;
    }

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    ret = query_getattr(dbconn, path, stbuf);

    if(ret){
        if (ret != -ENOENT)
            log_printf(LOG_ERROR, "Error: query_getattr()\n");
        pool_put(dbconn);
        return ret;
    }else{
        long inode = query_inode(dbconn, path);
        if(inode < 0){
            log_printf(LOG_ERROR, "Error: query_inode()\n");
            pool_put(dbconn);
            return inode;
        }

        stbuf->st_size = query_size(dbconn, inode);
    }

    pool_put(dbconn);

    return ret;
}

static int mysqlfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;
    MYSQL *dbconn;
    long inode;

    log_printf(LOG_D_CALL, "mysqlfs_readdir(\"%s\")\n", path);

#ifdef STATUSDIR
    if (0 == strcmp (path, status_pathname))
    {
        /* if printing the bogus "status" directory, dump the content and get out */
        log_printf(LOG_D_CALL, "mysqlfs_readdir(\"%s\")(@%d)\n", path, __LINE__);
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);
        filler(buf, "txt", NULL, 0);
        filler(buf, "xml", NULL, 0);

        return 0;
    }
#endif

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, path);
    if(inode < 0){
        log_printf(LOG_ERROR, "Error: query_inode()\n");
        pool_put(dbconn);
        return inode;
    }

    
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

#ifdef STATUSDIR
    /* stuff in the bogus status subdir */
    if (0 == strcmp (path, "/"))
    {
        log_printf(LOG_D_CALL, "mysqlfs_readdir(\"%s\")(@%d)\n", path, __LINE__);
        filler(buf, status_pathname+1, NULL, 0);
    }
#endif


    query_readdir(dbconn, inode, buf, filler);
    pool_put(dbconn);

    return 0;
}

/** FUSE function for mknod(const char *pathname, mode_t mode, dev_t dev); API call.  @see http://linux.die.net/man/2/mknod */
static int mysqlfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    int ret;
    MYSQL *dbconn;
    long parent_inode;
    char dir_path[PATH_MAX];

    log_printf(LOG_D_CALL, "mysqlfs_mknod(\"%s\", %o): %s\n", path, mode,
	       S_ISREG(mode) ? "file" :
	       S_ISDIR(mode) ? "directory" :
	       S_ISLNK(mode) ? "symlink" :
	       "other");

    if(!(strlen(path) < PATH_MAX)){
        log_printf(LOG_ERROR, "Error: Filename too long\n");
        return -ENAMETOOLONG;
    }
    strncpy(dir_path, path, PATH_MAX);
    dirname(dir_path);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    parent_inode = query_inode(dbconn, dir_path);
    if(parent_inode < 0){
        pool_put(dbconn);
        return -ENOENT;
    }

    ret = query_mknod(dbconn, path, mode, rdev, parent_inode, S_ISREG(mode) || S_ISLNK(mode));
    if(ret < 0){
        pool_put(dbconn);
        return ret;
    }

    pool_put(dbconn);
    return 0;
}

static int mysqlfs_mkdir(const char *path, mode_t mode){
    int ret;
    MYSQL *dbconn;
    long inode;
    char dir_path[PATH_MAX];

    log_printf(LOG_D_CALL, "mysqlfs_mkdir(\"%s\", 0%o)\n", path, mode);
    
    if(!(strlen(path) < PATH_MAX)){
        log_printf(LOG_ERROR, "Error: Filename too long\n");
        return -ENAMETOOLONG;
    }
    strncpy(dir_path, path, PATH_MAX);
    dirname(dir_path);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, dir_path);
    if(inode < 0){
        pool_put(dbconn);
        return -ENOENT;
    }

    ret = query_mkdir(dbconn, path, mode, inode);
    if(ret < 0){
        log_printf(LOG_ERROR, "Error: query_mkdir()\n");
        pool_put(dbconn);
        return ret;
    }

    pool_put(dbconn);
    return 0;
}

static int mysqlfs_unlink(const char *path)
{
    int ret;
    long inode, parent, nlinks;
    char name[PATH_MAX];
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysqlfs_unlink(\"%s\")\n", path);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    ret = query_inode_full(dbconn, path, name, sizeof(name),
			   &inode, &parent, &nlinks);
    if (ret < 0) {
        if (ret != -ENOENT)
            log_printf(LOG_ERROR, "Error: query_inode_full(%s): %s\n",
		       path, strerror(ret));
	goto err_out;
    }

    ret = query_rmdirentry(dbconn, name, parent);
    if (ret < 0) {
        log_printf(LOG_ERROR, "Error: query_rmdirentry()\n");
	goto err_out;
    }

    /* Only the last unlink() must set deleted flag. 
     * This is a shortcut - query_set_deleted() wouldn't
     * set the flag if there is still an existing direntry
     * anyway. But we'll save some DB processing here. */
    if (nlinks > 1)
        return 0;
    
    ret = query_set_deleted(dbconn, inode);
    if (ret < 0) {
        log_printf(LOG_ERROR, "Error: query_set_deleted()\n");
	goto err_out;
    }

    ret = query_purge_deleted(dbconn, inode);
    if (ret < 0) {
        log_printf(LOG_ERROR, "Error: query_purge_deleted()\n");
	goto err_out;
    }

    pool_put(dbconn);

    return 0;

err_out:
    pool_put(dbconn);
    return ret;
}

static int mysqlfs_chmod(const char* path, mode_t mode)
{
    int ret;
    long inode;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysql_chmod(\"%s\", 0%3o)\n", path, mode);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, path);
    if (inode < 0) {
        pool_put(dbconn);
        return inode;
    }

    ret = query_chmod(dbconn, inode, mode);
    if(ret){
        log_printf(LOG_ERROR, "Error: query_chmod()\n");
        pool_put(dbconn);
        return -EIO;
    }

    pool_put(dbconn);

    return ret;
}

static int mysqlfs_chown(const char *path, uid_t uid, gid_t gid)
{
    int ret;
    long inode;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysql_chown(\"%s\", %ld, %ld)\n", path, uid, gid);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, path);
    if (inode < 0) {
        pool_put(dbconn);
        return inode;
    }

    ret = query_chown(dbconn, inode, uid, gid);
    if(ret){
        log_printf(LOG_ERROR, "Error: query_chown()\n");
        pool_put(dbconn);
        return -EIO;
    }

    pool_put(dbconn);

    return ret;
}

static int mysqlfs_truncate(const char* path, off_t length)
{
    int ret;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysql_truncate(\"%s\"): len=%lld\n", path, length);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    ret = query_truncate(dbconn, path, length);
    if (ret < 0) {
        log_printf(LOG_ERROR, "Error: query_length()\n");
        pool_put(dbconn);
        return -EIO;
    }

    pool_put(dbconn);

    return 0;
}

static int mysqlfs_utime(const char *path, struct utimbuf *time)
{
    int ret;
    long inode;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysql_utime(\"%s\")\n", path);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, path);
    if (inode < 0) {
        pool_put(dbconn);
        return inode;
    }

    ret = query_utime(dbconn, inode, time);
    if (ret < 0) {
        log_printf(LOG_ERROR, "Error: query_utime()\n");
        pool_put(dbconn);
        return -EIO;
    }

    pool_put(dbconn);

    return 0;
}

static int mysqlfs_open(const char *path, struct fuse_file_info *fi)
{
    MYSQL *dbconn;
    long inode;
    int ret;

    log_printf(LOG_D_CALL, "mysqlfs_open(\"%s\")\n", path);

#ifdef STATUSDIR
    /* take a short-circuit for the bogus virtual files */
    if (0 == strncmp (path, status_pathname, len_status_pathname))
    {
        char *a = (char *) path + len_status_pathname;

        log_printf(LOG_D_CALL, "%s(\"%s\")(@%d)\n", __FUNCTION__, a, __LINE__);
        if (0 == strcmp (a, "/txt"))
        {
            fi->fh = inode_status_txt;
            return 0;
        }
        else if (0 == strcmp (a, "/xml"))
        {
            fi->fh = inode_status_xml;
            return 0;
        }

        /* otherwise, fall-thru to a inode-lookup failure */
    }
#endif

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, path);
    if(inode < 0){
        pool_put(dbconn);
        return -ENOENT;
    }

    /* Save inode for future use. Lets us skip path->inode translation.  */
    fi->fh = inode;

    log_printf(LOG_D_OTHER, "inode(\"%s\") = %d\n", path, fi->fh);

    ret = query_inuse_inc(dbconn, inode, 1);
    if (ret < 0) {
        pool_put(dbconn);
        return ret;
    }

    pool_put(dbconn);

    return 0;
}

#ifdef STATUSDIR
static int snprint_status(char *dest, size_t size, struct mysqlfs_opt *opt, long inode)
{
    /* this needs to be migrated to header files so that the maintenance of pool.c doesn't ened a lock-step maintenance of this function */
    extern unsigned int lifo_unused_cnt;
    extern unsigned int lifo_pool_cnt;

    switch (inode)
    {
        case inode_status_txt: /* produce text/plain format */
            return snprintf (dest, size, "host: %s\nuser: %s\ndb:   %s\nport: %d\nuri:  "
                "mysql://%s@%s:%d/%s\nfsck: %slog:  %s\nblocksize:  %u\nosxnospotlight: %s\nconnections init: %d\nconnections idle: %d\n"
                "connections pool: %d\nconnections unused: %d\n",
                opt->host, opt->user, opt->db, (opt->port ? opt->port : MYSQL_PORT), opt->user, opt->host, (opt->port ? opt->port : MYSQL_PORT), opt->db,
                (opt->fsck ? "true" : "false"), opt->logfile, DATA_BLOCK_SIZE, (opt->osxnospotlight ? "true (block)" : "false (allow)"), opt->init_conns, opt->max_idling_conns, lifo_pool_cnt, lifo_unused_cnt);

        case inode_status_xml: /* produce text/xml format */
            return snprintf (dest, size, "<?xml version=\"1.0\"?>\n<mysqlfs xmlns=\"http://mysqlfs.sf.net/xsd/%s/statusfile.xsd\">\n  <host>%s</host>\n  <user>%s</user>\n  <db>%s</db>\n  <port>%d</port>\n"
                "  <uri>mysql://%s@%s:%d/%s</uri>\n  <fsck>%s</fsck>\n  <log>%s</log>\n  <blocksize>%u</blocksize>\n  <osxnospotlight>%s</osxnospotlight>\n"
                "  <connections>\n    <init>%d</init>\n    <idle>%d</idle>\n"
                "    <pool>%d</pool>\n    <unused>%d</unused>\n  </connections>\n</mysqlfs>\n",
                XMLVERSION, opt->host, opt->user, opt->db, (opt->port ? opt->port : MYSQL_PORT), opt->user, opt->host, (opt->port ? opt->port : MYSQL_PORT), opt->db,
                (opt->fsck ? "true" : "false"), opt->logfile, DATA_BLOCK_SIZE, (opt->osxnospotlight ? "true" : "false"), opt->init_conns, opt->max_idling_conns, lifo_pool_cnt, lifo_unused_cnt);
    }

    return -1;
}

static int mysqlfs_status_read(const char *subpath, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi)
{
    static char ok[8 * 1024];

    int len = snprint_status(ok, sizeof(ok), theopts, fi->fh);
    int l = ((len - offset) > size ? size : (len - offset));
    log_printf(LOG_D_CALL, "%s(\"%s\")(%d of %d)(@%d)\n", __FUNCTION__, subpath, offset, l, __LINE__);

    if (len < offset) return 0;

    if (offset < len)
        memcpy(buf, (ok + offset), l);

    return l;
}
#endif

static int mysqlfs_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi)
{
    int ret;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysqlfs_read(\"%s\" %zu@%llu)\n", path, size, offset);

#ifdef STATUSDIR
    /* take a short-circuit for the bogus virtual files */
    if (0 == strncmp (path, status_pathname, len_status_pathname))
    {
        char *a = (char *) path + len_status_pathname;
        log_printf(LOG_D_CALL, "%s(\"%s\")(@%d)\n", __FUNCTION__, a, __LINE__);
        return mysqlfs_status_read(a, buf, size, offset, fi);
    }
#endif
    /* see http://www.macosxhints.com/article.php?story=20060814124808745 */
    if ( (0 < theopts->osxnospotlight) && (0 == strcmp (path, "/.metadata_never_index")))
    {
        char *a = PACKAGE_STRING;				/* in case I cannot point directly to static content */
        int l = MIN(strlen(PACKAGE_STRING)-offset,size);	/* tossaway length calc */
        strncpy (buf, a+offset, l);
        return l;
    }

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    ret = query_read(dbconn, fi->fh, buf, size, offset);
    pool_put(dbconn);

    return ret;
}

static int mysqlfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    int ret;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysqlfs_write(\"%s\" %zu@%lld)\n", path, size, offset);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    ret = query_write(dbconn, fi->fh, buf, size, offset);
    pool_put(dbconn);

    return ret;
}

static int mysqlfs_release(const char *path, struct fuse_file_info *fi)
{
    int ret;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "mysqlfs_release(\"%s\")\n", path);

#ifdef STATUSDIR
    /* take a short-circuit for the bogus virtual files */
    switch (fi->fh)
    {
        case inode_status_txt:
        case inode_status_xml:
            return 0;

        /* there might be a bunch of files later; we fall-thru to the standard processing */
    }
#endif

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    ret = query_inuse_inc(dbconn, fi->fh, -1);
    if (ret < 0) {
        pool_put(dbconn);
        return ret;
    }

    ret = query_purge_deleted(dbconn, fi->fh);
    if (ret < 0) {
        pool_put(dbconn);
        return ret;
    }

    pool_put(dbconn);

    return 0;
}

static int mysqlfs_link(const char *from, const char *to)
{
    int ret;
    long inode, new_parent;
    MYSQL *dbconn;
    char *tmp, *name, esc_name[PATH_MAX * 2];

    log_printf(LOG_D_CALL, "link(%s, %s)\n", from, to);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, from);
    if(inode < 0){
        pool_put(dbconn);
        return inode;
    }

    tmp = strdup(to);
    name = dirname(tmp);
    new_parent = query_inode(dbconn, name);
    free(tmp);
    if (new_parent < 0) {
        pool_put(dbconn);
        return new_parent;
    }

    tmp = strdup(to);
    name = basename(tmp);
    mysql_real_escape_string(dbconn, esc_name, name, strlen(name));
    free(tmp);

    ret = query_mkdirentry(dbconn, inode, esc_name, new_parent);
    if(ret < 0){
        pool_put(dbconn);
        return ret;
    }

    pool_put(dbconn);

    return 0;
}

static int mysqlfs_symlink(const char *from, const char *to)
{
    int ret;
    int inode;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "%s(\"%s\" -> \"%s\")\n", __func__, from, to);

    ret = mysqlfs_mknod(to, S_IFLNK | 0755, 0);
    if (ret < 0)
      return ret;

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, to);
    if(inode < 0){
        pool_put(dbconn);
        return -ENOENT;
    }

    ret = query_write(dbconn, inode, from, strlen(from), 0);
    if (ret > 0) ret = 0;

    pool_put(dbconn);

    return ret;
}

static int mysqlfs_readlink(const char *path, char *buf, size_t size)
{
    int ret;
    long inode;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "%s(\"%s\")\n", __func__, path);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    inode = query_inode(dbconn, path);
    if(inode < 0){
        pool_put(dbconn);
        return -ENOENT;
    }

    memset (buf, 0, size);
    ret = query_read(dbconn, inode, buf, size, 0);
    log_printf(LOG_DEBUG, "readlink(%s): %s [%zd -> %d]\n", path, buf, size, ret);
    pool_put(dbconn);

    if (ret > 0) ret = 0;
    return ret;
}

static int mysqlfs_rename(const char *from, const char *to)
{
    int ret;
    MYSQL *dbconn;

    log_printf(LOG_D_CALL, "%s(%s -> %s)\n", __func__, from, to);

    // FIXME: This should be wrapped in a transaction!!!
    mysqlfs_unlink(to);

    if ((dbconn = pool_get()) == NULL)
      return -EMFILE;

    ret = query_rename(dbconn, from, to);

    pool_put(dbconn);

    return ret;
}

/** used below in fuse_main() to define the entry points for a FUSE filesystem; this is the same VMT-like jump table used throughout the UNIX kernel. */
static struct fuse_operations mysqlfs_oper = {
    .getattr	= mysqlfs_getattr,
    .readdir	= mysqlfs_readdir,
    .mknod	= mysqlfs_mknod,
    .mkdir	= mysqlfs_mkdir,
    .unlink	= mysqlfs_unlink,
    .rmdir	= mysqlfs_unlink,
    .chmod	= mysqlfs_chmod,
    .chown	= mysqlfs_chown,
    .truncate	= mysqlfs_truncate,
    .utime	= mysqlfs_utime,
    .open	= mysqlfs_open,
    .read	= mysqlfs_read,
    .write	= mysqlfs_write,
    .release	= mysqlfs_release,
    .link	= mysqlfs_link,
    .symlink	= mysqlfs_symlink,
    .readlink	= mysqlfs_readlink,
    .rename	= mysqlfs_rename,
};

/** print out a brief usage aide-memoire to stderr */
void usage(){
    fprintf(stderr,
            "usage: mysqlfs [opts] <mountpoint>\n\n");
    fprintf(stderr,
            "       mysqlfs [-osocket=/tmp/mysql.sock] [-oport=####] -ohost=host -ouser=user -opassword=password "
            "-odatabase=database ./mountpoint\n");
    fprintf(stderr,
            "       mysqlfs [-d] [-ologfile=filename] -ohost=host -ouser=user -opassword=password "
            "-odatabase=database ./mountpoint\n");
    fprintf(stderr,
            "       mysqlfs [-mycnf_group=group_name] -ohost=host -ouser=user -opassword=password "
            "-odatabase=database ./mountpoint\n");
    fprintf(stderr, "\n(mimick mysql options)\n");
    fprintf(stderr,
            "       mysqlfs --host=host --user=user --password=password --database=database ./mountpoint\n");
    fprintf(stderr,
            "       mysqlfs -h host -u user --password=password -D database ./mountpoint\n");
    fprintf(stderr, "\n(stop spotlight (specific host one-offs))\n");
    fprintf(stderr,
            "       mysqlfs --osxnospotlight --host=host --user=user --password=password --database=database ./mountpoint\n");
}

/** macro to set a call value with a default -- defined yet? */
#define MYSQLFS_OPT_KEY(t, p, v) { t, offsetof(struct mysqlfs_opt, p), v }

/** FUSE_OPT_xxx keys defines for use with fuse_opt_parse() */
enum
  {
    KEY_BACKGROUND,	/**< debug: key for option to activate mysqlfs::bg to force-background the server */
    KEY_DEBUG_DNQ,	/**< debug: Dump (Config) and Quit */
    KEY_HELP,
    KEY_VERSION,
  };

/** fuse_opt for use with fuse_opt_parse() */
static struct fuse_opt mysqlfs_opts[] =
  {
    MYSQLFS_OPT_KEY(  "background",	bg,	1),
    MYSQLFS_OPT_KEY(  "database=%s",	db,	1),
    MYSQLFS_OPT_KEY("--database=%s",	db,	1),
    MYSQLFS_OPT_KEY( "-D %s",		db,	1),
    MYSQLFS_OPT_KEY(  "fsck",		fsck,	1),
    MYSQLFS_OPT_KEY(  "fsck=%d",	fsck,	1),
    MYSQLFS_OPT_KEY("--fsck=%d",	fsck,	1),
    MYSQLFS_OPT_KEY("nofsck",		fsck,	0),
    MYSQLFS_OPT_KEY(  "host=%s",	host,	0),
    MYSQLFS_OPT_KEY("--host=%s",	host,	0),
    MYSQLFS_OPT_KEY( "-h %s",		host,	0),
    MYSQLFS_OPT_KEY(  "logfile=%s",	logfile,	0),
    MYSQLFS_OPT_KEY("--logfile=%s",	logfile,	0),
    MYSQLFS_OPT_KEY(  "mycnf_group=%s",	mycnf_group,	0), /* Read defaults from specified group in my.cnf  -- Command line options still have precedence.  */
    MYSQLFS_OPT_KEY("--mycnf_group=%s",	mycnf_group,	0),
    MYSQLFS_OPT_KEY(  "osxspotlight",	osxnospotlight,	0),
    MYSQLFS_OPT_KEY("noosxspotlight",	osxnospotlight,	1),
    MYSQLFS_OPT_KEY("osxnospotlight",	osxnospotlight,	1),
    MYSQLFS_OPT_KEY(  "password=%s",	passwd,	0),
    MYSQLFS_OPT_KEY("--password=%s",	passwd,	0),
    MYSQLFS_OPT_KEY(  "port=%d",	port,	0),
    MYSQLFS_OPT_KEY("--port=%d",	port,	0),
    MYSQLFS_OPT_KEY( "-P %d",		port,	0),
    MYSQLFS_OPT_KEY(  "socket=%s",	socket,	0),
    MYSQLFS_OPT_KEY("--socket=%s",	socket,	0),
    MYSQLFS_OPT_KEY( "-S %s",		socket,	0),
    MYSQLFS_OPT_KEY(  "user=%s",	user,	0),
    MYSQLFS_OPT_KEY("--user=%s",	user,	0),
    MYSQLFS_OPT_KEY( "-u %s",		user,	0),

    FUSE_OPT_KEY("debug-dnq",	KEY_DEBUG_DNQ),
    FUSE_OPT_KEY("-v",		KEY_VERSION),
    FUSE_OPT_KEY("--version",	KEY_VERSION),
    FUSE_OPT_KEY("--help",	KEY_HELP),
    FUSE_OPT_END
  };



static int mysqlfs_opt_proc(void *data, const char *arg, int key,
                            struct fuse_args *outargs){
    struct mysqlfs_opt *opt = (struct mysqlfs_opt *) data;

    switch (key)
    {
        case FUSE_OPT_KEY_OPT: /* dig through the list for matches */
    /*
     * There are primitives for this in FUSE, but no need to change at this point
     */
            break;

        case KEY_DEBUG_DNQ:
        /*
         * Debug: Dump Config and Quit -- used to debug options-handling changes
         */

            fprintf (stderr, "DEBUG: Dump and Quit\n\n");
            fprintf (stderr, "connect: mysql://%s:%s@%s:%d/%s\n", opt->user, opt->passwd, opt->host, (opt->port ? opt->port : MYSQL_PORT), opt->db);
            fprintf (stderr, "connect: sock://%s\n", opt->socket);
            fprintf (stderr, "fsck? %s\n", (opt->fsck ? "yes" : "no"));
            fprintf (stderr, "group: %s\n", opt->mycnf_group);
            fprintf (stderr, "pool: %d initial connections\n", opt->init_conns);
            fprintf (stderr, "pool: %d idling connections\n", opt->max_idling_conns);
            fprintf (stderr, "logfile: file://%s\n", opt->logfile);
            fprintf (stderr, "bg? %s (debug)\n\n", (opt->bg ? "yes" : "no"));

            exit (2);

        case KEY_HELP: /* trigger usage call */
	    usage ();
            exit (0);

        case KEY_VERSION: /* show version and quit */
	    fprintf (stderr, "%s-%s fuse-%2.1f\n\n", PACKAGE_TARNAME, PACKAGE_VERSION, ((double) FUSE_USE_VERSION)/10.0);
	    exit (0);

        default: /* key != FUSE_OPT_KEY_OPT */
            fuse_opt_add_arg(outargs, arg);
            return 0;
    }

    fuse_opt_add_arg(outargs, arg);
    return 0;
}

/**
 * main
 */
int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct mysqlfs_opt opt = {
	.init_conns	= 1,
	.max_idling_conns = 5,
	.mycnf_group	= "mysqlfs",
#ifdef DEBUG
	.logfile	= "mysqlfs.log",
#endif
    };

    log_file = stderr;

    /** theopts kludge is used for both statusdir (if configured at build) and for osxnospotlight */
    theopts = &opt;

    fuse_opt_parse(&args, &opt, mysqlfs_opts, mysqlfs_opt_proc);

    if (pool_init(&opt) < 0) {
        log_printf(LOG_ERROR, "Error: pool_init() failed\n");
        fuse_opt_free_args(&args);
        return EXIT_FAILURE;        
    }

    /*
     * I found that -- running from a script (ie no term?) -- the MySQLfs would not background, so the terminal is held; this makes automated testing difficult.
     *
     * I (allanc) put this into here to allow for AUTOTEST, but then autotest has to seek-and-destroy the app.  This isn't quite perfect yet, I get some flakiness here, othertines the pid is 4 more than the parent, which is odd.
     */
    if (0 < opt.bg)
    {
        if (0 < fork())
            return EXIT_SUCCESS;
        //else
        //    fprintf (stderr, "forked %d\n", getpid());
    }

    /* only create a log file if we have a logfile set; note that --enable-debug sets a default above */
    if (NULL != opt.logfile)
        log_file = log_init(opt.logfile, 1);

    fuse_main(args.argc, args.argv, &mysqlfs_oper);
    fuse_opt_free_args(&args);

    pool_cleanup();

    return EXIT_SUCCESS;
}
