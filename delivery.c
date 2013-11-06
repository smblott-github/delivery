
/* ************************************************************************
 * delivery:
 *    deliver one data stream to one or more dynamically attaching and
 *    detaching clients
 *
 * delivery <server_command> [ <arg> ... ] -- is server mode:
 *    wait for a client to connect, then call <server_command> with its
 *    arguments and deliver its standard output to the client; subsequent
 *    clients receive the same data stream (there's at most one invocation of
 *    <server_command> running at any one time): one source, multiple sinks for
 *    a single data stream
 *
 *    the server psocess exits when the last client disconnects.  If the server
 *    is required to be persistent, then some other mechanism is required to
 *    retsart it (such as "supervise", see below).
 *
 * delivery -c <client_command> [ <arg> ... ] -- is client mode:
 *    connect to the server and execute <client_command> with its arguments,
 *    the standard output of <server_command> is fed to the standard input of
 *    <client_command>
 *
 *    for example:
 *	 delivery -c cat
 *    spits data streamed from the server out on standard output
 *
 *    if no command is provided:
 *       delivery -c
 *    then "cat" is assumed and the stream is delivered on standard output
 *
 * delivery -r -- restarts <server_command>:
 *    if the server is running <server_command>, close that process and start a
 *    new instance of <server_command>; other than a possible short delay,
 *    active clients remain active, but now see data streamed from the new
 *    invocation of <server_command>
 *
 * note:
 *    delivery guarantees no particular alignment with respect to the data
 *    stream received by any client other than the first; most multimedia data
 *    streams contain frame-boundary markers, and most multimedia players will
 *    happily play streams which do not begin frame aligned
 *
 *    delivery creates PID, socket and lock files in "/tmp"
 *
 *    delivery runs well under the "supervise" utility; "supervise" is part of
 *    the "daemontools" package:
 *       - http://cr.yp.to/daemontools.html
 *
 * why would one need such a thing?
 *    some devices (such a DVB tuner cards) can have at most a single process
 *    attached; with delivery, the delivery server process can be made to
 *    attach to the device and stream the data to multiple clients
 *
 *    internet radio stations consume bandwidth; generally, N clients consume N
 *    times the bandwidth; with delivery, a single delivery server process can
 *    stream an internet radio station into (say) the home, and forward that
 *    stream on to multiple clients
 *
 *    on CPU-constrained devices such as ARM-based NAS devices, a single
 *    encoding process (lame, say, or ffmpeg) may consume a considerable
 *    portion of the available CPU cycles;  delivery can allow multiple clients
 *    to consume the output of a single such process.
 */

/* ************************************************************************
 * includes
 */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>

/* ************************************************************************
 * constants
 */

// #define PIDFILE      "./run.pid"
// #define SOCKFILE     "./run.sock"
#define DELIVERYPID  "_DELIVERY_PID"
#define TMPDIR       "/tmp"
#define MAXCLIENT     1024
#define MAXNAME       64

/* ************************************************************************
 * static data
 *
 * ANSI C specifies that static data is initially zeroed, so no need to
 * initialise these ...
 */

static FILE *src;            // data source (popen)
static int   src_fd;         // Unix domain socket
static int   src_kill;       // whether and how to kill src on SIGTERM
static int   fd[MAXCLIENT];  // client file descriptors
static int   cnt;            // client count
static int   reopen;         // should the server re-popen <server_command>?
static char *buffer;	     // buffer: <server_command> -> buffer -> <client_command>
static int   bufsz;	     // buffer size
static int   err;            // generic integer error variable
static int   i;              // generic integer variable
static char  *cp;            // generic string variable
static int   world;          // world writable socket

static char *tmpbasename;
static char *PIDFILE;
static char *SOCKFILE;
static char *LOCKFILE;

/* ************************************************************************
 * forward declarations ...
 */

void close_src();

/* ************************************************************************
 * die cleanly (although it probably matters little)
 */

void rm_sockfile();
void rm_pidfile();

void die(char *message, int e)
{
   rm_sockfile();
   rm_pidfile();

   if ( message )
      fprintf(stderr, "exit %d: %s\n", e, message);

   close_src();
   while ( cnt )
      close(fd[--cnt]);

   exit(e);
}

void sig_die(int signal)
{
   die("signal, die", signal);
}

/* ************************************************************************
 * set a file descriptor (socket) to be blocking or non-blocking
 */

void mk_blocking(int fd)
{
   int flags;
   if ( (flags = fcntl(fd, F_GETFL, 0)) == -1 )
      die("get flags", errno);
   if ( fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1 )
      die("set flags", errno);
}

void mk_nonblocking(int fd)
{
   int flags;
   if ( (flags = fcntl(fd, F_GETFL, 0)) == -1 )
      die("get flags", errno);
   if ( fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 )
      die("set flags", errno);
}

/* ************************************************************************
 * reading and writing the PID file
 */

void rm_pidfile()
{
   unlink(PIDFILE);
}

void wrt_pidfile()
{
   FILE *fp;
   
   if ( ! ( fp = fopen(PIDFILE, "w") ) )
      die("fopen PIDFILE", errno);
   atexit(rm_pidfile);

   fprintf(fp, "%d", getpid());
   fclose(fp);
}

pid_t rd_pidfile()
{
   pid_t pid;
   FILE *fp;
   
   if ( ( fp = fopen(PIDFILE, "r") ) == NULL )
      die("fopen PIDFILE", errno);

   if ( fscanf(fp, "%d", &pid) != 1 )
      die("error reading pidfile", EINVAL);

   fclose(fp);

   if ( pid <= 0 )
      die("invalid pid (pid <= 0)", EINVAL);

   if ( kill(pid, 0) )
      die("invalid pid (kill(pid,0))", EINVAL);

   return pid;
}

/* ************************************************************************
 * socket utilities
 */

void rm_sockfile()
{
   if ( src_fd )
   {
      close(src_fd);
      src_fd = 0;
   }
   unlink(SOCKFILE);
}

struct sockaddr_un mk_sockaddr()
{
   struct sockaddr_un addr;

   bzero(&addr, sizeof(addr));

   strncpy(addr.sun_path, SOCKFILE, sizeof(addr.sun_path) - 1);
   addr.sun_family = AF_UNIX;
#if defined(__FreeBSD__)
   addr.sun_len = SUN_LEN(&addr) + 1 /* the NULL byte */ ;
#endif

   return addr;
}

/* ************************************************************************
 * routine for server to check for new clients
 */

void check_for_new_clients()
{

   /* first time through ...
    */

   if ( src_fd == 0 )
   {
      struct sockaddr_un addr = mk_sockaddr();

      rm_sockfile();
      atexit(rm_sockfile);

      // mask_t mask;
      int mask;

      if ( (src_fd = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0 )
	 die("socket", errno);

      if ( world )
         mask = umask(0);

      if( bind(src_fd, (struct sockaddr *) &addr, SUN_LEN(&addr)) != 0 )
	 die("bind", errno);

      if ( world )
         umask(mask);

      if ( listen(src_fd, 10) != 0 )
	 die("listen", errno);
   }

   /* every time through ...
    */

   int client_fd;

   /* the src_fd must be blocking if there are no clients, and non-blocking
    * otherwise; the client_fd inherits the blocking status of src_fd, so it
    * must be explicitly made blocking after accept
    */

   if ( cnt == 0 )
   {
      fprintf(stderr, "delivery server: blocking ...\n");
      mk_blocking(src_fd);
   }

accept_client:
   client_fd = accept(src_fd, NULL, 0);

   if ( client_fd == -1 && errno == EWOULDBLOCK )
      return; // normal exit point

   if ( client_fd == -1 && errno == EINTR )
      goto accept_client;

   if ( client_fd == -1 )
      die("accept",errno);

   /* we reach here only if there is in fact a new client connecting
    */

   fprintf(stderr, "delivery server: non-blocking ...\n");
   mk_nonblocking(src_fd);
   mk_blocking(client_fd);

   if ( MAXCLIENT == cnt )
   {
      fprintf(stderr, "MAXCLIENT (%d) exceeded\n", MAXCLIENT);
      close(client_fd);
   }
   else
   {
      fprintf(stderr, "new: %d/%d --> %d\n", cnt, cnt, cnt + 1);
      fd[cnt++] = client_fd;
   }

   /* loop back, and try to accept another client ...
    */

   goto accept_client;
}

/* ************************************************************************
 */

void reopen_src(int s)
{
   fprintf(stderr, "signal %d (reopen_src)\n", s);
   if ( src )
      reopen = 1;
}

void close_src()
{
   if ( src )
   {
      signal(SIGCHLD, SIG_DFL);
      pclose(src);
      // is there a race condition here?  can we be sure that any SIGCHLD has
      // been delivered at this point?
      signal(SIGCHLD, sig_die);
      src = NULL;
   }
   reopen = 0;
}

char *print(char *prev, const char *format, ...) {
   char *cp = 0;
   va_list ap;
   va_start(ap,format);
   if ( ! vasprintf(&cp, format, ap) )
      perror("vasprintf failed)");
   if ( ! cp )
      perror("vasprintf/malloc failed)");
   va_end(ap);
   if ( prev )
      free(prev);
   return cp;
}

void open_src(char *argv[])
{
   if ( reopen || cnt == 0 ) close_src();
   if ( src    || cnt == 0 ) return;

   cp = 0;
   for (i=0; argv[i]; i+=1)
      cp = print(cp, "%s%s%s", i ? cp : "", i ? " " : "", argv[i]);

   fprintf(stderr, "popen: %s\n", cp);
   if ( ! ( src = popen(cp, "r") ) )
      die(argv[0], EBADF);

   free(cp);
}

/* ************************************************************************
 */

int read_buf()
{
   assert(src);

   if ( buffer == NULL )
   {
      /* choose a size for the read/write buffer and allocate space
       *   (not sure why we're bothering with this; might as well just choose
       *   bufsz = 4096 and be done)
       */
      struct stat sb;

      if ( ( bufsz = (int) sysconf(_SC_PAGESIZE) ) == -1 )
	 die("sysconf", errno);

      if ( fstat(fileno(src), &sb) == -1 )
	 die("stat", errno);

      if ( sb.st_blksize > bufsz )
	 bufsz = sb.st_blksize;

      fprintf(stderr, "bufsz: %d\n", bufsz);

      if ( bufsz <= 0 )
	 die("bufsz", EINVAL);

      if ( ! ( buffer = malloc(bufsz) ) )
	 die("malloc", errno);
   }

   do err = fread(buffer, bufsz, 1, src);
   while ( err < 1 && ( errno == EINTR || errno == EAGAIN ) );

   if ( err != 1 )
      die("fread", errno);

   return err == 1;
}

/* ************************************************************************
 */

void write_buf()
{
   int i = 0;

   while ( i < cnt )
   {
      int off, nw, nr = bufsz;

      for (off = 0; nr != 0; nr -= nw, off += nw)
	 if ( ( nw = write(fd[i], buffer + off, nr) ) < 0 )
	 {
	    nw = 0;
	    if ( errno != EINTR )
	       break;
	 }

      if ( nr == 0 )
      {
	 // successful write: move on to next client
	 i += 1;
	 continue;
      }

      // unsuccessful write: close this client
      fprintf(stderr, "drop: %d/%d --> %d\n", i, cnt, cnt-1);
      close(fd[i]);

      int j;
      for ( j=i+1; j<cnt; j+=1 )
	 fd[j-1] = fd[j];

      fd[--cnt] = 0;
   }
}

/* ************************************************************************
 */

void usage(char *name)
{
   fprintf(stderr,"usage: %s shell-command [ arg ... ]    (server mode)\n",    name);
   fprintf(stderr,"   or: %s -c shell-command [ arg ... ] (client mode)\n",    name);
   fprintf(stderr,"   or: %s -r                           (restart source)\n", name);
   die(0,EINVAL);
}

/* ************************************************************************
 */

void reopen_server()
{
   if ( kill(rd_pidfile(), SIGHUP) != 0 )
      die("cannot signal server process", EIO);
}

/* ************************************************************************
 */

static int   default_client_argc   = 1;
static char *default_client_argv[] = { "cat", NULL };

void client(int argc, char *argv[], int opt_dryrun)
{
   struct sockaddr_un addr = mk_sockaddr();
   int fd;

   if ( argc == 0 )
   {
      argc = default_client_argc;
      argv = default_client_argv;
   }

   if ( (fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0 )
      die("socket", errno);

   if( connect(fd, (struct sockaddr *) &addr, SUN_LEN(&addr)) )
      die("connect", errno);

   close(STDIN_FILENO);
   if ( dup2(fd, STDIN_FILENO) == -1 )
      die("dup2", EIO);
   close(fd);

   if ( execvp(argv[0], argv) == -1 )
      die("execvp", errno);

   die("unreachable", errno);
}

/* ************************************************************************
 */

int main(int argc, char *argv[])
{
   char *my_name     = argv[0];
   int   opt_client  = 0;
   int   opt_dryrun  = 0;
   int   opt_restart = 0;

   /* options
    */

   {
      int opt;

      while ( (opt = getopt(argc, argv, "dwcrt:n:")) != -1 )
      {
	 switch ( opt )
	 {
	    case 'w':
	       world = 1;
	       break;
	    case 'd':
	       opt_dryrun = 1;
	       break;
	    case 'c':
	       opt_client = 1;
	       break;
	    case 'r':
	       opt_restart = 1;
	       break;
	    case 't':
	       src_kill = atoi(optarg);
	       break;
	    case 'n':
               tmpbasename = optarg;
	       break;
	    default:
	       usage(my_name);
	       die("unreachable", 0);
	       break;
	 }
      }

      argv += optind;
      argc -= optind;
   }

   /* set up file names
    */

   if ( ! tmpbasename )
   {
      char  cs[MAXNAME];
      FILE *CS = popen("realpath . | cksum /dev/stdin", "r");

      if ( CS == NULL )
         die("popen/tmpbasename", errno);

      if ( fscanf(CS, "%s", cs) != 1 )
         die("popen/fscanf", 1);

      pclose(CS);
      tmpbasename = print(0, "%s", cs);
   }

   PIDFILE  = print(0, "%s/delivery.%s.pid",  TMPDIR, tmpbasename);
   SOCKFILE = print(0, "%s/delivery.%s.sock", TMPDIR, tmpbasename);
   LOCKFILE = print(0, "%s/delivery.%s.lock", TMPDIR, tmpbasename);
   printf("%s\n", SOCKFILE);

   if ( opt_dryrun )
      exit(0);

   /* restart running server, or become client (or both)
    */

   if ( opt_restart ) reopen_server();
   if ( opt_client  ) client(argc, argv, opt_dryrun); // never returns
   if ( opt_restart ) die(0,0);

   /* if we reach here, then this is the server process ...
    */

   if ( ! argc )
      die("no arguments", 1);

   /* lock file: we want at most one server process ...
    */

   {
      LOCKFILE = print(0, "%s/delivery.%s.lock", TMPDIR, tmpbasename);

      int fd = open(LOCKFILE, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO );
      if ( fd == -1 )
      {
         fprintf(stderr, "error: could not create lock file: %s\n", LOCKFILE);
         exit(1);
      }

      if ( flock(fd, LOCK_EX | LOCK_NB) )
      {
         fprintf(stderr, "error: could not obtain exclusive lock: %s\n", LOCKFILE);
         exit(1);
      }
   }

   /* put a variable in the environment so that the command can detect, if it
    * wishes, that it's being called under the control of delivery
    */

   cp = print(0, "%d", getpid());
   setenv(DELIVERYPID, cp, 0);
   free(cp);

   /* signals and pidfile
    */

   signal(SIGHUP,  reopen_src);
   signal(SIGTERM, sig_die);
   signal(SIGINT,  sig_die);
   signal(SIGKILL, sig_die);
   signal(SIGCHLD, sig_die);
   signal(SIGPIPE, SIG_IGN);

   wrt_pidfile();

   /* main server loop ...
    */

   do
   {
      check_for_new_clients(); // blocking, but only if there are no active clients
      open_src(argv);
      if ( read_buf() )
         write_buf(); 
   }
   while ( cnt );

   die("",0);
   return 0;
}

