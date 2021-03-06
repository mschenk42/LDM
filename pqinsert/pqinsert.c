/*
 *   Copyright 1993, University Corporation for Atmospheric Research
 *   See ../COPYRIGHT file for copying and redistribution conditions.
 */

/* 
 * Convert files to ldm "products" and insert in local que
 */
#include <config.h>

#if defined(NO_MMAP) || !defined(HAVE_MMAP)
    #define USE_MMAP 0
#else
    #define USE_MMAP 1
#endif

#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <rpc/rpc.h>
#include <signal.h>
#if USE_MMAP
    #include <sys/mman.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include "ldm.h"
#include "pq.h"
#include "globals.h"
#include "remote.h"
#include "atofeedt.h"
#include "ldmprint.h"
#include "inetutil.h"
#include "log.h"
#include "md5.h"

#ifdef NO_ATEXIT
#include "atexit.h"
#endif

        /* N.B.: assumes hostname doesn't change during program execution :-) */
static char             myname[HOSTNAMESIZE];
static feedtypet        feedtype = EXP;
#if !USE_MMAP
    static struct pqe_index pqeIndex;
#endif


static void
usage(
        const char* const   progname /*  id string */
)
{
    log_add(
"Usage: %s [options] filename ...\n"
"    Options:\n"
"    -v            Verbose, tell me about each product\n"
"    -l dest       Log to `dest`. One of: \"\" (system logging daemon), \"-\"\n"
"                  (standard error), or file `dest`. Default is \"%s\"\n"
"    -q queue      Use <queue> as product-queue. Default:\n"
"                  \"%s\"\n"
"    -s seqno      Set initial product sequence number to <seqno>.\n"
"                  Default: 0\n"
"    -f feedtype   Assert your feed type as <feedtype>. Default: \"EXP\"\n"
"    -p productID  Assert product-ID as <productID>. Default is the \n"
"                  filename. With multiple files, product-ID becomes\n"
"                  <productID>.<seqno>\n"
"    -i            Compute product signature (MD5 checksum) from product ID\n",
            progname, log_get_default_destination(), getDefaultQueuePath());
    log_flush_error();
    exit(1);
}


void
cleanup(void)
{
    if (pq) {
#if !USE_MMAP
        if (!pqeIsNone(pqeIndex))
            (void)pqe_discard(pq, pqeIndex);
#endif

        (void) pq_close(pq);
        pq = NULL;
    }

    log_fini();
}


static void
signal_handler(
        int sig
)
{
#ifdef SVR3SIGNALS
        /* 
         * Some systems reset handler to SIG_DFL upon entry to handler.
         * In that case, we reregister our handler.
         */
        (void) signal(sig, signal_handler);
#endif
    switch(sig) {
      case SIGINT :
         exit(1);
      case SIGTERM :
         done = 1;
         return;
      case SIGUSR1 :
         log_refresh();
         return;
    }
}


static void
set_sigactions(void)
{
#ifdef HAVE_SIGACTION
        struct sigaction sigact;

        sigemptyset(&sigact.sa_mask);
        sigact.sa_flags = 0;

        /* Ignore these */
        sigact.sa_handler = SIG_IGN;
        (void) sigaction(SIGALRM, &sigact, NULL);
        (void) sigaction(SIGCHLD, &sigact, NULL);

        /* Handle these */
#ifdef SA_RESTART       /* SVR4, 4.3+ BSD */
        /* usually, restart system calls */
        sigact.sa_flags |= SA_RESTART;
#endif
        sigact.sa_handler = signal_handler;
        (void) sigaction(SIGTERM, &sigact, NULL);
        (void) sigaction(SIGUSR1, &sigact, NULL);
        /* Don't restart after interrupt */
        sigact.sa_flags = 0;
#ifdef SA_INTERRUPT     /* SunOS 4.x */
        sigact.sa_flags |= SA_INTERRUPT;
#endif
        (void) sigaction(SIGINT, &sigact, NULL);
#else
        
        (void) signal(SIGUSR1, SIG_IGN);
        (void) signal(SIGALRM, SIG_IGN);
        (void) signal(SIGCHLD, SIG_IGN);

        (void) signal(SIGTERM, signal_handler);
        (void) signal(SIGINT, signal_handler);
#endif

    sigset_t sigset;
    (void)sigemptyset(&sigset);
    (void)sigaddset(&sigset, SIGALRM);
    (void)sigaddset(&sigset, SIGCHLD);
    (void)sigaddset(&sigset, SIGTERM);
    (void)sigaddset(&sigset, SIGUSR1);
    (void)sigaddset(&sigset, SIGINT);
    (void)sigprocmask(SIG_UNBLOCK, &sigset, NULL);
}


#if !USE_MMAP
static int
fd_md5(MD5_CTX *md5ctxp, int fd, off_t st_size, signaturet signature)
{
        int           nread;
        unsigned char buf[8192];

        MD5Init(md5ctxp);
        for(; st_size > 0; st_size -= nread )
        {
                nread = read(fd, buf, sizeof(buf));
                if(nread <= 0)
                {
                        log_syserr("fd_md5: read");
                        return -1;
                } /* else */
                MD5Update(md5ctxp, buf, nread);
                (void)exitIfDone(1);
        }
        MD5Final(signature, md5ctxp);
        return 0;
}
#else
static int
mm_md5(MD5_CTX *md5ctxp, void *vp, size_t sz, signaturet signature)
{
        MD5Init(md5ctxp);

        MD5Update(md5ctxp, vp, sz);

        MD5Final((unsigned char*)signature, md5ctxp);
        return 0;
}
#endif


int main(
        int ac,
        char *av[]
)
{
        const char* const progname = basename(av[0]);
        int useProductID = FALSE;
        int signatureFromId = FALSE;
        char *productID = NULL;
        int multipleFiles = FALSE;
        char identifier[KEYSIZE];
        int status;
        int seq_start = 0;
        enum ExitCode {
            exit_success = 0,   /* all files inserted successfully */
            exit_system = 1,    /* operating-system failure */
            exit_pq_open = 2,   /* couldn't open product-queue */
            exit_infile = 3,    /* couldn't process input file */
            exit_dup = 4,       /* input-file already in product-queue */
            exit_md5 = 6        /* couldn't initialize MD5 processing */
        } exitCode = exit_success;

        (void)log_init(progname);

#if !USE_MMAP
        pqeIndex = PQE_NONE;
#endif

        {
            extern int optind;
            extern int opterr;
            extern char *optarg;
            int ch;

            opterr = 0; /* Suppress getopt(3) error messages */

            while ((ch = getopt(ac, av, ":ivxl:q:f:s:p:")) != EOF)
                    switch (ch) {
                    case 'i':
                            signatureFromId = 1;
                            break;
                    case 'v':
                            if (!log_is_enabled_info)
                                (void)log_set_level(LOG_LEVEL_INFO);
                            break;
                    case 'x':
                            (void)log_set_level(LOG_LEVEL_DEBUG);
                            break;
                    case 'l':
                            (void)log_set_destination(optarg);
                            break;
                    case 'q':
                            setQueuePath(optarg);
                            break;
                    case 's':
                            seq_start = atoi(optarg);
                            break;
                    case 'f':
                            feedtype = atofeedtypet(optarg);
                            if(feedtype == NONE)
                            {
                                fprintf(stderr, "Unknown feedtype \"%s\"\n", optarg);
                                    usage(progname);
                            }
                            break;
                    case 'p':
                            useProductID = TRUE;
                            productID = optarg;
                            break;
                    case ':': {
                        log_add("Option \"-%c\" requires an operand", optopt);
                        usage(progname);
                    }
                    /* no break */
                    default:
                        log_add("Unknown option: \"%c\"", optopt);
                        usage(progname);
                        /* no break */
                    }

            ac -= optind; av += optind ;

            if(ac < 1) usage(progname);
            }

        const char* const       pqfname = getQueuePath();

        /*
         * register exit handler
         */
        if(atexit(cleanup) != 0)
        {
                log_syserr("atexit");
                exit(exit_system);
        }

        /*
         * set up signal handlers
         */
        set_sigactions();

        /*
         * who am i, anyway
         */
        (void) strncpy(myname, ghostname(), sizeof(myname));
        myname[sizeof(myname)-1] = 0;

        /*
         * open the product queue
         */
        if(status = pq_open(pqfname, PQ_DEFAULT, &pq))
        {
                if (PQ_CORRUPT == status) {
                    log_error("The product-queue \"%s\" is inconsistent\n",
                            pqfname);
                }
                else {
                    log_error("pq_open: \"%s\" failed: %s",
                            pqfname, status > 0 ? strerror(status) :
                                            "Internal error");
                }
                exit(exit_pq_open);
        }


        {
        char *filename;
        int fd;
        struct stat statb;
        product prod;
        MD5_CTX *md5ctxp = NULL;

        /*
         * Allocate an MD5 context
         */
        md5ctxp = new_MD5_CTX();
        if(md5ctxp == NULL)
        {
                log_syserr("new_md5_CTX failed");
                exit(exit_md5);
        }


        /* These members are constant over the loop. */
        prod.info.origin = myname;
        prod.info.feedtype = feedtype;

        if (ac > 1) {
          multipleFiles = TRUE;
        }

        for(prod.info.seqno = seq_start ; ac > 0 ;
                         av++, ac--, prod.info.seqno++)
        {
                filename = *av;

                fd = open(filename, O_RDONLY, 0);
                if(fd == -1)
                {
                        log_syserr("open: %s", filename);
                        exitCode = exit_infile;
                        continue;
                }

                if( fstat(fd, &statb) == -1) 
                {
                        log_syserr("fstat: %s", filename);
                        (void) close(fd);
                        exitCode = exit_infile;
                        continue;
                }

                /* Determine what to use for product identifier */
                if (useProductID) 
                  {
                    if (multipleFiles) 
                      {
                        sprintf(identifier,"%s.%d", productID, prod.info.seqno);
                        prod.info.ident = identifier;
                      }
                    else
                      prod.info.ident = productID;
                   }
                else
                    prod.info.ident = filename;
                
                prod.info.sz = statb.st_size;
                prod.data = NULL;

                /* These members, and seqno, vary over the loop. */
                status = set_timestamp(&prod.info.arrival);
                if(status != ENOERR) {
                        log_syserr("set_timestamp: %s, filename");
                        exitCode = exit_infile;
                        continue;
                }

#if USE_MMAP
                prod.data = mmap(0, prod.info.sz,
                        PROT_READ, MAP_PRIVATE, fd, 0);
                if(prod.data == MAP_FAILED)
                {
                        log_syserr("mmap: %s", filename);
                        (void) close(fd);
                        exitCode = exit_infile;
                        continue;
                }

                status = 
                    signatureFromId
                        ? mm_md5(md5ctxp, prod.info.ident,
                            strlen(prod.info.ident), prod.info.signature)
                        : mm_md5(md5ctxp, prod.data, prod.info.sz,
                            prod.info.signature);

                (void)exitIfDone(1);

                if (status != 0) {
                    log_syserr("mm_md5: %s", filename);
                    (void) munmap(prod.data, prod.info.sz);
                    (void) close(fd);
                    exitCode = exit_infile;
                    continue;
                }

                /* These members, and seqno, vary over the loop. */
                status = set_timestamp(&prod.info.arrival);
                if(status != ENOERR) {
                        log_syserr("set_timestamp: %s, filename");
                        exitCode = exit_infile;
                        continue;
                }

                /*
                 * Do the deed
                 */
                status = pq_insert(pq, &prod);

                switch (status) {
                case ENOERR:
                    /* no error */
                    if(log_is_enabled_info)
                        log_info("%s", s_prod_info(NULL, 0, &prod.info,
                            log_is_enabled_debug)) ;
                    break;
                case PQUEUE_DUP:
                    log_error("Product already in queue: %s",
                        s_prod_info(NULL, 0, &prod.info, 1));
                    exitCode = exit_dup;
                    break;
                case PQUEUE_BIG:
                    log_error("Product too big for queue: %s",
                        s_prod_info(NULL, 0, &prod.info, 1));
                    exitCode = exit_infile;
                    break;
                case ENOMEM:
                    log_error("queue full?");
                    exitCode = exit_system;
                    break;  
                case EINTR:
#if defined(EDEADLOCK) && EDEADLOCK != EDEADLK
                case EDEADLOCK:
                    /*FALLTHROUGH*/
#endif
                case EDEADLK:
                    /* TODO: retry ? */
                    /*FALLTHROUGH*/
                default:
                    log_error("pq_insert: %s", status > 0
                        ? strerror(status) : "Internal error");
                    break;
                }

                (void) munmap(prod.data, prod.info.sz);
#else // USE_MMAP above; !USE_MMAP below
                status = 
                    signatureFromId
                        ? mm_md5(md5ctxp, prod.info.ident,
                            strlen(prod.info.ident), prod.info.signature)
                        : fd_md5(md5ctxp, fd, statb.st_size,
                            prod.info.signature);

                (void)exitIfDone(1);

                if (status != 0) {
                        log_syserr("xx_md5: %s", filename);
                        (void) close(fd);
                        exitCode = exit_infile;
                        continue;
                }

                if(lseek(fd, 0, SEEK_SET) == (off_t)-1)
                {
                        log_syserr("rewind: %s", filename);
                        (void) close(fd);
                        exitCode = exit_infile;
                        continue;
                }

                pqeIndex = PQE_NONE;
                status = pqe_new(pq, &prod.info, &prod.data, &pqeIndex);

                if(status != ENOERR) {
                    log_syserr("pqe_new: %s", filename);
                    exitCode = exit_infile;
                }
                else {
                    ssize_t     nread = read(fd, prod.data, prod.info.sz);

                    (void)exitIfDone(1);

                    if (nread != prod.info.sz) {
                        log_syserr("read %s %u", filename, prod.info.sz);
                        status = EIO;
                    }
                    else {
                        status = pqe_insert(pq, pqeIndex);
                        pqeIndex = PQE_NONE;

                        switch (status) {
                        case ENOERR:
                            /* no error */
                            if(ulogIsVerbose())
                                log_info("%s", s_prod_info(NULL, 0, &prod.info,
                                    log_is_enabled_debug)) ;
                            break;
                        case PQUEUE_DUP:
                            log_error("Product already in queue: %s",
                                s_prod_info(NULL, 0, &prod.info, 1));
                            exitCode = exit_dup;
                            break;
                        case ENOMEM:
                            log_error("queue full?");
                            break;  
                        case EINTR:
#if defined(EDEADLOCK) && EDEADLOCK != EDEADLK
                        case EDEADLOCK:
                            /*FALLTHROUGH*/
#endif
                        case EDEADLK:
                            /* TODO: retry ? */
                            /*FALLTHROUGH*/
                        default:
                            log_error("pq_insert: %s", status > 0
                                ? strerror(status) : "Internal error");
                        }
                    }                   /* data read into `pqeIndex` region */

                    if (status != ENOERR) {
                        (void)pqe_discard(pq, pqeIndex);
                        pqeIndex = PQE_NONE;
                    }
                }                       /* `pqeIndex` region allocated */

#endif
                (void) close(fd);
        }                               /* input-file loop */

        free_MD5_CTX(md5ctxp);  
        }                               /* code block */

        exit(exitCode);
}
