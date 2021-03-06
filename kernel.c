/*
  This file contains the vnode layer used by the file system construction
  kit.  It is the file system independent layer and managed hooking up
  requests from the test program (fsh and tstfs) to the actual file system
  routines.  It provides a rather posix-like interface generally with
  the routines preceded by a "sys_" prefix.
  
  THIS CODE COPYRIGHT DOMINIC GIAMPAOLO.  NO WARRANTY IS EXPRESSED 
  OR IMPLIED.  YOU MAY USE THIS CODE AND FREELY DISTRIBUTE IT FOR
  NON-COMMERCIAL USE AS LONG AS THIS NOTICE REMAINS ATTACHED.

  FOR COMMERCIAL USE, CONTACT DOMINIC GIAMPAOLO (dbg@be.com).

  Dominic Giampaolo
  dbg@be.com
*/
#include "compat.h"

#include "skiplist.h"
#include "lock.h"
#include "fsproto.h"
#include "kprotos.h"

#include <sys/stat.h>

#define     OMODE_MASK      (O_RDONLY | O_WRONLY | O_RDWR)
#define     SLEEP_TIME      (10000.0)
#define     MAX_SYM_LINKS   16

#define     FREE_LIST       0
#define     USED_LIST       1
#define     LOCKED_LIST     2
#define     LIST_NUM        3

#define     FD_FILE         1
#define     FD_DIR          2
#define     FD_WD           4

#define     FD_ALL          (FD_FILE | FD_DIR | FD_WD)

#define     DEFAULT_FD_NUM  (128)
#define     VNNUM           256

typedef unsigned long       fsystem_id;

typedef struct vnode vnode;
typedef struct vnlist vnlist;
typedef struct vnlink vnlink;
typedef struct fsystem fsystem;
typedef struct nspace nspace;
typedef struct ofile ofile;
typedef struct ioctx ioctx;
typedef struct fdarray fdarray;

struct vnlist {
    vnode           *head;
    vnode           *tail;
    int             num;
};

struct vnlink {
    vnode           *prev;
    vnode           *next;
};

struct vnode {
    vnode_id        vnid;
    nspace *        ns;
    nspace *        mounted;
    char            remove;
    char            busy;
    char            inlist;
    char            watched;
    vnlink          nspace;
    vnlink          list;
    int             rcnt;
    void *          data;
};

struct fsystem {
    fsystem_id          fsid;
    bool                fixed;
    image_id            aid;
    char                name[IDENT_NAME_LENGTH];
    int                 rcnt;
    vnode_ops           ops;
};

struct nspace {
    nspace_id           nsid;
    my_dev_t            dev;
    my_ino_t            ino;
    fsystem             *fs;
    vnlist              vnodes;
    void                *data;
    vnode *             root;
    vnode *             mount;
    nspace *            prev;
    nspace *            next;
    char                shutdown;
};

struct ofile {
    short           type;
    ushort          flags;
    vnode *         vn;
    void *          cookie;
    long            rcnt;
    long            ocnt;

    fs_off_t            pos;
    int             omode;
};


struct ioctx {
    lock            lock;
    int             kerrno;
    vnode *         cwd;
    fdarray *       fds;
};

struct fdarray {
    long            rcnt;
    lock            lock;
    int             num;
    ulong           *alloc;
    ulong           *coes;
    ofile           *fds[1];
};

extern struct {
    const char *    name;
    vnode_ops *     ops;
} fixed_fs[];

static vnode_id     invalid_vnid = 0;
static vnode *      rootvn;
static int          max_glb_file;
static fdarray *    global_fds;
static vnlist       lists[LIST_NUM];
static nspace *     nshead;
static lock         vnlock;
static lock         fstablock;
static nspace **    nstab;
static fsystem **   fstab;
static int          nns;
static int          nfs;
static fsystem_id   nxfsid;
static nspace_id    nxnsid;
static SkipList     skiplist;
static int          vnnum;
static int          usdvnnum;



int                 sys_rstat(bool kernel, int fd, const char *path,
                              struct my_stat *st, bool eatlink);
static fsystem *    inc_file_system(const char *name);
static int          dec_file_system(fsystem *fs);

static int      get_dir_fd(bool kernel, int fd, const char *path, char *filename,
                    vnode **dvn);
static int      get_file_fd(bool kernel, int fd, const char *path,
                    int eatsymlink, vnode **vn);
static int      get_file_vn(nspace_id nsid, vnode_id vnid, const char *path,
                    int eatsymlink, vnode **vn);
static int      parse_path_fd(bool kerne, int fd, char **pstart,
                    int eatsymlink, vnode **vn);
static int      parse_path_vn(nspace_id nsid, vnode_id vnid, char **start,
                    int eatsymlink, vnode **vn);
static int      parse_path(vnode *bvn, char **pstart, char *path,
                    int eatsymlink, vnode **vn);

static char *   cat_paths(char *a, char *b);

static int      load_vnode(nspace_id nsid, vnode_id vnid, char r, vnode **vnp);
static vnode *  lookup_vnode(nspace_id nsid, vnode_id vnid);
static void     move_vnode(vnode *vn, int list);
static vnode *  steal_vnode(int list);
static void     flush_vnode(vnode *vn, char r);
static int      sort_vnode(vnode *vn);
static void     clear_vnode(vnode *vn);
static void     inc_vnode(vnode *vn);
static void     dec_vnode(vnode *vn, char r);
static int      compare_vnode(vnode *vna, vnode *vnb);

static nspace * nsidtons(nspace_id nsid);
static int      alloc_wd_fd(bool kernel, vnode *vn, bool coe, int *fdp);

static int      is_root(vnode *root, vnode **mount);
static int      is_mount_vnode(vnode *mount, vnode **root);
static int      is_mount_vnid(nspace_id nsid, vnode_id vnid, vnode_id *mount);

static ofile *      get_fd(bool kernel, int fd, int type);
static int          put_fd(ofile *f);
static int          new_fd(bool kernel, int nfd, ofile *f, int fd, bool coe);
static int          remove_fd(bool kernel, int fd, int type);
static int          get_coe(bool kernel, int fd, int type, bool *coe);
static int          set_coe(bool kernel, int fd, int type, bool coe);
static int          get_omode(bool kernel, int fd, int type, int *omode);
static int          invoke_close(ofile *f);
static int          invoke_free(ofile *f);

static fdarray *    new_fds(int num);
static int          free_fds(fdarray *fds);


#define BITSZ(n)        (((n) + 31) & ~31)
#define SETBIT(a,i,v)   *((a)+(i)/32) = (*((a)+(i)/32) & ~(1<<((i)%32))) | (v<<((i)%32))
#define GETBIT(a,i)     ((*((a)+(i)/32) & (1<<((i)%32))) >> ((i)%32))

/* ---------------------------------------------------------------- */

#include <stdio.h>

static void
PANIC(char *s)
{
    printf(s);
}



#ifdef DEBUG
int
dump_fsystem(int argc, char **argv)
{
    struct fsystem *fs;

    if (argv[1] == NULL) {
        kprintf("%s needs an address argument\n", argv[0]);
        return 1;
    }

    fs = (struct fsystem *)strtoul(argv[1], NULL, 0);
    
    kprintf("fs @ 0x%x name %s rcnt %d ops @ 0x%x\n", fs, fs->name,
            fs->rcnt, &fs->ops);
}

int
dump_ioctx(int argc, char **argv)
{
    struct ioctx *ioctx;

    if (argv[1] == NULL) {
        kprintf("%s needs an address argument\n", argv[0]);
        return 1;
    }

    ioctx = (struct ioctx *)strtoul(argv[1], NULL, 0);
    
    kprintf("ioctx @ 0x%x, kerrno %d, cwd 0x%x, fdarray 0x%x\n", ioctx,
            ioctx->kerrno, ioctx->cwd, ioctx->fds);
}


int
dump_vnode(int argc, char **argv)
{
    struct vnode *vn;

    if (argv[1] == NULL) {
        kprintf("%s needs an address argument\n", argv[0]);
        return 1;
    }

    vn = (vnode *)strtoul(argv[1], NULL, 0);
    
    kprintf("vnode @ 0x%x vnid 0x%x ns 0x%x mounted 0x%x\n", vn, vn->vnid,
            vn->ns, vn->mounted);
    kprintf("remove %d busy %d inlist %d\n", vn->remove, vn->busy, vn->inlist);
    kprintf("nspace 0x%x list 0x%x rcnt 0x%x data 0x%x\n", &vn->nspace,
            &vn->list, vn->rcnt, vn->data);
}


int
dump_fdarray(int argc, char **argv)
{
    int             i;
    struct fdarray *fds;

    if (argv[1] == NULL) {
        kprintf("%s needs an address argument\n", argv[0]);
        return 1;
    }

    fds = (struct fdarray *)strtoul(argv[1], NULL, 0);
    
    kprintf("fdarray @ 0x%x  rcnt %d lock %d num %d\n", fds, fds->rcnt,
            fds->num);
    kprintf("alloc 0x%x coes 0x%x\n", fds->alloc, fds->coes);
    for(i=0; i < fds->num; i++)
        if (fds->fds[i])
            kprintf("fd %3d @ 0x%x\n", i, fds->fds[i]);
}


int
dump_ofile(int argc, char **argv)
{
    struct ofile *ofile;

    if (argv[1] == NULL) {
        kprintf("%s needs an address argument\n", argv[0]);
        return 1;
    }

    ofile = (struct ofile *)strtoul(argv[1], NULL, 0);
    
    kprintf("ofile @ 0x%x type %d flags %d vn 0x%x cookie 0x%x\n",
            ofile, ofile->type, ofile->flags, ofile->vn, ofile->cookie);
    kprintf("rcnt %d ocnt %d pos 0x%x omode 0x%x\n", ofile->rcnt, ofile->ocnt,
            ofile->pos, ofile->omode);
}

int
dump_nspace(int argc, char **argv)
{
    struct nspace *ns;

    if (argv[1] == NULL) {
        kprintf("%s needs an address argument\n", argv[0]);
        return 1;
    }

    ns = (struct nspace *)strtoul(argv[1], NULL, 0);
    
    kprintf("ns @ 0x%x nsid %d vnlist @ 0x%x data 0x%x\n", ns, ns->nsid,
            &ns->vnodes, ns->data);
    kprintf("root 0x%x mount 0x%x prev 0x%x next 0x%x\n", ns->root, ns->mount,
            ns->prev, ns->next);
    kprintf("shutdown %d fs @ 0x%x\n", ns->shutdown, ns->fs);
}

void
do_dump_io_info(thread_rec *thr)
{
    int i;
    struct fdarray *fds;

    if (thr->ioctx == NULL || thr->ioctx->fds == NULL) {
        kprintf("thread: %d (%s)\n  No io info?!?\n", thr->thid, thr->name);
        return;
    }
        
    kprintf("thread: %d (%s)\n", thr->thid, thr->name);
    fds = thr->ioctx->fds;

    for(i=0; i < fds->num; i++)
        if (fds->fds[i])
            kprintf("  fd %3d vnode @ 0x%.8x (vnid 0x%.8x, data 0x%.8x)\n", i,
                    fds->fds[i]->vn, fds->fds[i]->vn->vnid,
                    fds->fds[i]->vn->data);
}


int
dump_io_info(int argc, char **argv)
{
    int             i;
    thread_rec     *thr;

    if (argv[1] == NULL) {
        kprintf("%s needs an thread name/address argument\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "-n") == 0) {
        int len;
        
        /* hunt for the name in argv[2] */
        if (argv[2] == NULL) {
            kprintf("thread: the `-name' option requires an argument\n");
            return 0;
        }

        len = strlen(argv[2]);
        for(i=0; i < nthreads; i++) {
            if (thread_tab[i] == NULL) 
                continue;

            if (mystrstr(thread_tab[i]->name, argv[2]) != NULL) {
                thr = thread_tab[i];
                do_dump_io_info(thr);
            }
        }
    } else {
        ulong num;

        num = strtoul(argv[2], NULL, 0);

        if (num < 0x2ffff && isbadthid(num) == 0)
            thr = thread_tab[thidtoslot(num)];
        else
            thr = (thread_rec *)num;
        
        if (thr == 0)
            return 0;

        do_dump_io_info(thr);
    }

    return 1;
}



int
find_vn(int argc, char **argv)
{
    nspace      *ns;
    vnode       *vn;
    vnode       fakevn;
    vnode_id    vnid;

    if (argv[1] == NULL) {
        kprintf("%s needs a vnid argument\n", argv[0]);
        return 1;
    }

    vnid = (vnode_id)strtoul(argv[1], NULL, 0);

    for(ns=nshead; ns; ns=ns->next) {
        fakevn.ns = ns;
        fakevn.vnid = vnid;
        vn = SearchSL(skiplist, &fakevn);
        if (vn)
            kprintf("vn = 0x%x (nsid = %d)\n", vn, vn->ns->nsid);
    }

    return 0;
}

#endif /* DEBUG */


/* ---------------------------------------------------------------- */

#ifdef USER
int memsize = 8 * 1024 * 1024;
#endif

int
init_vnode_layer(void)
{
    int         err;
    vnode       *vns;
    vnode_id    vnid;
    int         i;
    fsystem     *fs;
    nspace      *ns;
    void        *data;
    size_t      sz;
    extern vnode_ops rootfs;  /* XXXdbg */

    /*
    compute vnnum based on memsize. 256 vnodes with 8MB.
    compute usdvnnum based on vnnum. only 1/4 of total vnodes should
    remain unlocked. 
    */

    vnnum = memsize >> 15;
    usdvnnum = vnnum >> 2;

    vns = (vnode *) calloc(sizeof(vnode) * vnnum, 1);
    for(i=0; i<vnnum; i++) {
        vns[i].vnid = invalid_vnid;
        vns[i].ns = NULL;
        vns[i].mounted = NULL;
        vns[i].remove = FALSE;
        vns[i].busy = FALSE;
        vns[i].inlist = FREE_LIST;
        vns[i].nspace.next = vns[i].nspace.prev = NULL;
        vns[i].list.next = (i == vnnum-1 ? NULL : &vns[i+1]);
        vns[i].list.prev = (i == 0 ? NULL : &vns[i-1]);
        vns[i].rcnt = 0;
        vns[i].data = NULL;
    }
    lists[FREE_LIST].head = &vns[0];
    lists[FREE_LIST].tail = &vns[vnnum-1];
    lists[FREE_LIST].num = vnnum;
    lists[USED_LIST].head = NULL;
    lists[USED_LIST].tail = NULL;
    lists[USED_LIST].num = 0;
    lists[LOCKED_LIST].head = NULL;
    lists[LOCKED_LIST].tail = NULL;
    lists[LOCKED_LIST].num = 0;
    
    skiplist = NewSL(&compare_vnode, NULL, NO_DUPLICATES);

    /*
    set max # of file systems and mount points.
    with 8MB, up to 32 fs and 64 mount points.
    */

    nns = memsize >> 17;
    nfs = memsize >> 18;

    nxfsid = 1;
    nxnsid = 1;
    nstab = (nspace **) malloc(nns * sizeof(void *));
    memset(nstab, 0, nns * sizeof(void *));
    fstab = (fsystem **) malloc(nfs * sizeof(void *));
    memset(fstab, 0, nfs * sizeof(void *));

    new_lock(&vnlock, "vnlock");
    new_lock(&fstablock, "fstablock");

    /*
    determine the max number of files the kernel can open.
    8MB -> 256
    */

    max_glb_file = memsize >> 15;
    global_fds = new_fds(max_glb_file);


    /*
    install file systems
    */
    install_file_system(&rootfs, "rootfs", TRUE, -1);

    /*
    mount the root file system
    */

    fs = inc_file_system("rootfs");

    ns = (nspace *) malloc(sizeof(nspace));
    ns->fs = fs;
    ns->nsid = nxnsid++;
    nstab[ns->nsid % nns] = ns;
    ns->vnodes.head = ns->vnodes.tail = NULL;
    ns->data = NULL;
    ns->root = NULL;
    ns->mount = NULL;
    ns->shutdown = FALSE;
    ns->prev = ns->next = NULL;
    nshead = ns;

    err = (*fs->ops.mount)(ns->nsid, NULL, 0, NULL, 0, &data, &vnid);
    ns->data = data;
    ns->root = lookup_vnode(ns->nsid, vnid);
    rootvn = ns->root;


#ifdef DEBUG

    add_debugger_cmd("ioctx",   dump_ioctx,   "dump a thread ioctx struct");
    add_debugger_cmd("vnode",   dump_vnode,   "dump a vnode struct");
    add_debugger_cmd("fdarray", dump_fdarray, "dump an fd array");
    add_debugger_cmd("ofile",   dump_ofile,   "dump an ofile struct");
    add_debugger_cmd("nspace",  dump_nspace,  "dump a nspace struct");
    add_debugger_cmd("fsystem", dump_fsystem, "dump a fsystem struct");
    add_debugger_cmd("ioinfo",  dump_io_info, "dump io info for a thread");
    add_debugger_cmd("findvn",  find_vn,      "find a vnid (in all threads)");

#endif /* DEBUG */

    return 0;
}


/* ---------------------------------------------------------------- */

int
sys_sync(void)
{
    nspace      *ns;
    op_sync     *op;

    LOCK(vnlock);
    for(ns = nshead; ns; ns = ns->next) {
        ns->root->rcnt++;
        UNLOCK(vnlock);
        op = ns->fs->ops.sync;
        if (op)
            (*op)(ns->data);
        LOCK(vnlock);
        ns->root->rcnt--;
    }
    UNLOCK(vnlock);
    return 0;
}

static ioctx *
get_cur_ioctx(void)
{
    static int init = 0;
    static ioctx io;

    if (init == 0) {
        init = 1;
        memset(&io, 0, sizeof(io));
    }

    return &io;
}


/*
 * sys_chdir
 */

int
sys_chdir(bool kernel, int fd, const char *path)
{
    int              err;
    ioctx           *io;
    vnode           *vn;
    op_rstat        *op;
    struct my_stat   st;

    err = get_file_fd(kernel, fd, path, TRUE, &vn);
    if (err)
        goto error1;
    op = vn->ns->fs->ops.rstat;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(vn->ns->data, vn->data, &st);
    if (err)
        goto error2;
    if (!MY_S_ISDIR(st.mode)) {
        err = ENOTDIR;
        goto error2;
    }
    io = get_cur_ioctx();
    LOCK(io->lock);
    dec_vnode(io->cwd, FALSE);
    io->cwd = vn;
    UNLOCK(io->lock);

    return 0;   

error2:
    dec_vnode(vn, FALSE);
error1:
    return err;
}


/*
 * sys_access
 */

int
sys_access(bool kernel, int fd, const char *path, int mode)
{
    int         err;
    vnode       *vn;
    op_access   *op;

    err = get_file_fd(kernel, fd, path, TRUE, &vn);
    if (err)
        goto error1;
    op = vn->ns->fs->ops.access;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(vn->ns->data, vn->data, mode);
    if (err)
        goto error2;
    dec_vnode(vn, FALSE);
    return 0;

error2:
    dec_vnode(vn, FALSE);
error1:
    return err;
}

/*
 * sys_symlink
 */

int
sys_symlink(bool kernel, const char *oldpath, int nfd, const char *newpath)
{
    int             err;
    char            filename[FILE_NAME_LENGTH];
    char            *buf;
    vnode           *dvn;
    op_symlink      *op;

    err = get_dir_fd(kernel, nfd, newpath, filename, &dvn);
    if (err)
        goto error1;
    err = new_path(oldpath, &buf);
    if (err)
        goto error2;
    op = dvn->ns->fs->ops.symlink;
    if (!op) {
        err = EINVAL;
        goto error3;
    }
    err = (*op)(dvn->ns->data, dvn->data, filename, buf);
    if (err)
        goto error3;

    dec_vnode(dvn, FALSE);
    free_path(buf);

    return 0;

error3:
    free_path(buf);
error2:
    dec_vnode(dvn, FALSE);
error1:
    return err;
}

/*
 * sys_readlink
 */

ssize_t
sys_readlink(bool kernel, int fd, const char *path, char *buf, size_t bufsize)
{
    int             err;
    vnode           *vn;
    op_readlink     *op;
    size_t          sz;

    err = get_file_fd(kernel, fd, path, FALSE, &vn);
    if (err)
        goto error1;
    op = vn->ns->fs->ops.readlink;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    sz = bufsize;
    err = (*op)(vn->ns->data, vn->data, buf, &sz);
    if (err)
        goto error2;
    dec_vnode(vn, FALSE);

    return sz;

error2:
    dec_vnode(vn, FALSE);
error1:
    return err;
}

/*
 * sys_mkdir
 */

int
sys_mkdir(bool kernel, int fd, const char *path, int perms)
{
    int             err;
    char            filename[FILE_NAME_LENGTH];
    vnode           *dvn;
    op_mkdir        *op;

    err = get_dir_fd(kernel, fd, path, filename, &dvn);
    if (err)
        goto error1;
    op = dvn->ns->fs->ops.mkdir;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(dvn->ns->data, dvn->data, filename, perms);
    if (err)
        goto error2;

    dec_vnode(dvn, FALSE);

    return 0;

error2:
    dec_vnode(dvn, FALSE);
error1:
    return err;
}


/*
 * opendir.
 */

int
sys_opendir(bool kernel, int fd, const char *path, bool coe)
{
    int             err;
    op_opendir      *op;
    op_free_cookie  *opf;
    ofile           *f;
    int             nfd;
    vnode           *vn;
    void            *cookie;

    err = get_file_fd(kernel, fd, path, TRUE, &vn);
    if (err)
        goto error1;
    op = vn->ns->fs->ops.opendir;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(vn->ns->data, vn->data, &cookie);
    if (err)
        goto error2;

    /*
    find a file descriptor
    */

    f = (ofile *) calloc(sizeof(ofile), 1);
    if (!f) {
        err = ENOMEM;
        goto error3;
    }

    f->type = FD_DIR;
    f->vn = vn;
    f->cookie = cookie;
    f->rcnt = 0;
    f->ocnt = 0;

    nfd = new_fd(kernel, -1, f, -1, coe);
    if (nfd < 0) {
        err = EMFILE;
        goto error4;
    }

    return nfd;

error4:
    free(f);
error3:
    (*vn->ns->fs->ops.closedir)(vn->ns->data, vn->data, cookie);
    opf = vn->ns->fs->ops.free_dircookie;
    if (opf)
        (*opf)(vn->ns->data, vn->data, cookie);
error2:
    dec_vnode(vn, FALSE);
error1:
    if (err > 0)        /* XXXdbg -- a hack for linux */
        err = -err;

    return err;
}


/*
 * closedir
 */

int
sys_closedir(bool kernel, int fd)
{
    return remove_fd(kernel, fd, FD_DIR);
}

/*
 * readdir.
 */

int
sys_readdir(bool kernel, int fd, struct my_dirent *buf, size_t bufsize,
        long count)
{
    ofile            *f;
    int               err;
    vnode            *vn;
    struct my_dirent *p;
    struct my_stat    st;
    long              i;
    nspace_id         nsid;
    vnode_id          vnid;
    long              nm;

    f = get_fd(kernel, fd, FD_DIR);
    if (!f) {
        err = EBADF;
        goto error1;
    }
    vn = f->vn;
    nm = count;
    err = (*vn->ns->fs->ops.readdir)(vn->ns->data, vn->data, f->cookie,
            &nm, buf, bufsize);
    if (err)
        goto error1;

    /*
    patch the mount points and the root.
    */

    LOCK(vnlock);
    nsid = vn->ns->nsid;
    p = buf;
    for(i=0; i<nm; i++) {
        if (is_mount_vnid(nsid, p->d_ino, &vnid))
            p->d_ino = vnid;
        if (vn->ns->mount && !strcmp(p->d_name, "..")) {
            UNLOCK(vnlock);
            err = sys_rstat(kernel, fd, "..", &st, FALSE);
            if (err)
                goto error2;
            LOCK(vnlock);
            p->d_ino = st.ino;
        }
        p = (struct my_dirent *) ((char *) p + p->d_reclen);
    }
    UNLOCK(vnlock);

    put_fd(f);
    return nm;

error2:
    put_fd(f);
error1:
    return err;
}


/*
 * rewinddir
 */

int
sys_rewinddir(bool kernel, int fd)
{
    ofile       *f;
    int         err;
    vnode       *vn;

    f = get_fd(kernel, fd, FD_DIR);
    if (!f)
        return EBADF;
    vn = f->vn;
    err = (*vn->ns->fs->ops.rewinddir)(vn->ns->data, vn->data, f->cookie);
    put_fd(f);
    return err;
}


/*
 * open/create files.
 */

int
sys_open(bool kernel, int fd, const char *path, int omode, int perms,
        bool coe)
{
    int             err;
    char            filename[FILE_NAME_LENGTH];
    vnode           *vn, *dvn;
    vnode_id        vnid;
    void            *cookie;
    ofile           *f;
    int             nfd;
    fdarray         *fds;
    op_create       *opc;
    op_open         *opo;
    op_free_cookie  *opf;

    if (omode & O_CREAT) {
        err = get_dir_fd(kernel, fd, path, filename, &dvn);
        if (err)
            goto errorA;
        opc = dvn->ns->fs->ops.create;
        if (!opc) {
            err = EINVAL;
            goto errorB;
        }
        err = (*opc)(dvn->ns->data, dvn->data, filename, omode, perms, &vnid,
                        &cookie);
        if (err)
            goto errorB;
        LOCK(vnlock);
        vn = lookup_vnode(dvn->ns->nsid, vnid);
        UNLOCK(vnlock);

        dec_vnode(dvn, FALSE);
    } else {
        err = get_file_fd(kernel, fd, path, TRUE, &vn);
        if (err)
            goto error1;
        opo = vn->ns->fs->ops.open;
        if (!opo) {
            err = EINVAL;
            goto error2;
        }
        err = (*opo)(vn->ns->data, vn->data, omode, &cookie);
        if (err)
            goto error2;
    }

    /*
    find a file descriptor
    */

    f = (ofile *) calloc(sizeof(ofile), 1);
    if (!f) {
        err = ENOMEM;
        goto error3;
    }

    f->type = FD_FILE;
    f->vn = vn;
    f->cookie = cookie;
    f->ocnt = 0;
    f->rcnt = 0;
    f->pos = 0;
    f->omode = omode;

    nfd = new_fd(kernel, -1, f, -1, coe);
    if (nfd < 0) {
        err = EMFILE;
        goto error4;
    }

    return nfd;

error4:
    free(f);
error3:
    (*vn->ns->fs->ops.close)(vn->ns->data, vn->data, cookie);
    opf = vn->ns->fs->ops.free_cookie;
    if (opf)
        (*opf)(vn->ns->data, vn->data, cookie);
    if (omode & O_CREAT)
        goto errorC;
error2:
    dec_vnode(vn, FALSE);
error1:
    return err;

errorC:
    (*vn->ns->fs->ops.unlink)(dvn->ns->data, dvn->data, filename);
    dec_vnode(vn, FALSE);
errorB:
    dec_vnode(dvn, FALSE);
errorA:
    if (err > 0)        /* XXXdbg -- a hack for linux */
        err = -err;

    return err;
}


/*
 * sys_close
 */

int
sys_close(bool kernel, int fd)
{
    return remove_fd(kernel, fd, FD_FILE);
}


/*
 * sys_lseek
 */

fs_off_t
sys_lseek(bool kernel, int fd, fs_off_t pos, int whence)
{
    ofile           *f;
    int              err;
    struct my_stat   st;
    vnode           *vn;
    op_rstat        *op;
    
    f = get_fd(kernel, fd, FD_FILE);
    if (!f) {
        err = EBADF;
        goto error1;
    }
    switch(whence) {
    case SEEK_SET:
        f->pos = pos;
        break;
    case SEEK_CUR:
        if ((f->omode & O_APPEND) == 0)
            f->pos += pos;
        else {               /* we're in append mode so ask where the EOF is */
            vn = f->vn;
            op = vn->ns->fs->ops.rstat;
            if (!op) {
                err = EINVAL;
                goto error2;
            }
            err = (*op)(vn->ns->data, vn->data, &st);
            if (err)
                goto error2;
            pos += st.size;
            f->pos = pos;
            break;
        }
        break;
    case SEEK_END:
        vn = f->vn;
        op = vn->ns->fs->ops.rstat;
        if (!op) {
            err = EINVAL;
            goto error2;
        }
        err = (*op)(vn->ns->data, vn->data, &st);
        if (err)
            goto error2;
        pos += st.size;
        f->pos = pos;
        break;
    default:
        put_fd(f);
        return EINVAL;
    }

    if (f->pos < 0) {
        f->pos = 0;
        err = EINVAL;
        goto error2;
    }

    pos = f->pos;
    put_fd(f);
    return pos;

error2:
    put_fd(f);
error1:
    return err;
}


/*
 * sys_read
 */


ssize_t
sys_read(bool kernel, int fd, void *buf, size_t len)
{
    ofile       *f;
    int         err;
    vnode       *vn;
    size_t      sz;


    f = get_fd(kernel, fd, FD_FILE);
    if (!f) {
        err = EBADF;
        goto error1;
    }
    if ((f->omode & OMODE_MASK) == O_WRONLY) {
        err = EBADF;
        goto error2;
    }
    vn = f->vn;
    sz = len;
    err = (*vn->ns->fs->ops.read)(vn->ns->data, vn->data, f->cookie, f->pos,
        buf, &sz);
    if (err)
        goto error2;

    /*
    the update of f->pos is not protected. does it matter?
    simultaneous I/Os on the same file are unpredictable anyway.
    */

    f->pos += sz;

    put_fd(f);

    return sz;

error2:
    put_fd(f);  
error1:
    return err;
}

/*
 * sys_write
 */

ssize_t
sys_write(bool kernel, int fd, void *buf, size_t len)
{
    ofile       *f;
    int         err;
    vnode       *vn;
    size_t      sz;

    f = get_fd(kernel, fd, FD_FILE);
    if (!f) {
        err = EBADF;
        goto error1;
    }
    if ((f->omode & OMODE_MASK) == O_RDONLY) {
        err = EBADF;
        goto error2;
    }
    vn = f->vn;
    sz = len;
    err = (*vn->ns->fs->ops.write)(vn->ns->data, vn->data, f->cookie, f->pos,
        buf, &sz);
    if (err)
        goto error2;

    /*
    the update of f->pos is not protected. does it matter?
    simultaneous I/Os on the same file are unpredictable anyway.
    */

    f->pos += sz;

    put_fd(f);
    return sz;

error2:
    put_fd(f);  
error1:
    return err;
}



/*
 * sys_ioctl
 */

int
sys_ioctl(bool kernel, int fd, int cmd, void *arg, size_t sz)
{
    ofile       *f;
    int         err;
    vnode       *vn;
    op_ioctl    *op;

    f = get_fd(kernel, fd, FD_FILE);
    if (!f) {
        err = EBADF;
        goto error1;
    }
    vn = f->vn;
    op = vn->ns->fs->ops.ioctl;
    if (op)
        err = (*op)(vn->ns->data, vn->data, f->cookie, cmd, arg, sz);
    else
        err = EINVAL;
    if (err)
        goto error2;

    put_fd(f);
    return 0;

error2:
    put_fd(f);
error1:
    return err;
}


/*
 * sys_link
 */

int
sys_link(bool kernel, int ofd, const char *oldpath, int nfd,
    const char *newpath)
{
    int             err;
    vnode           *vn, *dvn;
    char            filename[FILE_NAME_LENGTH];
    op_link         *op;
    
    err = get_file_fd(kernel, ofd, oldpath, TRUE, &vn);
    if (err)
        goto error1;
    err = get_dir_fd(kernel, nfd, newpath, filename, &dvn);
    if (err)
        goto error2;

    if (vn->ns != dvn->ns) {
        err = EXDEV;
        goto error3;
    }
    op = dvn->ns->fs->ops.link;
    if (!op) {
        err = EINVAL;
        goto error3;
    }
    err = (*op)(dvn->ns->data, dvn->data, filename, vn->data);
    if (err)
        goto error3;

    dec_vnode(dvn, FALSE);
    dec_vnode(vn, FALSE);
    return 0;

error3:
    dec_vnode(dvn, FALSE);
error2:
    dec_vnode(vn, FALSE);
error1:
    return err;
}

/*
 * sys_unlink
 */

int
sys_unlink(bool kernel, int fd, const char *path)
{
    int             err;
    vnode           *dvn;
    char            filename[FILE_NAME_LENGTH];
    op_unlink       *op;
    
    err = get_dir_fd(kernel, fd, path, filename, &dvn);
    if (err)
        goto error1;

    op = dvn->ns->fs->ops.unlink;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(dvn->ns->data, dvn->data, filename);
    if (err)
        goto error2;

    dec_vnode(dvn, FALSE);
    return 0;

error2:
    dec_vnode(dvn, FALSE);
error1:
    return err;
}


/*
 * sys_rmdir
 */

int
sys_rmdir(bool kernel, int fd, const char *path)
{
    int             err;
    vnode           *dvn;
    char            filename[FILE_NAME_LENGTH];
    op_unlink       *op;
    
    err = get_dir_fd(kernel, fd, path, filename, &dvn);
    if (err)
        goto error1;

    op = dvn->ns->fs->ops.rmdir;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(dvn->ns->data, dvn->data, filename);
    if (err)
        goto error2;

    dec_vnode(dvn, FALSE);
    return 0;

error2:
    dec_vnode(dvn, FALSE);
error1:
    return err;
}

/*
 * sys_rename
 */

int
sys_rename(bool kernel, int ofd, const char *oldpath,
    int nfd, const char *newpath)
{
    int             err;
    char            newname[FILE_NAME_LENGTH], oldname[FILE_NAME_LENGTH];
    vnode           *odvn, *ndvn;
    op_rename       *op;

    err = get_dir_fd(kernel, ofd, oldpath, oldname, &odvn);
    if (err)
        goto error1;
    err = get_dir_fd(kernel, nfd, newpath, newname, &ndvn);
    if (err)
        goto error2;
    
    if (odvn->ns != ndvn->ns) {
        err = EXDEV;
        goto error2;
    }

    op = odvn->ns->fs->ops.rename;
    if (!op) {
        err = EINVAL;
        goto error3;
    }
    err = (*op)(odvn->ns->data, odvn->data, oldname, ndvn->data, newname);
    if (err)
        goto error3;

    dec_vnode(odvn, FALSE);
    dec_vnode(ndvn, FALSE);

    return 0;

error3:
    dec_vnode(ndvn, FALSE);
error2:
    dec_vnode(odvn, FALSE);
error1:
    return err;
}

/*
 * sys_rstat
 */

int
sys_rstat(bool kernel, int fd, const char *path, struct my_stat *st,
          bool eatlink)
{
    int         err;
    vnode       *vn;
    op_rstat    *op;

    err = get_file_fd(kernel, fd, path, eatlink, &vn);
    if (err)
        goto error1;
    op = vn->ns->fs->ops.rstat;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(vn->ns->data, vn->data, st);
    if (err)
        goto error2;
    dec_vnode(vn, FALSE);

    return 0;

error2:
    dec_vnode(vn, FALSE);
error1:
    return err;
}

/*
 * sys_wstat
 */

int
sys_wstat(bool kernel, int fd, const char *path, struct my_stat *st, long mask,
        bool eatlink)
{
    int         err;
    vnode       *vn;
    op_wstat    *op;

    err = get_file_fd(kernel, fd, path, eatlink, &vn);
    if (err)
        goto error1;
    op = vn->ns->fs->ops.wstat;
    if (!op) {
        err = EINVAL;
        goto error2;
    }
    err = (*op)(vn->ns->data, vn->data, st, mask);
    if (err)
        goto error2;
    dec_vnode(vn, FALSE);

    return 0;

error2:
    dec_vnode(vn, FALSE);
error1:
    return err;
}


/*
 * sys_mount
 */

void *
sys_mount(bool kernel, const char *filesystem, int fd, const char *where,
        const char *device, ulong flags, void *parms, size_t len)
{
    int                 err;
    int                 i;
    vnode_id            vnid;
    vnode               *mount;
    fsystem             *fs;
    nspace              *ns, *ans;
    void                *data;
    struct stat         st;
    struct my_stat      mst;
    my_dev_t            dev;
    my_ino_t            ino;
    
    dev = -1;
    ino = -1;
    if (device) {
        err = stat(device, &st);
        if (err)
            return NULL;
        dev = st.st_dev;
        ino = st.st_ino;
    }

    err = get_file_fd(TRUE, fd, where, TRUE, &mount);
    if (err)
        goto error1;

    err = (*mount->ns->fs->ops.rstat)(mount->ns->data, mount->data, &mst);
    if (err)
        goto error2;
    if (!MY_S_ISDIR(mst.mode)) {
        err = ENOTDIR;
        goto error2;
    }

    ns = (nspace *) malloc(sizeof(nspace));
    if (!ns) {
        err = ENOMEM;
        goto error2;
    }

    fs = inc_file_system(filesystem);
    if (!fs) {
        err = ENODEV;
        goto error3;
    }

    LOCK(vnlock);

    if (device) {
        for(ans=nshead; ans; ans=ans->next)
            if ((ans->dev == dev) && (ans->ino == ino)) {
                UNLOCK(vnlock);
printf("KERNEL: trying to mount %s twice (already mounted as %s)\n", device, ans->fs->name);
                err = ENODEV;
                goto error4;
            }
    }

    for(i=0; i<nns; i++, nxnsid++)
        if (!nstab[nxnsid % nns]) {
            nstab[nxnsid % nns] = ns;
            ns->nsid = nxnsid;
            nxnsid++;
            break;
        }
    if (i == nns) {
        UNLOCK(vnlock);
        err = EMFILE;
        goto error4;
    }
    nstab[ns->nsid % nns] = ns;
    ns->fs = fs;
    ns->vnodes.head = ns->vnodes.tail = NULL;
    ns->data = NULL;
    ns->root = NULL;
    ns->mount = NULL;
    ns->prev = NULL;
    ns->next = nshead;
    ns->shutdown = FALSE;
    ns->dev = dev;
    ns->ino = ino;
    if (nshead)
        nshead->prev = ns;
    nshead = ns;

    UNLOCK(vnlock);

    err = (*fs->ops.mount)(ns->nsid, device, flags, parms, len, &data, &vnid);
    if (err)
        goto error5;
        
    LOCK(vnlock);
    ns->root = lookup_vnode(ns->nsid, vnid);
    ns->data = data;
    
    if ((mount == rootvn) || (mount->mounted)) {
        err = EBUSY;
        goto error6;
    }
    mount->mounted = ns;
    ns->mount = mount;


    UNLOCK(vnlock);

    return data;

error6:
    dec_vnode(ns->root, FALSE);
    (*fs->ops.unmount)(data);
error5:
    LOCK(vnlock);
    nstab[ns->nsid % nns] = NULL;
    if (ns->prev)
        ns->prev->next = ns->next;
    else
        nshead = ns->next;
    if (ns->next)
        ns->next->prev = ns->prev;
    UNLOCK(vnlock);
error4:
    dec_file_system(fs);
error3:
    free(ns);
error2:
    dec_vnode(mount, FALSE);
error1:
    errno = err;
    return NULL;
}


/*
 * sys_unmount
 */

int
sys_unmount(bool kernel, int fd, const char *where)
{
    int             err;
    nspace          *ns;
    fsystem         *fs;
    vnode           *root, *vn, *mount;
    
    err = get_file_fd(TRUE, fd, where, TRUE, &root);
    if (err)
        goto error1;

    LOCK(vnlock);

    ns = root->ns;
    fs = ns->fs;
    if (ns->root != root) {
        err = EINVAL;
        goto error2;
    }


    /*
    don't allow to unmount the root file system
    */

    if (root == rootvn) {
        err = EBUSY;
        goto error2;
    }

    /*
    decrement twice root: one for the mount, one for the get_file.
    */

    root->rcnt -= 2;

    for(vn = ns->vnodes.head; vn; vn = vn->nspace.next)
        if (vn->busy || (vn->rcnt != 0)) {
            err = EBUSY;
            goto error3;
        }

    mount = ns->mount;
    mount->mounted = NULL;

    ns->shutdown = TRUE;
    for(vn = ns->vnodes.head; vn; vn = vn->nspace.next)
        vn->busy = TRUE;
        
    while (ns->vnodes.head) {
        vn = ns->vnodes.head;
        UNLOCK(vnlock);
        err = (*fs->ops.release_vnode)(vn->ns->data, vn->data, FALSE);
        LOCK(vnlock);
        if (err)
            PANIC("ERROR WRITING VNODE!!!\n");
        vn->busy = FALSE;
        clear_vnode(vn);
        move_vnode(vn, FREE_LIST);
    }

    if (ns->prev)
        ns->prev->next = ns->next;
    else
        nshead = ns->next;
    if (ns->next)
        ns->next->prev = ns->prev;

    nstab[ns->nsid % nns] = NULL;
    UNLOCK(vnlock);

    (*fs->ops.unmount)(ns->data);

    free(ns);

    dec_file_system(fs);
    dec_vnode(mount, FALSE);

    return 0;

error3:
    root->rcnt++;
error2:
    UNLOCK(vnlock);
error1:
    return err;
}


/*
 * get_dir and get_file: basic functions to parse a path and get the vnode
 * for either the parent directory or the file itself.
 */

static int
get_dir_fd(bool kernel, int fd, const char *path, char *filename, vnode **dvn)
{
    int         err;
    char        *p, *np;

    err = new_path(path, &p);
    if (err)
        goto error1;
    np = strrchr(p, '/');
    if (!np) {
        strcpy(filename, p);
        strcpy(p, ".");
    } else {
        strcpy(filename, np+1);
        np[1] = '.';
        np[2] = '\0';
    }
    err = parse_path_fd(kernel, fd, &p, TRUE, dvn);
    if (err)
        goto error2;
    free_path(p);
    return 0;
    
error2:
    free_path(p);
error1:
    return err;
}

static int
get_file_fd(bool kernel, int fd, const char *path, int eatsymlink, vnode **vn)
{
    int         err;
    char        *p;

    err = new_path(path, &p);
    if (err)
        goto error1;
    err = parse_path_fd(kernel, fd, &p, eatsymlink, vn);
    if (err)
        goto error2;
    free_path(p);
    return 0;
    
error2:
    free_path(p);
error1:
    return err;
}

static int
get_file_vn(nspace_id nsid, vnode_id vnid, const char *path, int eatsymlink,
        vnode **vn)
{
    int         err;
    char        *p;

    err = new_path(path, &p);
    if (err)
        goto error1;
    err = parse_path_vn(nsid, vnid, &p, eatsymlink, vn);
    if (err)
        goto error2;
    free_path(p);
    return 0;
    
error2:
    free_path(p);
error1:
    return err;
}

static int
parse_path_fd(bool kernel, int fd, char **pstart, int eatsymlink, vnode **vnp)
{
    vnode           *bvn;
    ofile           *f;
    ioctx           *io;
    char            *path;

    path = *pstart;
    if (path && (*path == '/')) {
        do
            path++;
        while (*path == '/');
        bvn = rootvn;
        inc_vnode(bvn);
    } else
        if (fd >= 0) {
            f = get_fd(kernel, fd, FD_ALL);
            if (!f)
                return EBADF;
            bvn = f->vn;
            inc_vnode(bvn);
            put_fd(f);
        } else {
            io = get_cur_ioctx();
            LOCK(io->lock);
            bvn = io->cwd;
            inc_vnode(bvn);
            UNLOCK(io->lock);
        }
    return parse_path(bvn, pstart, path, eatsymlink, vnp);
}

static int
parse_path_vn(nspace_id nsid, vnode_id vnid, char **pstart, int eatsymlink,
    vnode **vnp)
{
    int             err;
    vnode           *bvn;
    char            *path;

    path = *pstart;
    if (path && (*path == '/')) {
        do
            path++;
        while (*path == '/');
        bvn = rootvn;
        inc_vnode(bvn);
    } else {
        err = load_vnode(nsid, vnid, FALSE, &bvn);
        if (err)
            return err;
    }
    return parse_path(bvn, pstart, path, eatsymlink, vnp);

error1:
    dec_vnode(bvn, FALSE);
    return err;
}

static int
parse_path(vnode *bvn, char **pstart, char *path, int eatsymlink, vnode **vnp)
{
    int             err;
    int             iter;
    char            *p, *np, *newpath, **fred;
    vnode_id        vnid;
    vnode           *vn;

    if (!path) {
        *vnp = bvn;
        return 0;
    }

    iter = 0;
    p = path;
    vn = NULL;

    while(TRUE) {

    /*
    exit if we're done
    */

        if (*p == '\0') {
            err = 0;
            break;
        }

    /*
    isolate the next component
    */

        np = strchr(p+1, '/');
        if (np) {
            *np = '\0';
            do
                np++;
            while (*np == '/');
        } else
            np = strchr(p+1, '\0');
        
    /*
    filter '..' if at the root of a namespace
    */

        if (!strcmp(p, "..") && is_root(bvn, &vn)) {
            dec_vnode(bvn, FALSE);
            bvn = vn;
        }

    /*
    ask the file system to eat this component
    */

        newpath = NULL;
        fred = &newpath;
        if (!eatsymlink && (*np == '\0'))
            fred = NULL;

        err = (*bvn->ns->fs->ops.walk)(bvn->ns->data, bvn->data, p, fred,
                &vnid);
        p = np;
        if (!err) {
            if (newpath)
                vn = bvn;
            else {
                LOCK(vnlock);
                vn = lookup_vnode(bvn->ns->nsid, vnid);
                UNLOCK(vnlock);
                dec_vnode(bvn, FALSE);
            }
        } else {
            dec_vnode(bvn, FALSE);
            break;
        }

    /*
    deal with symbolic links
    */

        if (newpath) {

    /*
    protection against cyclic graphs (with bad symbolic links).
    */

            iter++;
            if (iter > MAX_SYM_LINKS) {
                dec_vnode(vn, FALSE);
                err = ELOOP;
                break;
            }

            p = cat_paths(newpath, np);
            if (!p) {
                dec_vnode(vn, FALSE);
                err = ENOMEM;
                break;
            }
            free_path(*pstart);
            *pstart = p;
            if (*p == '/') {
                do
                    p++;
                while (*p == '/');
                dec_vnode(vn, FALSE);
                bvn = rootvn;
                inc_vnode(bvn);
            } else
                bvn = vn;
            continue;
        }

    /*
    reached a mounting point
    */

        if (is_mount_vnode(vn, &bvn)) {
            dec_vnode(vn, FALSE);
            continue;
        }

        bvn = vn;
    }

    if (!err)
        *vnp = bvn;

    return err;
}



/*
 * get_vnode
 */

int
get_vnode(nspace_id nsid, vnode_id vnid, void **data)
{
    int         err;
    vnode       *vn;

    err = load_vnode(nsid, vnid, TRUE, &vn);
    if (err)
        return err;
    *data = vn->data;
    return 0;
}


/*
 * put_vnode
 */

int
put_vnode(nspace_id nsid, vnode_id vnid)
{
    vnode           *vn;

    LOCK(vnlock);
    vn = lookup_vnode(nsid, vnid);
    if (!vn) {
        UNLOCK(vnlock);
        return ENOENT;
    }
    UNLOCK(vnlock);
    dec_vnode(vn, TRUE);
    return 0;
}

/*
 * new_vnode
 */

int
new_vnode(nspace_id nsid, vnode_id vnid, void *data)
{
    int         err;
    vnode       *vn;

    LOCK(vnlock);
    vn = steal_vnode(FREE_LIST);
    if (!vn) {
        vn = steal_vnode(USED_LIST);
        if (!vn) {
            PANIC("OUT OF VNODE!!!\n");
            UNLOCK(vnlock);
            return ENOMEM;
        }
        flush_vnode(vn, TRUE);
    }

    vn->ns = nsidtons(nsid);
    if (!vn->ns) {
        UNLOCK(vnlock);
        return ENOENT;
    }
    vn->vnid = vnid;
    vn->data = data;
    vn->rcnt = 1;
    err = sort_vnode(vn);
    UNLOCK(vnlock);
    return err;
}

/*
 * remove_vnode
 */

int
remove_vnode(nspace_id nsid, vnode_id vnid)
{
    vnode       *vn;

    LOCK(vnlock);
    vn = lookup_vnode(nsid, vnid);
    if (!vn || (vn->rcnt == 0)) {
        UNLOCK(vnlock);
        return ENOENT;
    }
    vn->remove = TRUE;
    UNLOCK(vnlock);
    return 0;
}

/*
 * unremove_vnode
 */

int
unremove_vnode(nspace_id nsid, vnode_id vnid)
{
    vnode       *vn;

    LOCK(vnlock);
    vn = lookup_vnode(nsid, vnid);
    if (!vn || (vn->rcnt == 0)) {
        UNLOCK(vnlock);
        return ENOENT;
    }
    vn->remove = FALSE;
    UNLOCK(vnlock);
    return 0;
}

/*
 * is_vnode_removed
 */

int
is_vnode_removed(nspace_id nsid, vnode_id vnid)
{
    vnode       *vn;
    int         res;

    LOCK(vnlock);
    vn = lookup_vnode(nsid, vnid);
    if (!vn) {
        UNLOCK(vnlock);
        return ENOENT;
    }
    res = vn->remove;
    UNLOCK(vnlock);
    return res;
}

/*
 * miscelleanous vnode functions
 */


static void
inc_vnode(vnode *vn)
{
    LOCK(vnlock);
    vn->rcnt++;
    UNLOCK(vnlock);
}

static void
dec_vnode(vnode *vn, char r)
{
    vnode       *ovn;

    LOCK(vnlock);
    vn->rcnt--;
    if (vn->rcnt == 0)
        if (vn->remove) {
            vn->busy = TRUE;
            move_vnode(vn, LOCKED_LIST);
            UNLOCK(vnlock);
            (*vn->ns->fs->ops.remove_vnode)(vn->ns->data, vn->data, r);
            LOCK(vnlock);
            clear_vnode(vn);
            move_vnode(vn, FREE_LIST);
        } else {
            move_vnode(vn, USED_LIST);
            if (lists[USED_LIST].num > usdvnnum) {
                ovn = steal_vnode(USED_LIST);
                flush_vnode(ovn, r);
                move_vnode(ovn, FREE_LIST);
            }
        }
    UNLOCK(vnlock);

    return;
}

static void
clear_vnode(vnode *vn)
{
    DeleteSL(skiplist, vn);

    if (vn->nspace.prev)
        vn->nspace.prev->nspace.next = vn->nspace.next;
    else
        vn->ns->vnodes.head = vn->nspace.next;
    if (vn->nspace.next)
        vn->nspace.next->nspace.prev = vn->nspace.prev;
    else
        vn->ns->vnodes.tail = vn->nspace.prev;
    vn->nspace.next = vn->nspace.prev = NULL;

    vn->vnid = invalid_vnid;
    vn->ns = NULL;
    vn->remove = FALSE;
    vn->data = NULL;
    vn->rcnt = 0;
    vn->busy = FALSE;
    vn->mounted = NULL;
}

static int
sort_vnode(vnode *vn)
{
    if (!InsertSL(skiplist, vn))
        return ENOMEM;

    vn->nspace.next = vn->ns->vnodes.head;
    vn->nspace.prev = NULL;
    if (vn->ns->vnodes.head)
        vn->ns->vnodes.head->nspace.prev = vn;
    else
        vn->ns->vnodes.tail = vn;
    vn->ns->vnodes.head = vn;
    return 0;
}

static void
move_vnode(vnode *vn, int list)
{
    if (vn->list.prev)
        vn->list.prev->list.next = vn->list.next;
    else
        lists[vn->inlist].head = vn->list.next;
    if (vn->list.next)
        vn->list.next->list.prev = vn->list.prev;
    else
        lists[vn->inlist].tail = vn->list.prev;
    lists[vn->inlist].num--;
    vn->inlist = list;
    vn->list.next = NULL;
    vn->list.prev = lists[list].tail;
    if (vn->list.prev)
        vn->list.prev->list.next = vn;
    else
        lists[list].head = vn;
    lists[list].tail = vn;
    lists[list].num++;
}

static vnode *
steal_vnode(int list)
{
    vnode       *vn;

    vn = lists[list].head;
    if (!vn)
        return NULL;
    move_vnode(vn, LOCKED_LIST);
    return vn;
}

static void
flush_vnode(vnode *vn, char r)
{
    int     err;

    vn->busy = TRUE;
    UNLOCK(vnlock);
    err = (*vn->ns->fs->ops.release_vnode)(vn->ns->data, vn->data, r);
    if (err)
        PANIC("ERROR WRITING VNODE!!!\n");
    LOCK(vnlock);
    vn->busy = FALSE;
    clear_vnode(vn);
}

static vnode *
lookup_vnode(nspace_id nsid, vnode_id vnid)
{
    vnode       fakevn;
    nspace      fakens;

    fakens.nsid = nsid;
    fakevn.ns = &fakens;
    fakevn.vnid = vnid;
    return SearchSL(skiplist, &fakevn);
}

static int
load_vnode(nspace_id nsid, vnode_id vnid, char r, vnode **vnp)
{
    int             err;
    vnode           *vn;

    LOCK(vnlock);
    while (TRUE) {
        vn = lookup_vnode(nsid, vnid);
        if (vn)
            if (vn->busy) {
                UNLOCK(vnlock);
                snooze(SLEEP_TIME);
                LOCK(vnlock);
                continue;
            } else
                break;

        vn = steal_vnode(FREE_LIST);
        if (!vn) {
            vn = steal_vnode(USED_LIST);
            if (!vn)
                PANIC("OUT OF VNODE!!!\n");
        } else
            break;

        flush_vnode(vn, r);
        move_vnode(vn, FREE_LIST);
    }

    if (vn->ns == NULL) {
        vn->ns = nsidtons(nsid);
        if (!vn->ns) {
            err = ENOENT;
            goto error1;
        }
        vn->vnid = vnid;
        vn->busy = TRUE;
        err = sort_vnode(vn);
        if (err)
            goto error2;
        move_vnode(vn, LOCKED_LIST);
        UNLOCK(vnlock);
        err = (*vn->ns->fs->ops.read_vnode)(vn->ns->data, vnid, r, &vn->data);
        LOCK(vnlock);
        vn->busy = FALSE;
        if (err)
            goto error2;
        vn->rcnt = 1;
    } else {
        vn->rcnt++;
        if (vn->rcnt == 1)
            move_vnode(vn, LOCKED_LIST);
    }
    *vnp = vn;
    UNLOCK(vnlock);
    return 0;

error2:
    clear_vnode(vn);
error1:
    move_vnode(vn, FREE_LIST);
    UNLOCK(vnlock);
    return err;
}

static int
compare_vnode(vnode *vna, vnode *vnb)
{
    if (vna->vnid > vnb->vnid)
        return 1;
    else
        if (vna->vnid < vnb->vnid)
            return -1;
        else
            if (vna->ns->nsid > vnb->ns->nsid)
                return 1;
            else
                if (vna->ns->nsid < vnb->ns->nsid)
                    return -1;
                else
                    return 0;
}

/*
 * path management functions
 */

int
new_path(const char *path, char **copy)
{
    const char  *q, *r;
    char        *p;
    int         l, s;

    if (!path) {
        *copy = NULL;
        return 0;
    }
    l = strlen(path);
    if (l == 0)
        return ENOENT;
    s = l;
    if (path[l-1] == '/')
        s++;

    if (l >= MAXPATHLEN)
        return ENAMETOOLONG;

    q = path;
    while(*q != '\0') {
        while (*q == '/')
            q++;
        r = q;
        while ((*q != '/') && (*q != '\0'))
            q++;
        if (q - r >= FILE_NAME_LENGTH)
            return ENAMETOOLONG;        
    }

    p = (char *) malloc(s+1);
    if (!p)
        return ENOMEM;

    /* ### do real checking: MAXPATHLEN, max file name len, buffer address... */

    strcpy(p, path);    
    if (p[l-1] == '/') {
        p[l] = '.';
        p[l+1] = '\0';
    }
    *copy = p;
    return 0;
}

void
free_path(char *p)
{
    if (p) {
        free(p);
    }
}

static char *
cat_paths(char *a, char *b)
{
    char        *p;

    p = (char *) realloc(a, strlen(a) + strlen(b) + 2);
    if (!p)
        return NULL;
    strcat(p, "/");
    strcat(p, b);
    return p;
}

/* 
 * mount point management functions
 */

static int
is_mount_vnode(vnode *mount, vnode **root)
{
    nspace      *ns;

    LOCK(vnlock);
    ns = mount->mounted;
    if (ns) {
        *root = ns->root;
        ns->root->rcnt++;
    }
    UNLOCK(vnlock);
    return (ns != NULL);        
}

static int
is_mount_vnid(nspace_id nsid, vnode_id vnid, vnode_id *mount)
{
    nspace      *ns;

    for(ns = nshead; ns; ns = ns->next) {
        if (!ns->mount)
            continue;
        if (ns->mount->ns->nsid != nsid)
            continue;
        if (ns->mount->vnid != vnid)
            continue;
        *mount = ns->root->vnid;
        return TRUE;
    }
    return FALSE;
}

static int
is_root(vnode *root, vnode **mount)
{
    if ((root->ns->root == root) && root->ns->mount) {
        *mount = root->ns->mount;
        inc_vnode(*mount);
        return TRUE;
    } else
        return FALSE;
}

/*
 * file descriptor management functions
 */
 
static ofile *
get_fd(bool kernel, int fd, int type)
{
    ofile       *f;
    fdarray     *fds;

    f = NULL;
    if (kernel)
        fds = global_fds;
    else
        fds = get_cur_ioctx()->fds;
    LOCK(fds->lock);
    if ((fd >= 0) && (fd < fds->num) && fds->fds[fd]) {
        f = fds->fds[fd];
        if (f->type & type)
            atomic_add(&f->rcnt, 1);
        else
            f = NULL;
    }
    UNLOCK(fds->lock);
    return f;
}


static int
put_fd(ofile *f)
{
    long            cnt;

    cnt = atomic_add(&f->rcnt, -1);
    if (cnt == 1)
        invoke_free(f);
    return 0;
}

static int
new_fd(bool kernel, int nfd, ofile *f, int fd, bool coe)
{
    int         i, j, num, end;
    fdarray     *fds;
    ofile       *of;
    int         err;
    long        cnt;

    if (kernel)
        fds = global_fds;
    else
        fds = get_cur_ioctx()->fds;

    LOCK(fds->lock);

    num = fds->num;

    if (!f) {
        if ((fd < 0) || (fd >= num)) {
            err = EBADF;
            goto error1;
        }
        f = fds->fds[fd];
        if (!f) {
            err = EBADF;
            goto error1;
        }
    }

    atomic_add(&f->rcnt, 1);
    atomic_add(&f->ocnt, 1);

    if (nfd >= 0) {
        if (nfd >= num) {
            err = EBADF;
            goto error2;
        }
        of = fds->fds[nfd];
        fds->fds[nfd] = f;
        SETBIT(fds->alloc, nfd, TRUE);
        SETBIT(fds->coes, nfd, coe);

        UNLOCK(fds->lock);
        if (of) {
            cnt = atomic_add(&of->ocnt, -1);
            if (cnt == 1)
                invoke_close(of);
            cnt = atomic_add(&of->rcnt, -1);
            if (cnt == 1)
                invoke_free(of);
        }
        return nfd;
    }

    end = num & ~31;
    for(j=0; j<end; j+=32)
        if (fds->alloc[j/32] != 0xffffffff)
            for(i=j; i<j+32; i++)
                if (!GETBIT(fds->alloc, i))
                    goto found;
    for(i=end; i<num; i++)
        if (!GETBIT(fds->alloc, i))
            goto found;

    err = EMFILE;
    goto error2;

found:

    SETBIT(fds->alloc, i, 1);
    fds->fds[i] = f;
    SETBIT(fds->coes, i, coe);
    UNLOCK(fds->lock);
    return i;

error2:
    atomic_add(&f->rcnt, -1);
    atomic_add(&f->ocnt, -1);
error1:
    UNLOCK(fds->lock);
    return err;
}

static int
remove_fd(bool kernel, int fd, int type)
{
    ofile       *f;
    fdarray     *fds;
    long        cnt;
    int         err;

    f = NULL;
    if (kernel)
        fds = global_fds;
    else
        fds = get_cur_ioctx()->fds;
    LOCK(fds->lock);
    if ((fd >= 0) && (fd < fds->num) && fds->fds[fd]) {
        f = fds->fds[fd];
        if (f->type == type) {
            SETBIT(fds->alloc, fd, 0);
            fds->fds[fd] = NULL;
        } else
            f = NULL;
    }
    UNLOCK(fds->lock);
    if (f == NULL)
        return EBADF;

    err = 0;
    cnt = atomic_add(&f->ocnt, -1);
    if (cnt == 1)
        err = invoke_close(f);
    cnt = atomic_add(&f->rcnt, -1);
    if (cnt == 1)
        invoke_free(f);
    return err;
}

static int
get_coe(bool kernel, int fd, int type, bool *coe)
{
    ofile       *f;
    fdarray     *fds;

    f = NULL;
    if (kernel)
        fds = global_fds;
    else
        fds = get_cur_ioctx()->fds;
    LOCK(fds->lock);
    if ((fd >= 0) && (fd < fds->num) && fds->fds[fd]) {
        f = fds->fds[fd];
        if (f->type == type) {
            *coe = GETBIT(fds->coes, fd);
            UNLOCK(fds->lock);
            return 0;
        }
    }
    UNLOCK(fds->lock);
    return EBADF;
}

static int
set_coe(bool kernel, int fd, int type, bool coe)
{
    ofile       *f;
    fdarray     *fds;

    f = NULL;
    if (kernel)
        fds = global_fds;
    else
        fds = get_cur_ioctx()->fds;
    LOCK(fds->lock);
    if ((fd >= 0) && (fd < fds->num) && fds->fds[fd]) {
        f = fds->fds[fd];
        if (f->type == type) {
            SETBIT(fds->coes, fd, coe);
            UNLOCK(fds->lock);
            return 0;
        }
    }
    UNLOCK(fds->lock);
    return EBADF;
}

static int
get_omode(bool kernel, int fd, int type, int *omode)
{
    ofile       *f;
    fdarray     *fds;

    f = NULL;
    if (kernel)
        fds = global_fds;
    else
        fds = get_cur_ioctx()->fds;
    LOCK(fds->lock);
    if ((fd >= 0) && (fd < fds->num) && fds->fds[fd]) {
        f = fds->fds[fd];
        if (f->type == type) {
            *omode = f->omode;
            UNLOCK(fds->lock);
            return 0;
        }
    }
    UNLOCK(fds->lock);
    return EBADF;
}

static int
invoke_close(ofile *f)
{
    int     err;
    vnode   *vn;

    vn = f->vn;
    switch (f->type) {
    case FD_FILE:
        err = (*vn->ns->fs->ops.close)(vn->ns->data, vn->data, f->cookie);
        break;
    case FD_DIR:
        err = (*vn->ns->fs->ops.closedir)(vn->ns->data, vn->data, f->cookie);
        break;
    case FD_WD:
    default:
        err = 0;
        break;
    }
    return err;
}

static int
invoke_free(ofile *f)
{
    vnode           *vn;
    op_free_cookie  *op = NULL;

    vn = f->vn;
    switch(f->type) {
    case FD_FILE:
        op = vn->ns->fs->ops.free_cookie;
        break;
    case FD_DIR:
        op = vn->ns->fs->ops.free_dircookie;
        break;
    case FD_WD:
        op = NULL;
        break;
    }
    if (op)
        (*op)(vn->ns->data, vn->data, f->cookie);
    dec_vnode(vn, FALSE);

    free(f);
    return 0;
}

/*
 * other routines
 */

static nspace *
nsidtons(nspace_id nsid)
{
    nspace      *ns;

    ns = nstab[nsid % nns];
    if (!ns || (ns->nsid != nsid) || ns->shutdown)
        return NULL;
    return ns;
}

static int
alloc_wd_fd(bool kernel, vnode *vn, bool coe, int *fdp)
{
    int             err;
    ofile           *f;
    int             nfd;

    /*
    find a file descriptor
    */

    f = (ofile *) calloc(sizeof(ofile), 1);
    if (!f) {
        err = ENOMEM;
        goto error1;
    }

    f->type = FD_WD;
    f->vn = vn;
    f->rcnt = 0;
    f->ocnt = 0;

    nfd = new_fd(kernel, -1, f, -1, coe);
    if (nfd < 0) {
        err = EMFILE;
        goto error2;
    }

    *fdp = nfd;
    return 0;

error2:
    free(f);
error1:
    return err;
}



/*
 * file system operations
 */

void *
install_file_system(vnode_ops *ops, const char *name, bool fixed, image_id aid)
{
    fsystem     *fs;
    int         i;

    fs = (fsystem *) malloc(sizeof(fsystem));
    if (!fs)
        return NULL;

    memcpy(&fs->ops, ops, sizeof(vnode_ops));
    strcpy(fs->name, name);
    fs->rcnt = 1;
    fs->fixed = fixed;
    fs->aid = aid;

    for(i=0; i<nfs; i++, nxfsid++)
        if (!fstab[nxfsid % nfs]) {
            fstab[nxfsid % nfs] = fs;
            fs->fsid = nxfsid;
            nxfsid++;
            break;
        }

    if (i == nfs) {
        free(fs);
        return NULL;
    }
    return (void *)fs;
}

static fsystem *
load_file_system(const char *name)
{
    return NULL;
}

static int
unload_file_system(fsystem *fs)
{
    fstab[fs->fsid % nfs] = NULL;
    free(fs);
    return 0;
}


static fsystem *
inc_file_system(const char *name)
{
    fsystem         *fs;
    int             i;

    fs = NULL;
    LOCK(fstablock);

    for(i=0; i<nfs; i++)
        if (fstab[i] && !strcmp(fstab[i]->name, name)) {
            fs = fstab[i];
            fs->rcnt++;
            break;
        }

    if (!fs)
        fs = load_file_system(name);

    UNLOCK(fstablock);

    return fs;
}

static int
dec_file_system(fsystem *fs)
{
    LOCK(fstablock);

    fs->rcnt--;
    if (!fs->fixed && (fs->rcnt == 0))
        unload_file_system(fs);

    UNLOCK(fstablock);
    return 0;
}


static fdarray *
new_fds(int num)
{
    fdarray *fds;
    size_t      sz;

    sz = sizeof(fdarray) + (num-1) * sizeof(void *) + 2*BITSZ(num);
    fds = (fdarray *) malloc(sz);
    if (!fds)
        return NULL;
    memset(fds, 0, sz);
    fds->rcnt = 1;
    if (new_lock(&fds->lock, "fdlock") < 0) {
        free(fds);
        return NULL;
    }
    fds->num = num;
    fds->alloc = (ulong *) &fds->fds[num];
    fds->coes = &fds->alloc[BITSZ(num) / sizeof(ulong)];
    return fds;
}

static int
free_fds(fdarray *fds)
{
    long    cnt;
    int     i;
    ofile   *f;

    for(i=0; i<fds->num; i++)
        if (fds->fds[i]) {
            f = fds->fds[i];
            cnt = atomic_add(&f->ocnt, -1);
            if (cnt == 1)
                invoke_close(f);
            cnt = atomic_add(&f->rcnt, -1);
            if (cnt == 1)
                invoke_free(f);
        }
    delete_sem(fds->lock.s);
    free(fds);
    return 0;
}
