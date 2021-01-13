/* Copyright (c) 1997-1999 Miller Puckette.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* Pd side of the Pd/Pd-gui interface.  Also, some system interface routines
that didn't really belong anywhere. */

#define printf __real_printf
#include "m_pd.h"
#include "s_stuff.h"
#include "m_imp.h"
#include "g_canvas.h"   /* for GUI queueing stuff */
#include "s_net.h"
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif
#ifdef HAVE_BSTRING_H
#include <bstring.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#endif

#include <stdarg.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/stat.h>
#include <glob.h>
#else
#include <stdlib.h>
#endif

#define stringify(s) str(s)
#define str(s) #s

#define DEBUG_MESSUP 1      /* messages up from pd to pd-gui */
#define DEBUG_MESSDOWN 2    /* messages down from pd-gui to pd */

#ifndef PDBINDIR
#define PDBINDIR "bin/"
#endif

#ifndef PDGUIDIR
#define PDGUIDIR "tcl/"
#endif

#ifndef WISH
# if defined _WIN32
#  define WISH "wish85.exe"
# elif defined __APPLE__
   // leave undefined to use dummy search path, otherwise
   // this should be a full path to wish on mac
#else
#  define WISH "wish"
# endif
#endif

#define LOCALHOST "localhost"

#if PDTHREADS
#include "pthread.h"
#endif

#define THREADED_GUI_IO

#ifdef THREADED_IO
#include "ringbuffer.h"
#endif // THREADED_IO

typedef struct _fdpoll
{
    int fdp_fd;
    t_fdpollfn fdp_fn;
    void *fdp_ptr;
#ifdef THREADED_IO
    int fdp_audio_thread;
    int fdp_data_available;
    t_rbskt* fdp_rbskt;
#endif // THREADED_IO
} t_fdpoll;

#define INBUFSIZE 4096
#ifdef THREADED_IO
#define RB_SIZE (8*INBUFSIZE)
void rb_dosend(ring_buffer*);
struct _rbskt { // t_rbskt
    ring_buffer* rs_rb;
    int rs_preserve_boundaries;
    int rs_errno; /* optional, used by TCP sockets to report errors */
};

t_rbskt* rbskt_new(int preserve_boundaries) {
    t_rbskt* x = (t_rbskt*)getbytes(sizeof(*x));
    x->rs_rb = rb_create(RB_SIZE);
    x->rs_preserve_boundaries = preserve_boundaries;
    x->rs_errno = 0;
    return x;
}

void rbskt_free(t_rbskt* x) {
    if(x) {
        rb_free(x->rs_rb);
        freebytes(x, sizeof(*x));
    }
}
#endif // THREADED_IO

struct _socketreceiver
{
    char *sr_inbuf;
    int sr_inhead;
    int sr_intail;
    void *sr_owner;
    int sr_udp;
    struct sockaddr_storage *sr_fromaddr; /* optional */
    t_socketnotifier sr_notifier;
    t_socketreceivefn sr_socketreceivefn;
    t_socketfromaddrfn sr_fromaddrfn; /* optional */
#ifdef THREADED_IO
    t_rbskt* sr_rbskt;
#endif // THREADED_IO
};

typedef struct _guiqueue
{
    void *gq_client;
    t_glist *gq_glist;
    t_guicallbackfn gq_fn;
    struct _guiqueue *gq_next;
} t_guiqueue;

struct _instanceinter
{
    int i_havegui;
    int i_nfdpoll;
    t_fdpoll *i_fdpoll;
    int i_maxfd;
    int i_guisock;
    t_socketreceiver *i_socketreceiver;
    t_guiqueue *i_guiqueuehead;
    t_binbuf *i_inbinbuf;
    char *i_guibuf;
    int i_guihead;
    int i_guitail;
    int i_guisize;
    int i_waitingforping;
    int i_bytessincelastping;
    int i_fdschanged;   /* flag to break fdpoll loop if fd list changes */
#ifdef THREADED_IO
    ring_buffer* i_rbsend;
    pthread_t i_iothread;
    int i_dontmanageio;
#ifdef THREADED_GUI_IO
    ring_buffer* i_guibuf_rb;
#endif // THREADED_GUI_IO
#endif // THREADED_IO

#ifdef _WIN32
    LARGE_INTEGER i_inittime;
    double i_freq;
#endif
#if PDTHREADS
    pthread_mutex_t i_mutex;
#endif
};

extern int sys_guisetportnumber;
extern int sys_addhist(int phase);
void sys_set_searchpath(void);
void sys_set_temppath(void);
void sys_set_extrapath(void);
void sys_set_startup(void);
void sys_stopgui(void);

/* ----------- functions for timing, signals, priorities, etc  --------- */

#ifdef _WIN32

static void sys_initntclock(void)
{
    LARGE_INTEGER f1;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!QueryPerformanceFrequency(&f1))
    {
          fprintf(stderr, "pd: QueryPerformanceFrequency failed\n");
          f1.QuadPart = 1;
    }
    pd_this->pd_inter->i_freq = f1.QuadPart;
    pd_this->pd_inter->i_inittime = now;
}

#if 0
    /* this is a version you can call if you did the QueryPerformanceCounter
    call yourself.  Necessary for time tagging incoming MIDI at interrupt
    level, for instance; but we're not doing that just now. */

double nt_tixtotime(LARGE_INTEGER *dumbass)
{
    if (pd_this->pd_inter->i_freq == 0) sys_initntclock();
    return (((double)(dumbass->QuadPart -
        pd_this->pd_inter->i_inittime.QuadPart)) / pd_this->pd_inter->i_freq);
}
#endif
#endif /* _WIN32 */

    /* get "real time" in seconds; take the
    first time we get called as a reference time of zero. */
double sys_getrealtime(void)
{
#ifndef _WIN32
    static struct timeval then;
    struct timeval now;
    gettimeofday(&now, 0);
    if (then.tv_sec == 0 && then.tv_usec == 0) then = now;
    return ((now.tv_sec - then.tv_sec) +
        (1./1000000.) * (now.tv_usec - then.tv_usec));
#else
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (pd_this->pd_inter->i_freq == 0) sys_initntclock();
    return (((double)(now.QuadPart -
        pd_this->pd_inter->i_inittime.QuadPart)) / pd_this->pd_inter->i_freq);
#endif
}

extern int sys_nosleep;

#ifdef THREADED_IO
EXTERN_STRUCT _netsend;
#define t_netsend struct _netsend
void netsend_readbin(t_netsend *x, ring_buffer* rb, int fd);
#endif // THREADED_IO

static void poll_fds()
{
    const unsigned int maxRecv = INBUFSIZE; // this is the max limit in calls to recv/recvfrom in x_net.c
    char* buf;
    ssize_t ret;
    struct timeval timout;
    int i;
    int pollem = 1;
    t_fdpoll *fp;
    timout.tv_sec = 0;
    timout.tv_usec = 0;
    buf = (char*)malloc(maxRecv);
    if (pollem && pd_this->pd_inter->i_nfdpoll)
    {
        fd_set readset, writeset, exceptset;
        FD_ZERO(&writeset);
        FD_ZERO(&readset);
        FD_ZERO(&exceptset);
        for (fp = pd_this->pd_inter->i_fdpoll,
            i = pd_this->pd_inter->i_nfdpoll; i--; fp++)
                FD_SET(fp->fdp_fd, &readset);
        if(select(pd_this->pd_inter->i_maxfd+1,
                  &readset, &writeset, &exceptset, &timout) < 0)
          perror("microsleep select");
        pd_this->pd_inter->i_fdschanged = 0;
        for (i = 0; i < pd_this->pd_inter->i_nfdpoll &&
            !pd_this->pd_inter->i_fdschanged; i++)
                if (FD_ISSET(pd_this->pd_inter->i_fdpoll[i].fdp_fd, &readset))
        {
            fp = pd_this->pd_inter->i_fdpoll + i;
            if(fp->fdp_audio_thread)
            {
                fp->fdp_data_available = 1;
                break; // TODO: use atomic flag instead
            }
            else
            {
                t_fdpoll* sys_fdpoll = pd_this->pd_inter->i_fdpoll;
                int fd = sys_fdpoll[i].fdp_fd;
                t_rbskt* rbskt = sys_fdpoll[i].fdp_rbskt;
                ring_buffer* rb = rbskt->rs_rb;
                unsigned int size = rb_available_to_write(rb);
                size = size > maxRecv ? maxRecv : size;
                // TODO: handle case where there is not enough space available in
                // the buffer. What is causing it? Too much data available or too
                // much data left in the rb?

                // we adapted socketreceiver_read and netsend_readbin to use
                // rb_recv instead of recv. So here we are only reading from the
                // socket and making the data available through rb_recv
                ret = recv(fd, buf, size, 0);
                if(ret < 0)
                {
                    size = 0;
                    ret = -errno;
                } else {
                    size = ret;
                }
                //__real_printf("Writing %d to %p\n", ret, rb);
                // store the received data in the ringbuffer
                if(rbskt->rs_preserve_boundaries)
                    ret = rb_write_to_buffer(rb, 2, &ret, sizeof(ret), buf, size);
                else {
                    ret = rb_write_to_buffer(rb, 1, buf, size);
                    if(ret < 0)
                        rbskt->rs_errno = -ret;
                    else
                        rbskt->rs_errno = 0;
                }
                fp->fdp_data_available = 1;
            }
        }
    }
    free(buf);
}

#ifdef THREADED_IO
ssize_t rb_send(ring_buffer* rb, int socket, const void *buffer, size_t length, int flags);
static pthread_mutex_t sys_mutexio = PTHREAD_MUTEX_INITIALIZER;
void sys_lockio()
{
    ret = pthread_mutex_lock(&sys_mutexio);
    if(ret)
        printf("lockio: %d\n", ret);
}
void sys_unlockio()
{
    int ret = pthread_mutex_unlock(&sys_mutexio);
    if(ret)
        printf("unlockio: %d\n", ret);
}
#endif // THREADED_IO

void sys_doio(t_pdinstance* pd_that)
{
#ifdef PDINSTANCE
    t_pdinstance* pd_bak = pd_this;
    pd_this = pd_that;
#endif // PDINSTANCE
#ifdef THREADED_IO
    sys_lockio();
#endif // THREADED_IO
    poll_fds();
#ifdef THREADED_IO
    sys_unlockio();
    rb_dosend(pd_this->pd_inter->i_rbsend);
#ifdef THREADED_GUI_IO
    rb_dosend(pd_this->pd_inter->i_guibuf_rb);
#endif // THREADED_GUI_IO
#endif // THREADED_IO
#ifdef PDINSTANCE
    pd_this = pd_bak;
#endif // PDINSTANCE
}

/* sleep (but cancel the sleeping if pollem is set and any file descriptors are
ready - in that case, dispatch any resulting Pd messages and return.  Called
with sys_lock() set.  We will temporarily release the lock if we actually
sleep. */
int sys_domicrosleep(int microsec, int pollem)
{
    int i, didsomething = 0;
    if (pollem)
    {
        if(!pd_this->pd_inter->i_dontmanageio)
        {
            sys_doio(pd_this);
        }
        for (i = 0; i < pd_this->pd_inter->i_nfdpoll; i++)
        {
            // TODO: use atomics for data_available
            int* data_available =
                &(pd_this->pd_inter->i_fdpoll[i].fdp_data_available);
            if(*data_available)
            {
                (*pd_this->pd_inter->i_fdpoll[i].fdp_fn)
                    (pd_this->pd_inter->i_fdpoll[i].fdp_ptr,
                        pd_this->pd_inter->i_fdpoll[i].fdp_fd);
                *data_available = 0;
                didsomething = 1;
            }
        }
        if (didsomething)
            return (1);
    }
    if (microsec)
    {
        sys_unlock();
#ifdef _WIN32
        Sleep(microsec/1000);
#else
        usleep(microsec);
#endif
        sys_lock();
    }
    return (0);
}

    /* sleep (but if any incoming or to-gui sending to do, do that instead.)
    Call with the PD unstance lock UNSET - we set it here. */
void sys_microsleep(int microsec)
{
    sys_lock();
    sys_domicrosleep(microsec, 1);
    sys_unlock();
}

#if !defined(_WIN32) && !defined(__CYGWIN__)
static void sys_signal(int signo, sig_t sigfun)
{
    struct sigaction action;
    action.sa_flags = 0;
    action.sa_handler = sigfun;
    memset(&action.sa_mask, 0, sizeof(action.sa_mask));
#if 0  /* GG says: don't use that */
    action.sa_restorer = 0;
#endif
    if (sigaction(signo, &action, 0) < 0)
        perror("sigaction");
}

static void sys_exithandler(int n)
{
    static int trouble = 0;
    if (!trouble)
    {
        trouble = 1;
        fprintf(stderr, "Pd: signal %d\n", n);
        sys_bail(1);
    }
    else _exit(1);
}

static void sys_alarmhandler(int n)
{
    fprintf(stderr, "Pd: system call timed out\n");
}

static void sys_huphandler(int n)
{
    struct timeval timout;
    timout.tv_sec = 0;
    timout.tv_usec = 30000;
    select(1, 0, 0, 0, &timout);
}

void sys_setalarm(int microsec)
{
    struct itimerval gonzo;
    int sec = (int)(microsec/1000000);
    microsec %= 1000000;
#if 0
    fprintf(stderr, "timer %d:%d\n", sec, microsec);
#endif
    gonzo.it_interval.tv_sec = 0;
    gonzo.it_interval.tv_usec = 0;
    gonzo.it_value.tv_sec = sec;
    gonzo.it_value.tv_usec = microsec;
    if (microsec)
        sys_signal(SIGALRM, sys_alarmhandler);
    else sys_signal(SIGALRM, SIG_IGN);
    setitimer(ITIMER_REAL, &gonzo, 0);
}

#endif /* NOT _WIN32 && NOT __CYGWIN__ */

    /* on startup, set various signal handlers */
void sys_setsignalhandlers(void)
{
#if !defined(_WIN32) && !defined(__CYGWIN__)
    signal(SIGHUP, sys_huphandler);
    signal(SIGINT, sys_exithandler);
    signal(SIGQUIT, sys_exithandler);
    signal(SIGILL, sys_exithandler);
# ifdef SIGIOT
    signal(SIGIOT, sys_exithandler);
# endif
    signal(SIGFPE, SIG_IGN);
    /* signal(SIGILL, sys_exithandler);
    signal(SIGBUS, sys_exithandler);
    signal(SIGSEGV, sys_exithandler); */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, SIG_IGN);
#if 0  /* GG says: don't use that */
    signal(SIGSTKFLT, sys_exithandler);
#endif
#endif /* NOT _WIN32 && NOT __CYGWIN__ */
}

#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)

#if defined(_POSIX_PRIORITY_SCHEDULING) || defined(_POSIX_MEMLOCK)
#include <sched.h>
#endif

#define MODE_NRT 0
#define MODE_RT 1
#define MODE_WATCHDOG 2

void sys_set_priority(int mode)
{
#ifdef _POSIX_PRIORITY_SCHEDULING
    struct sched_param par;
    int p1, p2, p3;
    p1 = sched_get_priority_min(SCHED_FIFO);
    p2 = sched_get_priority_max(SCHED_FIFO);
#ifdef USEAPI_JACK
    p3 = (mode == MODE_WATCHDOG ? p1 + 7 : (mode == MODE_RT ? p1 + 5 : 0));
#else
    p3 = (mode == MODE_WATCHDOG ? p2 - 5 : (mode == MODE_RT ? p2 - 7 : 0));
#endif
    par.sched_priority = p3;
    if (sched_setscheduler(0,
        (mode == MODE_NRT ? SCHED_OTHER : SCHED_FIFO), &par) < 0)
    {
        if (mode == MODE_WATCHDOG)
            fprintf(stderr, "priority %d scheduling failed.\n", p3);
        else post("priority %d scheduling failed; running at normal priority",
                p3);
    }
    else if (sys_verbose)
    {
        if (mode == MODE_RT)
            post("priority %d scheduling enabled.\n", p3);
        else post("running at normal (non-real-time) priority.\n");
    }
#endif

#if !defined(USEAPI_JACK)
    if (mode != MODE_NRT)
    {
            /* tb: force memlock to physical memory { */
        struct rlimit mlock_limit;
        mlock_limit.rlim_cur=0;
        mlock_limit.rlim_max=0;
        setrlimit(RLIMIT_MEMLOCK,&mlock_limit);
            /* } tb */
        if (mlockall(MCL_FUTURE) != -1 && sys_verbose)
            fprintf(stderr, "memory locking enabled.\n");
    }
    else munlockall();
#endif
}

#endif /* __linux__ */

/* ------------------ receiving incoming messages over sockets ------------- */

void sys_sockerror(const char *s)
{
    char buf[MAXPDSTRING];
    int err = socket_errno();
    socket_strerror(err, buf, sizeof(buf));
    error("%s: %s (%d)", s, buf, err);
}

void sys_dontmanageio(int status)
{
    pd_this->pd_inter->i_dontmanageio = status;
}

static void* poll_thread_loop(void* arg)
{
    printf("Running polling thread\n");
    t_pdinstance* pd_that = (t_pdinstance*)arg;
    while(1)
    {
        sys_doio(pd_that);
        usleep(3000);
    }
}

#ifdef THREADED_IO
#include <pthread.h> // attempt at declaring pthread_setname_np
int pthread_setname_np(pthread_t thread, const char *name); // forcing it

void sys_startiothread()
{
    sys_dontmanageio(1);
    if(pd_this->pd_inter->i_iothread == 0)
    {
        pthread_create(&pd_this->pd_inter->i_iothread, NULL, poll_thread_loop, (void*)pd_this);
        pthread_setname_np(pd_this->pd_inter->i_iothread, "libpd-fdPollThread");
    }
}

int init_rbsend()
{
    if(!pd_this->pd_inter->i_rbsend)
        pd_this->pd_inter->i_rbsend = rb_create(8 * RB_SIZE);
    if(!pd_this->pd_inter->i_rbsend)
    {
        perror("unable to create ring buffer for outgoing network packets");
        return -1;
    }
    return 0;
}

ssize_t rb_sendto(ring_buffer* rb, int socket, const void *buffer, size_t length, int flags, void* addr, size_t addrlen);
ssize_t sys_sendto(int sockfd, const void *buf, size_t len, int flags, void* addr, size_t addrlen)
{
    if(init_rbsend())
        return -1;
    return rb_sendto(pd_this->pd_inter->i_rbsend, sockfd, buf, len, flags, addr, addrlen);
}

void sys_addpollfnsr(int fd, t_fdpollfn fn, t_socketreceiver* sr)
{
    sys_addpollfnrb(fd, fn, sr, sr->sr_rbskt);
}

void sys_addpollfnrb(int fd, t_fdpollfn fn, void *ptr, t_rbskt* rbskt)
{
    sys_addpollfn(fd, fn, ptr);
    if(rbskt) {
        int nfd = pd_this->pd_inter->i_nfdpoll;
        t_fdpoll* fp = pd_this->pd_inter->i_fdpoll + nfd - 1;
        fp->fdp_audio_thread = 0;
        fp->fdp_rbskt = rbskt;
    } else {
        printf("rbskt not inited\n");
    }
}
#endif // THREADED_IO
void sys_addpollfn(int fd, t_fdpollfn fn, void *ptr)
{
#ifdef THREADED_IO
    sys_lockio();
#endif // THREADED_IO
    int nfd, size;
    t_fdpoll *fp;
    sys_init_fdpoll();
    nfd = pd_this->pd_inter->i_nfdpoll;
    size = nfd * sizeof(t_fdpoll);
    pd_this->pd_inter->i_fdpoll = (t_fdpoll *)t_resizebytes(
        pd_this->pd_inter->i_fdpoll, size, size + sizeof(t_fdpoll));
    fp = pd_this->pd_inter->i_fdpoll + nfd;
    fp->fdp_fd = fd;
    fp->fdp_fn = fn;
    fp->fdp_ptr = ptr;
#ifdef THREADED_IO
    fp->fdp_audio_thread = 1;
    fp->fdp_data_available = 0;
#endif // THREADED_IO
    pd_this->pd_inter->i_nfdpoll = nfd + 1;
    if (fd >= pd_this->pd_inter->i_maxfd)
        pd_this->pd_inter->i_maxfd = fd + 1;
    pd_this->pd_inter->i_fdschanged = 1;
#ifdef THREADED_IO
    sys_unlockio();
#endif // THREADED_IO
}

void sys_rmpollfn(int fd)
{
#ifdef THREADED_IO
    sys_lockio();
#endif // THREADED_IO
    int nfd = pd_this->pd_inter->i_nfdpoll;
    int i, size = nfd * sizeof(t_fdpoll);
    int found = 0;
    t_fdpoll *fp;
    pd_this->pd_inter->i_fdschanged = 1;
    for (i = nfd, fp = pd_this->pd_inter->i_fdpoll; i--; fp++)
    {
        if (fp->fdp_fd == fd)
        {
            found = 1;
#ifdef THREADED_IO
            rbskt_free(fp->fdp_rbskt);
#endif // THREADED_IO
            while (i--)
            {
                fp[0] = fp[1];
                fp++;
            }
            pd_this->pd_inter->i_fdpoll = (t_fdpoll *)t_resizebytes(
                pd_this->pd_inter->i_fdpoll, size, size - sizeof(t_fdpoll));
            pd_this->pd_inter->i_nfdpoll = nfd - 1;
            break;
        }
    }
    if(!found)
        post("warning: %d removed from poll list but not found", fd);
#ifdef THREADED_IO
    sys_unlockio();
#endif // THREADED_IO
}

t_socketreceiver *socketreceiver_new(void *owner, t_socketnotifier notifier,
    t_socketreceivefn socketreceivefn, int udp)
{
    t_socketreceiver *x = (t_socketreceiver *)getbytes(sizeof(*x));
    x->sr_inhead = x->sr_intail = 0;
    x->sr_owner = owner;
    x->sr_notifier = notifier;
    x->sr_socketreceivefn = socketreceivefn;
    x->sr_udp = udp;
    x->sr_fromaddr = NULL;
    x->sr_fromaddrfn = NULL;
#ifdef THREADED_IO
    x->sr_rbskt = rbskt_new(udp);
#endif // THREADED_IO
    if (!(x->sr_inbuf = malloc(INBUFSIZE))) bug("t_socketreceiver");
    return (x);
}

void socketreceiver_free(t_socketreceiver *x)
{
#ifdef THREADED_IO
#endif // THREADED_IO
    free(x->sr_inbuf);
    if (x->sr_fromaddr) free(x->sr_fromaddr);
    freebytes(x, sizeof(*x));
}

    /* this is in a separately called subroutine so that the buffer isn't
    sitting on the stack while the messages are getting passed. */
static int socketreceiver_doread(t_socketreceiver *x)
{
    char messbuf[INBUFSIZE], *bp = messbuf;
    int indx, first = 1;
    int inhead = x->sr_inhead;
    int intail = x->sr_intail;
    char *inbuf = x->sr_inbuf;
    for (indx = intail; first || (indx != inhead);
        first = 0, (indx = (indx+1)&(INBUFSIZE-1)))
    {
            /* if we hit a semi that isn't preceded by a \, it's a message
            boundary.  LATER we should deal with the possibility that the
            preceding \ might itself be escaped! */
        char c = *bp++ = inbuf[indx];
        if (c == ';' && (!indx || inbuf[indx-1] != '\\'))
        {
            intail = (indx+1)&(INBUFSIZE-1);
            binbuf_text(pd_this->pd_inter->i_inbinbuf, messbuf, bp - messbuf);
            if (sys_debuglevel & DEBUG_MESSDOWN)
            {
                write(2,  messbuf, bp - messbuf);
                write(2, "\n", 1);
            }
            x->sr_inhead = inhead;
            x->sr_intail = intail;
            return (1);
        }
    }
    return (0);
}

#ifdef THREADED_IO
int rb_recv(t_rbskt* rbskt, char* buf, size_t buflen, void* nothing)
{
    ring_buffer* rb = rbskt->rs_rb;
    int ret;
    ssize_t msglen;
    int available;
    available = rb_available_to_read(rb);
    if(rbskt->rs_preserve_boundaries) {
        if(available < sizeof(ssize_t))
            return 0;
        if((ret = rb_read_from_buffer(rb, (char*)&msglen, sizeof(msglen))))
        {
            perror("Error while reading from ring_buffer in rb_recv: couldn't read from rb");
            errno = EPERM;
            return -1;
        }
        // msglen contains the return value of recv(), or -errno if an error occurred
        if(msglen <= 0) {
            printf("%p Msglen: %d\n", rb, msglen);
            // to comply with recvfrom(),
            // set errno and return -1
            errno = -msglen;
            return -1;
        }
        // if recv() was successful, the ring buffer should contain at least as
        // much data as was retrieved by it.
        available -= sizeof(msglen);
    } else {
        // if an error occurred, it was passed by setting the errno field
        if(rbskt->rs_errno) {
            errno = rbskt->rs_errno;
            rbskt->rs_errno = 0; // resetting this is unsafe, as it may be modified by the other thread
            return -1;
        } else
            msglen = available;
    }
    if(available < msglen) {
        printf("%p msglen: %d available: %d\n", rb, msglen, available);
        errno = EPERM;
        perror("Error while reading from ring_buffer in rb_recv: not enough data in rb");
        return -1;
    }
    // only request as many bytes as we can store if they are available
    int actualLength = buflen < msglen ? buflen : msglen;
    ret = rb_read_from_buffer(rb, buf, actualLength);
    if(ret)
    {
        errno = EPERM;
        perror("Error while reading from ring_buffer in rb_recv");
        return -1;
    }
    if(msglen > buflen) {
        if(rbskt->rs_preserve_boundaries) {
            printf("buflen: %d, msglen: %d, actualLength: %d\n", buflen, msglen, actualLength);
            printf("warning: incoming %d packet truncated from %d to %d bytes.",
                ret, INBUFSIZE);
            // drain buffer
            char dest;
            while(1 == rb_read_from_buffer(rb, &dest, 1))
                    ;
        }
    }
    return actualLength;
}
#endif // THREADED_IO

static void socketreceiver_getudp(t_socketreceiver *x, int fd)
{
    char buf[INBUFSIZE+1];
    socklen_t fromaddrlen = sizeof(struct sockaddr_storage);
    int ret, readbytes = 0;
    while (1)
    {
#ifdef THREADED_IO
        ret = rb_recv(x->sr_rbskt, buf, INBUFSIZE, 0);
        // TODO: retrieve fromaddr
        x->sr_fromaddr = NULL;
        fromaddrlen = 0;
#else // THREADED_IO
        ret = (int)recvfrom(fd, buf, INBUFSIZE, 0,
            (struct sockaddr *)x->sr_fromaddr, (x->sr_fromaddr ? &fromaddrlen : 0));
#endif // THREADED_IO
        if (ret < 0)
        {
                /* socket_errno_udp() ignores some error codes */
            if (socket_errno_udp())
            {
                sys_sockerror("recv (udp)");
                    /* only notify and shutdown a UDP sender! */
                if (x->sr_notifier)
                {
                    (*x->sr_notifier)(x->sr_owner, fd);
                    sys_rmpollfn(fd);
                    sys_closesocket(fd);
                }
            }
            return;
        }
        else if (ret > 0)
        {
                /* handle too large UDP packets */
            if (ret > INBUFSIZE)
            {
                post("warning: incoming UDP packet truncated from %d to %d bytes.",
                    ret, INBUFSIZE);
                ret = INBUFSIZE;
            }
            buf[ret] = 0;
    #if 0
            post("%s", buf);
    #endif
            if (buf[ret-1] != '\n')
            {
    #if 0
                error("dropped bad buffer %s\n", buf);
    #endif
            }
            else
            {
                char *semi = strchr(buf, ';');
                if (semi)
                    *semi = 0;
                if (x->sr_fromaddrfn)
                    (*x->sr_fromaddrfn)(x->sr_owner, (const void *)x->sr_fromaddr);
                binbuf_text(pd_this->pd_inter->i_inbinbuf, buf, strlen(buf));
                outlet_setstacklim();
                if (x->sr_socketreceivefn)
                    (*x->sr_socketreceivefn)(x->sr_owner,
                        pd_this->pd_inter->i_inbinbuf);
                else bug("socketreceiver_getudp");
            }
            readbytes += ret;
            /* throttle */
            if (readbytes >= INBUFSIZE)
                return;
#ifdef THREADED_IO
            /* TODO: check for more data available */
            if (rb_available_to_read(x->sr_rbskt->rs_rb) <= 0)
                return;
#else // THREADED_IO
            /* check for pending UDP packets */
            if (socket_bytes_available(fd) <= 0)
                return;
#endif // THREADED_IO
        }
    }
}

void sys_exit(void);

void socketreceiver_read(t_socketreceiver *x, int fd)
{
    if (x->sr_udp)   /* UDP ("datagram") socket protocol */
        socketreceiver_getudp(x, fd);
    else  /* TCP ("streaming") socket protocol */
    {
        char *semi;
        int readto =
            (x->sr_inhead >= x->sr_intail ? INBUFSIZE : x->sr_intail-1);
        int ret;

            /* the input buffer might be full.  If so, drop the whole thing */
        if (readto == x->sr_inhead)
        {
            fprintf(stderr, "pd: dropped message from gui\n");
            x->sr_inhead = x->sr_intail = 0;
            readto = INBUFSIZE;
        }
        else
        {
#ifdef THREADED_IO
            ret = rb_recv(x->sr_rbskt, x->sr_inbuf + x->sr_inhead,
                readto - x->sr_inhead, 0);
#else // THREADED_IO
            ret = (int)recv(fd, x->sr_inbuf + x->sr_inhead,
                readto - x->sr_inhead, 0);
#endif // THREADED_IO
            if (ret <= 0)
            {
                if (ret < 0)
                    sys_sockerror("recv (tcp)");
                if (x == pd_this->pd_inter->i_socketreceiver)
                {
                    if (pd_this == &pd_maininstance)
                        sys_bail(1);
                    else
                    {
                        sys_rmpollfn(fd);
                        sys_closesocket(fd);
                        sys_stopgui();
                    }
                }
                else
                {
                    if (x->sr_notifier)
                        (*x->sr_notifier)(x->sr_owner, fd);
                    sys_rmpollfn(fd);
                    sys_closesocket(fd);
                }
            }
            else
            {
                x->sr_inhead += ret;
                if (x->sr_inhead >= INBUFSIZE) x->sr_inhead = 0;
                while (socketreceiver_doread(x))
                {
                    if (x->sr_fromaddrfn)
                    {
                        socklen_t fromaddrlen = sizeof(struct sockaddr_storage);
                        if(!getpeername(fd,
                                        (struct sockaddr *)x->sr_fromaddr,
                                        &fromaddrlen))
                            (*x->sr_fromaddrfn)(x->sr_owner,
                                (const void *)x->sr_fromaddr);
                    }
                    outlet_setstacklim();
                    if (x->sr_socketreceivefn)
                        (*x->sr_socketreceivefn)(x->sr_owner,
                            pd_this->pd_inter->i_inbinbuf);
                    else binbuf_eval(pd_this->pd_inter->i_inbinbuf, 0, 0, 0);
                    if (x->sr_inhead == x->sr_intail)
                        break;
                }
            }
        }
    }
}

void socketreceiver_set_fromaddrfn(t_socketreceiver *x,
    t_socketfromaddrfn fromaddrfn)
{
    x->sr_fromaddrfn = fromaddrfn;
    if (fromaddrfn)
    {
        if (!x->sr_fromaddr)
            x->sr_fromaddr = malloc(sizeof(struct sockaddr_storage));
    }
    else if (x->sr_fromaddr)
    {
        free(x->sr_fromaddr);
        x->sr_fromaddr = NULL;
    }
}

void sys_closesocket(int sockfd)
{
    socket_close(sockfd);
}

/* ---------------------- sending messages to the GUI ------------------ */
#define GUI_ALLOCCHUNK 8192
#define GUI_UPDATESLICE 512 /* how much we try to do in one idle period */
#define GUI_BYTESPERPING 1024 /* how much we send up per ping */

static void sys_trytogetmoreguibuf(int newsize)
{
    char *newbuf = realloc(pd_this->pd_inter->i_guibuf, newsize);
#if 0
    static int sizewas;
    if (newsize > 70000 && sizewas < 70000)
    {
        int i;
        for (i = pd_this->pd_inter->i_guitail;
            i < pd_this->pd_inter->i_guihead; i++)
                fputc(pd_this->pd_inter->i_guibuf[i], stderr);
    }
    sizewas = newsize;
#endif
#if 0
    fprintf(stderr, "new size %d (head %d, tail %d)\n",
        newsize, pd_this->pd_inter->i_guihead, pd_this->pd_inter->i_guitail);
#endif

        /* if realloc fails, make a last-ditch attempt to stay alive by
        synchronously writing out the existing contents.  LATER test
        this by intentionally setting newbuf to zero */
    if (!newbuf)
    {
        int bytestowrite = pd_this->pd_inter->i_guitail -
            pd_this->pd_inter->i_guihead;
        int written = 0;
        while (1)
        {
            int res = (int)send(pd_this->pd_inter->i_guisock,
                pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guitail +
                    written, bytestowrite, 0);
            if (res < 0)
            {
                perror("pd output pipe");
                sys_bail(1);
            }
            else
            {
                written += res;
                if (written >= bytestowrite)
                    break;
            }
        }
        pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    }
    else
    {
        pd_this->pd_inter->i_guisize = newsize;
        pd_this->pd_inter->i_guibuf = newbuf;
    }
}

int sys_havegui(void)
{
    return (pd_this->pd_inter->i_havegui);
}

void sys_vgui(const char *fmt, ...)
{
    int msglen, bytesleft, headwas, nwrote;
    va_list ap;

    if (!sys_havegui())
        return;
    if (!pd_this->pd_inter->i_guibuf)
    {
        if (!(pd_this->pd_inter->i_guibuf = malloc(GUI_ALLOCCHUNK)))
        {
            fprintf(stderr, "Pd: couldn't allocate GUI buffer\n");
            sys_bail(1);
        }
        pd_this->pd_inter->i_guisize = GUI_ALLOCCHUNK;
        pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    }
#ifdef THREADED_GUI_IO
    if (!pd_this->pd_inter->i_guibuf_rb)
        pd_this->pd_inter->i_guibuf_rb = rb_create(GUI_ALLOCCHUNK * 8);
#endif // THREADED_GUI_IO
    if (pd_this->pd_inter->i_guihead > pd_this->pd_inter->i_guisize -
        (GUI_ALLOCCHUNK/2))
            sys_trytogetmoreguibuf(pd_this->pd_inter->i_guisize +
                GUI_ALLOCCHUNK);
    va_start(ap, fmt);
    msglen = vsnprintf(pd_this->pd_inter->i_guibuf +
        pd_this->pd_inter->i_guihead,
            pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead,
                fmt, ap);
    va_end(ap);
    if(msglen < 0)
    {
        fprintf(stderr,
            "Pd: buffer space wasn't sufficient for long GUI string\n");
        return;
    }
    if (msglen >= pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead)
    {
        int msglen2, newsize = pd_this->pd_inter->i_guisize + 1 +
            (msglen > GUI_ALLOCCHUNK ? msglen : GUI_ALLOCCHUNK);
        sys_trytogetmoreguibuf(newsize);

        va_start(ap, fmt);
        msglen2 = vsnprintf(pd_this->pd_inter->i_guibuf +
            pd_this->pd_inter->i_guihead,
                pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead,
                    fmt, ap);
        va_end(ap);
        if (msglen2 != msglen)
            bug("sys_vgui");
        if (msglen >= pd_this->pd_inter->i_guisize -
            pd_this->pd_inter->i_guihead) msglen =
                pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead;
    }
    if (sys_debuglevel & DEBUG_MESSUP)
        fprintf(stderr, "%s",
            pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guihead);
    pd_this->pd_inter->i_guihead += msglen;
    pd_this->pd_inter->i_bytessincelastping += msglen;
}

void sys_gui(const char *s)
{
    sys_vgui("%s", s);
}

#ifdef THREADED_IO
// It is safe to call this from a real-time thread
ssize_t rb_sendto(ring_buffer* rb, int socket, const void *buffer, size_t length, int flags, void* addr, size_t addrlen)
{
    //printf("rb_send: fd: %d, rb: %p \"%s\"\n", socket, rb, (char*)buffer);
    size_t maxLength = rb_available_to_write(rb);
    size_t desiredLength = sizeof(size_t) + length;
    size_t actualLength = desiredLength <= maxLength ? desiredLength : maxLength;
    if(actualLength < desiredLength)
    {
        // now we are in trouble:
        // TODO: what should we do? Realloc the ringbuffer I guess
        perror("rb buffer is full, discarding message");
        return -1;
    }
    // TODO: what happens if a socket is removed while it is in the queue?
    rb_write_to_buffer(rb, 5,
            (char*)&socket, sizeof(socket),
            (char*)&length, sizeof(length),
            buffer, length,
            (char*)&addrlen, sizeof(addrlen),
            addr, addrlen
        );
    return actualLength;
}

ssize_t rb_send(ring_buffer* rb, int socket, const void *buffer, size_t length, int flags)
{
    return rb_sendto(rb, socket, buffer, length, flags, NULL, 0);
}

// This should be called from a non-realtime thread
void rb_dosend(ring_buffer* rb)
{
    int socket;
    size_t length;
    char* buf;
    size_t addrlen;
    struct sockaddr addr;
    if(rb_read_from_buffer(rb, (char*)&socket, sizeof(socket)) < 0)
    {
        // no message to retrieve
        return;
    }
    if(rb_read_from_buffer(rb, (char*)&length, sizeof(length)) < 0)
    {
        perror("we should not be here 1");
        return;
    }
    buf = (char*)malloc(length);
    if(rb_read_from_buffer(rb, buf, length) < 0)
    {
        perror("we should not be here 2");
        return;
    }
    if(rb_read_from_buffer(rb, (char*)&addrlen, sizeof(addrlen)) < 0)
    {
        perror("we should not be here 3");
        return;
    }
    if(rb_read_from_buffer(rb, (char*)&addr, addrlen) < 0)
    {
        perror("we should not be here 4");
        return;
    }
    int nwrote = sendto(socket, buf, length, 0, &addr, addrlen);

    if (nwrote < 0)
    {
        fprintf(stderr, "failed send(%d, %p, %d, 0) call: %d %s\n", socket, buf, length, errno, strerror(errno));
        fprintf(stderr, "TODO: call netsend_disconnect() or similar\n");
        // perror("pd-to-gui socket");
        // TODO: if this was the pd-gui socket, should call sys_bail(1);
        //sys_bail(1);
    }
    free(buf);
}
#endif // THREADED_IO

static int sys_flushtogui(void)
{
    int writesize = pd_this->pd_inter->i_guihead - pd_this->pd_inter->i_guitail,
        nwrote = 0;
    if (writesize > 0)
#ifdef THREADED_GUI_IO
        nwrote = (int)rb_send(pd_this->pd_inter->i_guibuf_rb, pd_this->pd_inter->i_guisock,
#else // THREADED_GUI_IO
        nwrote = (int)send(pd_this->pd_inter->i_guisock,
#endif // THREADED_GUI_IO
            pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guitail,
                writesize, 0);

#if 0
    if (writesize)
        fprintf(stderr, "wrote %d of %d\n", nwrote, writesize);
#endif

    if (nwrote < 0)
    {
        perror("pd-to-gui socket");
        sys_bail(1);
    }
    else if (!nwrote)
        return (0);
    else if (nwrote >= pd_this->pd_inter->i_guihead -
        pd_this->pd_inter->i_guitail)
            pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    else if (nwrote)
    {
        pd_this->pd_inter->i_guitail += nwrote;
        if (pd_this->pd_inter->i_guitail > (pd_this->pd_inter->i_guisize >> 2))
        {
            memmove(pd_this->pd_inter->i_guibuf,
                pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guitail,
                    pd_this->pd_inter->i_guihead -
                        pd_this->pd_inter->i_guitail);
            pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guihead -
                pd_this->pd_inter->i_guitail;
            pd_this->pd_inter->i_guitail = 0;
        }
    }
    return (1);
}

void glob_ping(t_pd *dummy)
{
    pd_this->pd_inter->i_waitingforping = 0;
}

static int sys_flushqueue(void)
{
    int wherestop = pd_this->pd_inter->i_bytessincelastping + GUI_UPDATESLICE;
    if (wherestop + (GUI_UPDATESLICE >> 1) > GUI_BYTESPERPING)
        wherestop = 0x7fffffff;
    if (pd_this->pd_inter->i_waitingforping)
        return (0);
    if (!pd_this->pd_inter->i_guiqueuehead)
        return (0);
    while (1)
    {
        if (pd_this->pd_inter->i_bytessincelastping >= GUI_BYTESPERPING)
        {
            sys_gui("pdtk_ping\n");
            pd_this->pd_inter->i_bytessincelastping = 0;
            pd_this->pd_inter->i_waitingforping = 1;
            return (1);
        }
        if (pd_this->pd_inter->i_guiqueuehead)
        {
            t_guiqueue *headwas = pd_this->pd_inter->i_guiqueuehead;
            pd_this->pd_inter->i_guiqueuehead = headwas->gq_next;
            (*headwas->gq_fn)(headwas->gq_client, headwas->gq_glist);
            t_freebytes(headwas, sizeof(*headwas));
            if (pd_this->pd_inter->i_bytessincelastping >= wherestop)
                break;
        }
        else break;
    }
    sys_flushtogui();
    return (1);
}

    /* flush output buffer and update queue to gui in small time slices */
static int sys_poll_togui(void) /* returns 1 if did anything */
{
    if (!sys_havegui())
        return (0);
        /* in case there is stuff still in the buffer, try to flush it. */
    sys_flushtogui();
        /* if the flush wasn't complete, wait. */
    if (pd_this->pd_inter->i_guihead > pd_this->pd_inter->i_guitail)
        return (0);

        /* check for queued updates */
    if (sys_flushqueue())
        return (1);

    return (0);
}

    /* if some GUI object is having to do heavy computations, it can tell
    us to back off from doing more updates by faking a big one itself. */
void sys_pretendguibytes(int n)
{
    pd_this->pd_inter->i_bytessincelastping += n;
}

void sys_queuegui(void *client, t_glist *glist, t_guicallbackfn f)
{
    t_guiqueue **gqnextptr, *gq;
    if (!pd_this->pd_inter->i_guiqueuehead)
        gqnextptr = &pd_this->pd_inter->i_guiqueuehead;
    else
    {
        for (gq = pd_this->pd_inter->i_guiqueuehead; gq->gq_next;
            gq = gq->gq_next)
                if (gq->gq_client == client)
                    return;
        if (gq->gq_client == client)
            return;
        gqnextptr = &gq->gq_next;
    }
    gq = t_getbytes(sizeof(*gq));
    gq->gq_next = 0;
    gq->gq_client = client;
    gq->gq_glist = glist;
    gq->gq_fn = f;
    gq->gq_next = 0;
    *gqnextptr = gq;
}

void sys_unqueuegui(void *client)
{
    t_guiqueue *gq, *gq2;
    while (pd_this->pd_inter->i_guiqueuehead &&
        pd_this->pd_inter->i_guiqueuehead->gq_client == client)
    {
        gq = pd_this->pd_inter->i_guiqueuehead;
        pd_this->pd_inter->i_guiqueuehead =
            pd_this->pd_inter->i_guiqueuehead->gq_next;
        t_freebytes(gq, sizeof(*gq));
    }
    if (!pd_this->pd_inter->i_guiqueuehead)
        return;
    for (gq = pd_this->pd_inter->i_guiqueuehead; (gq2 = gq->gq_next); gq = gq2)
        if (gq2->gq_client == client)
    {
        gq->gq_next = gq2->gq_next;
        t_freebytes(gq2, sizeof(*gq2));
        break;
    }
}

    /* poll for any incoming packets, or for GUI updates to send.  call with
    the PD instance lock set. */
int sys_pollgui(void)
{
    static double lasttime = 0;
    double now = 0;
    int didsomething = sys_domicrosleep(0, 1);
    if (!didsomething || (now = sys_getrealtime()) > lasttime + 0.5)
    {
        didsomething |= sys_poll_togui();
        if (now)
            lasttime = now;
    }
    return (didsomething);
}

void sys_init_fdpoll(void)
{
    if (pd_this->pd_inter->i_fdpoll)
        return;
    /* create an empty FD poll list */
    pd_this->pd_inter->i_fdpoll = (t_fdpoll *)t_getbytes(0);
    pd_this->pd_inter->i_nfdpoll = 0;
    pd_this->pd_inter->i_inbinbuf = binbuf_new();
}

/* --------------------- starting up the GUI connection ------------- */

static int sys_watchfd;

#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)
void glob_watchdog(t_pd *dummy)
{
    if (write(sys_watchfd, "\n", 1) < 1)
    {
        fprintf(stderr, "pd: watchdog process died\n");
        sys_bail(1);
    }
}
#endif

static void sys_init_deken(void)
{
    const char*os =
#if defined __linux__
        "Linux"
#elif defined __APPLE__
        "Darwin"
#elif defined __FreeBSD__
        "FreeBSD"
#elif defined __NetBSD__
        "NetBSD"
#elif defined __OpenBSD__
        "OpenBSD"
#elif defined _WIN32
        "Windows"
#else
# if defined(__GNUC__)
#  warning unknown OS
# endif
        0
#endif
        ;
    const char*machine =
#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
        "amd64"
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__) || defined(_M_IX86)
        "i386"
#elif defined(__ppc__)
        "ppc"
#elif defined(__aarch64__)
        "arm64"
#elif defined (__ARM_ARCH)
        "armv" stringify(__ARM_ARCH)
# if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#  if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        "b"
#  endif
# endif
#else
# if defined(__GNUC__)
#  warning unknown architecture
# endif
        0
#endif
        ;

        /* only send the arch info, if we are sure about it... */
    if (os && machine)
        sys_vgui("::deken::set_platform %s %s %d %d\n",
                 os, machine,
                 8 * sizeof(char*),
                 8 * sizeof(t_float));
}

static int sys_do_startgui(const char *libdir)
{
    char apibuf[256], apibuf2[256];
    struct addrinfo *ailist = NULL, *ai;
    int sockfd = -1;
    int portno = -1;
#ifndef _WIN32
    int stdinpipe[2];
    pid_t childpid;
#endif /* _WIN32 */

    sys_init_fdpoll();

    if (sys_guisetportnumber)  /* GUI exists and sent us a port number */
    {
        int status;
#ifdef __APPLE__
            /* guisock might be 1 or 2, which will have offensive results
            if somebody writes to stdout or stderr - so we just open a few
            files to try to fill fds 0 through 2.  (I tried using dup()
            instead, which would seem the logical way to do this, but couldn't
            get it to work.) */
        int burnfd1 = open("/dev/null", 0), burnfd2 = open("/dev/null", 0),
            burnfd3 = open("/dev/null", 0);
        if (burnfd1 > 2)
            close(burnfd1);
        if (burnfd2 > 2)
            close(burnfd2);
        if (burnfd3 > 2)
            close(burnfd3);
#endif

        /* get addrinfo list using hostname & port */
        status = addrinfo_get_list(&ailist,
            LOCALHOST, sys_guisetportnumber, SOCK_STREAM);
        if (status != 0)
        {
            fprintf(stderr,
                "localhost not found (inet protocol not installed?)\n%s (%d)",
                gai_strerror(status), status);
            return (1);
        }

        /* Sort to IPv4 for now as the Pd gui uses IPv4. */
        addrinfo_sort_list(&ailist, addrinfo_ipv4_first);

        /* We don't know in advance whether the GUI uses IPv4 or IPv6,
           so we try both and pick the one which works. */
        for (ai = ailist; ai != NULL; ai = ai->ai_next)
        {
            /* create a socket */
            sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sockfd < 0)
                continue;
        #if 1
            if (socket_set_boolopt(sockfd, IPPROTO_TCP, TCP_NODELAY, 1) < 0)
                fprintf(stderr, "setsockopt (TCP_NODELAY) failed");
        #endif
            /* try to connect */
            if (socket_connect(sockfd, ai->ai_addr, ai->ai_addrlen, 10.f) < 0)
            {
                sys_closesocket(sockfd);
                sockfd = -1;
                continue;
            }
            /* this addr worked */
            break;
        }
        freeaddrinfo(ailist);

        /* confirm that we could connect */
        if (sockfd < 0)
        {
            sys_sockerror("connecting stream socket");
            return (1);
        }

        pd_this->pd_inter->i_guisock = sockfd;
    }
    else    /* default behavior: start up the GUI ourselves. */
    {
        struct sockaddr_storage addr;
        int status;
#ifdef _WIN32
        char scriptbuf[MAXPDSTRING+30], wishbuf[MAXPDSTRING+30], portbuf[80];
        int spawnret;
#else
        char cmdbuf[4*MAXPDSTRING], *guicmd;
#endif
        /* get addrinfo list using hostname (get random port from OS) */
        status = addrinfo_get_list(&ailist, LOCALHOST, 0, SOCK_STREAM);
        if (status != 0)
        {
            fprintf(stderr,
                "localhost not found (inet protocol not installed?)\n%s (%d)",
                gai_strerror(status), status);
            return (1);
        }
        /* we prefer the IPv4 addresses because the GUI might not be IPv6 capable. */
        addrinfo_sort_list(&ailist, addrinfo_ipv4_first);
        /* try each addr until we find one that works */
        for (ai = ailist; ai != NULL; ai = ai->ai_next)
        {
            sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sockfd < 0)
                continue;
        #if 1
            /* ask OS to allow another process to reopen this port after we close it */
            if (socket_set_boolopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 1) < 0)
                fprintf(stderr, "setsockopt (SO_REUSEADDR) failed\n");
        #endif
        #if 1
            /* stream (TCP) sockets are set NODELAY */
            if (socket_set_boolopt(sockfd, IPPROTO_TCP, TCP_NODELAY, 1) < 0)
                fprintf(stderr, "setsockopt (TCP_NODELAY) failed");
        #endif
            /* name the socket */
            if (bind(sockfd, ai->ai_addr, ai->ai_addrlen) < 0)
            {
                socket_close(sockfd);
                sockfd = -1;
                continue;
            }
            /* this addr worked */
            memcpy(&addr, ai->ai_addr, ai->ai_addrlen);
            break;
        }
        freeaddrinfo(ailist);

        /* confirm that socket/bind worked */
        if (sockfd < 0)
        {
            sys_sockerror("bind");
            return (1);
        }
        /* get the actual port number */
        portno = socket_get_port(sockfd);
        if (sys_verbose) fprintf(stderr, "port %d\n", portno);

#ifndef _WIN32
        if (sys_guicmd)
            guicmd = sys_guicmd;
        else
        {
#ifdef __APPLE__
            int i;
            struct stat statbuf;
            glob_t glob_buffer;
            char *homedir = getenv("HOME");
            char embed_glob[FILENAME_MAX];
            char home_filename[FILENAME_MAX];
            char *wish_paths[11] = {
 "(custom wish not defined)",
 "(did not find a home directory)",
 "/Applications/Utilities/Wish.app/Contents/MacOS/Wish",
 "/Applications/Utilities/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/Applications/Wish.app/Contents/MacOS/Wish",
 "/Applications/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/Library/Frameworks/Tk.framework/Resources/Wish.app/Contents/MacOS/Wish",
 "/Library/Frameworks/Tk.framework/Resources/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/System/Library/Frameworks/Tk.framework/Resources/Wish.app/Contents/MacOS/Wish",
 "/System/Library/Frameworks/Tk.framework/Resources/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/usr/bin/wish"
            };
            /* this glob is needed so the Wish executable can have the same
             * filename as the Pd.app, i.e. 'Pd-0.42-3.app' should have a Wish
             * executable called 'Pd-0.42-3.app/Contents/MacOS/Pd-0.42-3' */
            sprintf(embed_glob, "%s/../MacOS/Pd*", libdir);
            glob_buffer.gl_matchc = 1; /* we only need one match */
            glob(embed_glob, GLOB_LIMIT, NULL, &glob_buffer);
            /* If we are using a copy of Wish embedded in the Pd.app, then it
             * will automatically load pd-gui.tcl if that embedded Wish can
             * find ../Resources/Scripts/AppMain.tcl, then Wish doesn't want
             * to receive the pd-gui.tcl as an argument.  Otherwise it needs
             * to know how to find pd-gui.tcl */
            if (glob_buffer.gl_pathc > 0)
                sprintf(cmdbuf, "\"%s\" %d\n", glob_buffer.gl_pathv[0], portno);
            else
            {
                int wish_paths_count = sizeof(wish_paths)/sizeof(*wish_paths);
                #ifdef WISH
                    wish_paths[0] = WISH;
                #endif
                sprintf(home_filename,
                        "%s/Applications/Wish.app/Contents/MacOS/Wish",homedir);
                wish_paths[1] = home_filename;
                for(i=0; i<wish_paths_count; i++)
                {
                    if (sys_verbose)
                        fprintf(stderr, "Trying Wish at \"%s\"\n",
                            wish_paths[i]);
                    if (stat(wish_paths[i], &statbuf) >= 0)
                        break;
                }
                if(i>=wish_paths_count)
                {
                    fprintf(stderr, "sys_startgui couldn't find tcl/tk\n");
                    sys_closesocket(sockfd);
                    return (1);
                }
                sprintf(cmdbuf, "\"%s\" \"%s/%spd-gui.tcl\" %d\n",
                        wish_paths[i], libdir, PDGUIDIR, portno);
            }
#else /* __APPLE__ */
            /* sprintf the wish command with needed environment variables.
            For some reason the wish script fails if HOME isn't defined so
            if necessary we put that in here too. */
            sprintf(cmdbuf,
  "TCL_LIBRARY=\"%s/lib/tcl/library\" TK_LIBRARY=\"%s/lib/tk/library\"%s \
  " WISH " \"%s/" PDGUIDIR "/pd-gui.tcl\" %d\n",
                 libdir, libdir, (getenv("HOME") ? "" : " HOME=/tmp"),
                    libdir, portno);
#endif /* __APPLE__ */
#ifdef THREADED_GUI_IO
            printf("running: %s\n", cmdbuf);
#endif // THREADED_GUI_IO
            guicmd = cmdbuf;
        }
        if (sys_verbose)
            fprintf(stderr, "%s", guicmd);

        childpid = fork();
        if (childpid < 0)
        {
            if (errno) perror("sys_startgui");
            else fprintf(stderr, "sys_startgui failed\n");
            sys_closesocket(sockfd);
            return (1);
        }
        else if (!childpid)                     /* we're the child */
        {
            sys_closesocket(sockfd);     /* child doesn't listen */
#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)
            sys_set_priority(MODE_NRT);  /* child runs non-real-time */
#endif
#ifndef __APPLE__
// TODO this seems unneeded on any platform hans@eds.org
                /* the wish process in Unix will make a wish shell and
                    read/write standard in and out unless we close the
                    file descriptors.  Somehow this doesn't make the MAC OSX
                        version of Wish happy...*/
            if (pipe(stdinpipe) < 0)
                sys_sockerror("pipe");
            else
            {
                if (stdinpipe[0] != 0)
                {
                    close (0);
                    dup2(stdinpipe[0], 0);
                    close(stdinpipe[0]);
                }
            }
#endif /* NOT __APPLE__ */
            execl("/bin/sh", "sh", "-c", guicmd, (char*)0);
            perror("pd: exec");
            fprintf(stderr, "Perhaps tcl and tk aren't yet installed?\n");
            _exit(1);
       }
#else /* NOT _WIN32 */
        /* fprintf(stderr, "%s\n", libdir); */

        strcpy(scriptbuf, "\"");
        strcat(scriptbuf, libdir);
        strcat(scriptbuf, "/" PDGUIDIR "pd-gui.tcl\"");
        sys_bashfilename(scriptbuf, scriptbuf);

        sprintf(portbuf, "%d", portno);

        strcpy(wishbuf, libdir);
        strcat(wishbuf, "/" PDBINDIR WISH);
        sys_bashfilename(wishbuf, wishbuf);

        spawnret = _spawnl(P_NOWAIT, wishbuf, WISH, scriptbuf, portbuf, NULL);
        if (spawnret < 0)
        {
            perror("spawnl");
            fprintf(stderr, "%s: couldn't load TCL\n", wishbuf);
            return (1);
        }
#endif /* NOT _WIN32 */
        if (sys_verbose)
            fprintf(stderr, "Waiting for connection request... \n");
        if (listen(sockfd, 5) < 0)
        {
            sys_sockerror("listen");
            sys_closesocket(sockfd);
            return (1);
        }

        pd_this->pd_inter->i_guisock = accept(sockfd, 0, 0);

        sys_closesocket(sockfd);

        if (pd_this->pd_inter->i_guisock < 0)
        {
            sys_sockerror("accept");
            return (1);
        }
        if (sys_verbose)
            fprintf(stderr, "... connected\n");
        pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    }

    pd_this->pd_inter->i_socketreceiver = socketreceiver_new(0, 0, 0, 0);
#ifdef THREADED_GUI_IO
    sys_addpollfnsr(pd_this->pd_inter->i_guisock,
#else // THREADED_GUI_IO
    sys_addpollfn(pd_this->pd_inter->i_guisock,
#endif // THREADED_GUI_IO
        (t_fdpollfn)socketreceiver_read,
            pd_this->pd_inter->i_socketreceiver);

            /* here is where we start the pinging. */
#if defined(__linux__) || defined(__FreeBSD_kernel__)
    if (sys_hipriority)
        sys_gui("pdtk_watchdog\n");
#endif
    sys_get_audio_apis(apibuf);
    sys_get_midi_apis(apibuf2);
    sys_set_searchpath();     /* tell GUI about path and startup flags */
    sys_set_temppath();
    sys_set_extrapath();
    sys_set_startup();
                       /* ... and about font, medio APIS, etc */
    sys_vgui("pdtk_pd_startup %d %d %d {%s} %s %s {%s} %s\n",
             PD_MAJOR_VERSION, PD_MINOR_VERSION,
             PD_BUGFIX_VERSION, PD_TEST_VERSION,
             apibuf, apibuf2, sys_font, sys_fontweight);
    sys_vgui("set pd_whichapi %d\n", sys_audioapi);
    sys_vgui("set zoom_open %d\n", sys_zoom_open == 2);

    sys_init_deken();
    return (0);
}

void sys_setrealtime(const char *libdir)
{
    char cmdbuf[MAXPDSTRING];
#if defined(__linux__) || defined(__FreeBSD_kernel__)
        /*  promote this process's priority, if we can and want to.
        If sys_hipriority not specified (-1), we assume real-time was wanted.
        Starting in Linux 2.6 one can permit real-time operation of Pd by]
        putting lines like:
                @audio - rtprio 99
                @audio - memlock unlimited
        in the system limits file, perhaps /etc/limits.conf or
        /etc/security/limits.conf, and calling Pd from a user in group audio. */
    if (sys_hipriority == -1)
        sys_hipriority = 1;

    snprintf(cmdbuf, MAXPDSTRING, "%s/bin/pd-watchdog", libdir);
    cmdbuf[MAXPDSTRING-1] = 0;
    if (sys_hipriority)
    {
        struct stat statbuf;
#ifdef THREADED_GUI_IO
        //HACK: TODO: remove || 1
        if (stat(cmdbuf, &statbuf) < 0 || 1)
#else // THREADED_GUI_IO
        if (stat(cmdbuf, &statbuf) < 0)
#endif // THREADED_GUI_IO
        {
            fprintf(stderr,
              "disabling real-time priority due to missing pd-watchdog (%s)\n",
                cmdbuf);
            sys_hipriority = 0;
        }
    }
    if (sys_hipriority)
    {
        int pipe9[2], watchpid;
            /* To prevent lockup, we fork off a watchdog process with
            higher real-time priority than ours.  The GUI has to send
            a stream of ping messages to the watchdog THROUGH the Pd
            process which has to pick them up from the GUI and forward
            them.  If any of these things aren't happening the watchdog
            starts sending "stop" and "cont" signals to the Pd process
            to make it timeshare with the rest of the system.  (Version
            0.33P2 : if there's no GUI, the watchdog pinging is done
            from the scheduler idle routine in this process instead.) */

        if (pipe(pipe9) < 0)
        {
            sys_sockerror("pipe");
            return;
        }
        watchpid = fork();
        if (watchpid < 0)
        {
            if (errno)
                perror("sys_setpriority");
            else fprintf(stderr, "sys_setpriority failed\n");
            return;
        }
        else if (!watchpid)             /* we're the child */
        {
            sys_set_priority(MODE_WATCHDOG);
            if (pipe9[1] != 0)
            {
                dup2(pipe9[0], 0);
                close(pipe9[0]);
            }
            close(pipe9[1]);

            if (sys_verbose) fprintf(stderr, "%s\n", cmdbuf);
            execl("/bin/sh", "sh", "-c", cmdbuf, (char*)0);
            perror("pd: exec");
            _exit(1);
        }
        else                            /* we're the parent */
        {
            sys_set_priority(MODE_RT);
            close(pipe9[0]);
                /* set close-on-exec so that watchdog will see an EOF when we
                close our copy - otherwise it might hang waiting for some
                stupid child process (as seems to happen if jackd auto-starts
                for us.) */
            if(fcntl(pipe9[1], F_SETFD, FD_CLOEXEC) < 0)
              perror("close-on-exec");
            sys_watchfd = pipe9[1];
                /* We also have to start the ping loop in the GUI;
                this is done later when the socket is open. */
        }
    }
    else if (sys_verbose)
        post("not setting real-time priority");
#endif /* __linux__ */

#ifdef _WIN32
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        fprintf(stderr, "pd: couldn't set high priority class\n");
#endif
#ifdef __APPLE__
    if (sys_hipriority)
    {
        struct sched_param param;
        int policy = SCHED_RR;
        int err;
        param.sched_priority = 80; /* adjust 0 : 100 */

        err = pthread_setschedparam(pthread_self(), policy, &param);
        if (err)
            post("warning: high priority scheduling failed");
    }
#endif /* __APPLE__ */
}

extern void sys_exit(void);

/* This is called when something bad has happened, like a segfault.
Call glob_quit() below to exit cleanly.
LATER try to save dirty documents even in the bad case. */
void sys_bail(int n)
{
    static int reentered = 0;
    if (!reentered)
    {
        reentered = 1;
#if !defined(__linux__) && !defined(__FreeBSD_kernel__) && !defined(__GNU__)
            /* sys_close_audio() hangs if you're in a signal? */
        fprintf(stderr ,"gui socket %d - \n", pd_this->pd_inter->i_guisock);
        fprintf(stderr, "closing audio...\n");
        sys_close_audio();
        fprintf(stderr, "closing MIDI...\n");
        sys_close_midi();
        fprintf(stderr, "... done.\n");
#endif
        exit(n);
    }
    else _exit(1);
}

void glob_quit(void *dummy)
{
    sys_close_audio();
    sys_close_midi();
    if (sys_havegui())
    {
        sys_closesocket(pd_this->pd_inter->i_guisock);
        sys_rmpollfn(pd_this->pd_inter->i_guisock);
    }
    exit(0);
}

    /* recursively descend to all canvases and send them "vis" messages
    if they believe they're visible, to make it really so. */
static void glist_maybevis(t_glist *gl)
{
    t_gobj *g;
    for (g = gl->gl_list; g; g = g->g_next)
        if (pd_class(&g->g_pd) == canvas_class)
            glist_maybevis((t_glist *)g);
    if (gl->gl_havewindow)
    {
        canvas_vis(gl, 0);
        canvas_vis(gl, 1);
    }
}

int sys_startgui(const char *libdir)
{
    t_canvas *x;
    for (x = pd_getcanvaslist(); x; x = x->gl_next)
        canvas_vis(x, 0);
    pd_this->pd_inter->i_havegui = 1;
    pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    if (sys_do_startgui(libdir))
        return (-1);
    for (x = pd_getcanvaslist(); x; x = x->gl_next)
        if (strcmp(x->gl_name->s_name, "_float_template") &&
            strcmp(x->gl_name->s_name, "_float_array_template") &&
                strcmp(x->gl_name->s_name, "_text_template"))
    {
        glist_maybevis(x);
        canvas_vis(x, 1);
    }
    return (0);
}

 /* more work needed here - for some reason we can't restart the gui after
 shutting it down this way.  I think the second 'init' message never makes
 it because the to-gui buffer isn't re-initialized. */
void sys_stopgui(void)
{
    t_canvas *x;
    for (x = pd_getcanvaslist(); x; x = x->gl_next)
        canvas_vis(x, 0);
    sys_vgui("%s", "exit\n");
    if (pd_this->pd_inter->i_guisock >= 0)
    {
        sys_closesocket(pd_this->pd_inter->i_guisock);
        sys_rmpollfn(pd_this->pd_inter->i_guisock);
        pd_this->pd_inter->i_guisock = -1;
    }
    pd_this->pd_inter->i_havegui = 0;
}

/* ----------- mutexes for thread safety --------------- */

void s_inter_newpdinstance(void)
{
    pd_this->pd_inter = getbytes(sizeof(*pd_this->pd_inter));
#if PDTHREADS
    pthread_mutex_init(&pd_this->pd_inter->i_mutex, NULL);
    pd_this->pd_islocked = 0;
#endif
#ifdef _WIN32
    pd_this->pd_inter->i_freq = 0;
#endif
    pd_this->pd_inter->i_havegui = 0;
}

void s_inter_free(t_instanceinter *inter)
{
    if (inter->i_fdpoll)
    {
        binbuf_free(inter->i_inbinbuf);
        inter->i_inbinbuf = 0;
        t_freebytes(inter->i_fdpoll, inter->i_nfdpoll * sizeof(t_fdpoll));
        inter->i_fdpoll = 0;
        inter->i_nfdpoll = 0;
    }
    freebytes(inter, sizeof(*inter));
}

void s_inter_freepdinstance(void)
{
    s_inter_free(pd_this->pd_inter);
}

#if PDTHREADS
#ifdef PDINSTANCE
static pthread_rwlock_t sys_rwlock = PTHREAD_RWLOCK_INITIALIZER;
#else /* PDINSTANCE */
static pthread_mutex_t sys_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif /* PDINSTANCE */
#endif /* PDTHREADS */

#if PDTHREADS

/* routines to lock and unlock Pd's global class structure or list of Pd
instances.  These are called internally within Pd when creating classes, adding
methods to them, or creating or freeing Pd instances.  They should probably
not be called from outside Pd.  They should be called at a point where the
current instance of Pd is currently locked via sys_lock() below; this gains
read access to the class and instance lists which must be released for the
write-lock to be available. */

void pd_globallock(void)
{
#ifdef PDINSTANCE
    if (!pd_this->pd_islocked)
        bug("pd_globallock");
    pthread_rwlock_unlock(&sys_rwlock);
    pthread_rwlock_wrlock(&sys_rwlock);
#endif /* PDINSTANCE */
}

void pd_globalunlock(void)
{
#ifdef PDINSTANCE
    pthread_rwlock_unlock(&sys_rwlock);
    pthread_rwlock_rdlock(&sys_rwlock);
#endif /* PDINSTANCE */
}

/* routines to lock/unlock a Pd instance for thread safety.  Call pd_setinsance
first.  The "pd_this"  variable can be written and read thread-safely as it
is defined as per-thread storage. */
void sys_lock(void)
{
#ifdef PDINSTANCE
    pthread_mutex_lock(&pd_this->pd_inter->i_mutex);
    pthread_rwlock_rdlock(&sys_rwlock);
    pd_this->pd_islocked = 1;
#else
    pthread_mutex_lock(&sys_mutex);
#endif
}

void sys_unlock(void)
{
#ifdef PDINSTANCE
    pd_this->pd_islocked = 0;
    pthread_rwlock_unlock(&sys_rwlock);
    pthread_mutex_unlock(&pd_this->pd_inter->i_mutex);
#else
    pthread_mutex_unlock(&sys_mutex);
#endif
}

int sys_trylock(void)
{
#ifdef PDINSTANCE
    int ret;
    if (!(ret = pthread_mutex_trylock(&pd_this->pd_inter->i_mutex)))
    {
        if (!(ret = pthread_rwlock_tryrdlock(&sys_rwlock)))
            return (0);
        else
        {
            pthread_mutex_unlock(&pd_this->pd_inter->i_mutex);
            return (ret);
        }
    }
    else return (ret);
#else
    return pthread_mutex_trylock(&sys_mutex);
#endif
}

#else /* PDTHREADS */

#ifdef TEST_LOCKING /* run standalone Pd with this to find deadlocks */
static int amlocked;
void sys_lock(void)
{
    if (amlocked) bug("duplicate lock");
    amlocked = 1;
}

void sys_unlock(void)
{
    if (!amlocked) bug("duplicate unlock");
    amlocked = 0;
}
#else
void sys_lock(void) {}
void sys_unlock(void) {}
#endif
void pd_globallock(void) {}
void pd_globalunlock(void) {}

#endif /* PDTHREADS */
