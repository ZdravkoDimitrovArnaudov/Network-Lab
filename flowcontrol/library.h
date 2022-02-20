#if DMALLOC
#include <dmalloc.h>
#endif /* DMALLOC */

#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include "global.h"

#ifndef _LIBRARYH_
#define _LIBRARYH_

struct config_common {
	int window; /* # of unacknowledged packets in flight */
	long timeout; /* Retransmission timeout in nanoseconds*/
	float probError;
};

/*
from http://stackoverflow.com/questions/3219393/
http://en.wikipedia.org/wiki/ANSI_escape_code#Colors
*/
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_BLACK   "\x1b[0m"
#define ANSI_COLOR_RESET   ANSI_COLOR_BLACK

#define FOO(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define DEBUG_TIMER(priority, MESSAGE, ...)\
{\
	if (opt_debug >= priority) {\
		printf(ANSI_COLOR_BLUE  "    TIMER: "   MESSAGE   ANSI_COLOR_RESET "\n", ##__VA_ARGS__);\
		fflush(stdout);\
	}\
}\

#define DEBUG_RECEPTION(priority, MESSAGE, ...)\
{\
	if (opt_debug >= priority) {\
		printf(ANSI_COLOR_GREEN  "    RECEPTION: "   MESSAGE   ANSI_COLOR_RESET "\n", ##__VA_ARGS__);\
		fflush(stdout);\
	}\
}\

#define DEBUG_SEND(priority, MESSAGE, ...)\
{\
	if (opt_debug >= priority) {\
		printf(ANSI_COLOR_MAGENTA  "    SEND: "  MESSAGE   ANSI_COLOR_RESET "\n", ##__VA_ARGS__);\
		fflush(stdout);\
	}\
}\

#define DEBUG_ERRORS(priority, MESSAGE, ...)\
{\
	if (opt_debug >= priority) {\
		printf(ANSI_COLOR_RED  "    ERRORS: "  MESSAGE   ANSI_COLOR_RESET "\n", ##__VA_ARGS__);\
		fflush(stdout);\
	}\
}\

#define DEBUG_MSG(priority, COLOR, MESSAGE)\
{\
  if (opt_debug >= priority) {\
      printf( "\(COLOR) (MESSAGE)");\
		fflush(stdout);\
    }\
  }\


extern char *progname; /* Set to name of program by main */
extern int opt_debug; /* When != 0, print packets */

#if !DMALLOC
void *xmalloc(size_t);
#endif /* !DMALLOC */
uint16_t cksum(const void *_data, int len); /* compute TCP-like checksum */

/* Returns 1 when two addresses equal, 0 otherwise */
int addreq(const struct sockaddr_storage *a, const struct sockaddr_storage *b);

/* Hash a socket address down to a number.  Multiple addresses may
 hash to the same number, and the hash value is not guaranteed to be
 the same on different machines, but this may be useful for
 implementing a hash table. */
//unsigned int addrhash(const struct sockaddr_storage *s);
/* Actual size of the real socket address structure stashed in a
 sockaddr_storage. */
size_t addrsize(const struct sockaddr_storage *ss);

/* Useful for debugging. */
void print_pkt(const packet_t *buf, const char *op, int n);

/* This is an opaque structure provided by rlib.  You only need
 * pointers to it.  */
typedef struct conn conn_t;

/* Call this function to send a UDP packet to the other side. */
int SEND_PACKET(const packet_t *pkt, size_t len);

/* Below are some utility functions you don't need for this lab */

/* Fill in a sockaddr_storage with a socket address.  If local is
 * non-zero, the socket will be used for binding.  If dgram is
 * non-zero, use datagram sockets (e.g., UDP).  If unixdom is
 * non-zero, use unix-domain sockets.  name is either "port" or
 * "host:port". */
int get_address(struct sockaddr_storage *ss, int local, int dgram, int unixdom, char *name);

/* Put socket in non-blocking mode */
int make_async(int s);

/* Bind to a particular socket (and listen if not dgram). */
int listen_on(int dgram, struct sockaddr_storage *ss);

/* Convenient way to get a socket connected to a destination */
int connect_to(int dgram, const struct sockaddr_storage *ss);

#include <time.h>
#include <sys/time.h>

#ifndef CLOCK_REALTIME
# define NEED_CLOCK_GETTIME 1
# define CLOCK_REALTIME 0
# undef CLOCK_MONOTONIC
# define CLOCK_MONOTONIC 1
#endif /* !HAVE_CLOCK_GETTIME */

#if NEED_CLOCK_GETTIME
int clock_gettime(int, struct timespec *);
#endif /* NEED_CLOCK_GETTIME */

#endif //_LIBRARYH_
