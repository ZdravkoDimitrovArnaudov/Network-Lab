#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sched.h>

#include "global.h"
#include "library.h"

char *progname;
int opt_debug;
int log_in = -1;
int log_out = -1;
struct config_common c;

// GLOBAL DATA HERE
int nfd; /* network file descriptor */
struct sockaddr_storage peer; /* network peer */
int continueExecution;

//syntheticTraffic:
// If >0, it means we use the synthetic traffic generator; the application sends a flow of messages
// If ==0, the console is used as the input/output of the application (default config)
static int syntheticTraffic;
int synthTrStart;
int synthDataBlock;
int synthTxIndex;
int synthRxIndex;
int synthTxIndex_1024;
int synthRxIndex_1024;

static int rpoll; /* If >0, it means we need to poll the input (console). The value is the offset into cevents array */
static int npoll; /* If >0, it means we need to poll the network. The value is the offset into cevents array */

static int rfd; /* input file descriptor */
static int wfd; /* output file descriptor */
//static int nfd; /* network file descriptor */
//static struct sockaddr_storage peer; /* network peer */

static char read_eof; /* zero if haven't received EOF */
static char write_eof; /* send EOF when output queue drained */
static char write_err; /* zero if it's okay to write to wfd */
static char xoff; /* non-zero to pause reading */

static void conn_mkevents(void);
static int debug_recv(int s, packet_t *buf, size_t len, int flags, struct sockaddr_storage *from);
int compareDates(struct timespec time1, struct timespec time2);

//int cevents_generation;
static struct pollfd *cevents;
static int ncevents;
static int *evreaders;
static struct pollfd netPolling;



static int pausedTransmission; //0: packets can be transmitted; >0: do not generate traffic (never call
static packet_t *packet_ptr;
static packet_t *corruptedPacket;

// Variables related to timers
// int gettimeofday(struct timeval *tv, struct timezone *tz);	, with tz being NULL
#define TIMER_COUNT 16
int activeTimerCount;
int timerSet[TIMER_COUNT]; //If >0, the given timer is set, and the expiration time is in the corresponding field in timerExpirationDate
struct timespec timerExpirationDate[TIMER_COUNT]; //Contains the specific date (in ns) in which each timer expires

//Stats
long receivedPackets, receivedCorrectPackets, receivedCorruptPackets;
long sentPackets, sentCorrectPackets, sentCorruptPackets;
long long generatedApplicationBytes, acceptedApplicationBytes; 	//Correct application bytes, not headers
long long sentBytes, sentCorrectBytes, sentCorruptBytes;			//Overall: Headers + application, including correct and corrupt packets
struct timespec startRxTime;	//The time of the first received packet. Valid if receivedPackets > 0
struct timespec startTxTime;	//The time of the first generated packet. Valid if generatedBytes > 0
int printedStats;
struct timespec lastStatPrintTime;

struct chunk {
	struct chunk *next;
	size_t size;
	size_t used;
	char buf[1];
};
typedef struct chunk chunk_t;

#if !DMALLOC
void *xmalloc(size_t n) {
	void *p = malloc(n);
	if (!p) {
		fprintf(stderr, "%s: out of memory allocating %d bytes\n", progname, (int) n);
		abort();
	}
	return p;
}
#endif /* !DMALLOC */

void print_pkt(const packet_t *buf, const char *op, int n) {
	static int pid = -1;
	int saved_errno = errno;
	if (pid == -1)
		pid = getpid();
	if (n < 0) {
		if (errno != EAGAIN)
			fprintf(stderr, "%5d %s(%3d): %s\n", pid, op, n, strerror(errno));
	} else if (n == 8)
		fprintf(stderr, "%5d %s(%3d): cksum = %04x, len = %04x, ack = %08x\n", pid, op, n, buf->cksum, ntohs(buf->len), ntohl(buf->ackno));
	else if (n >= 10)
		fprintf(stderr, "%5d %s(%3d): cksum = %04x, len = %04x, ack = %08x, seq = %08x\n", pid, op, n, buf->cksum, ntohs(buf->len), ntohl(buf->ackno),
				ntohl(buf->seqno));
	else
		fprintf(stderr, "%5d %s(%3d):\n", pid, op, n);
	errno = saved_errno;
}

/*
 * Sends a packet to the other end of the connection, size of the whole packet "len"
 */
int SEND_PACKET(const packet_t *pkt, size_t len) {
	int n, i, rv;
	float randomValue;
	randomValue = ((float) rand() / (RAND_MAX * 1.0));

	assert(sentPackets >= 0);
	sentPackets++;

	//Check if we can send data...
	// wait for events on the sockets, 3.5 second timeout
	rv = poll(&netPolling, 1, 0);
	if (rv == -1) {
	    perror("poll SEND_PACKET"); // error occurred in poll()
	} else if (rv == 0) {
	    //printf("Network socket not available to send data!!\n");
	    return -1;
	} else {
		assert(netPolling.revents & POLLOUT);
	}
	    // check for events on s1:

	if (randomValue < c.probError) {	//packet corruption!!
		DEBUG_ERRORS(1, "Sent packet is corrupted!! (Probability: %f)", c.probError);
		corruptedPacket->cksum = -1;	//Simple model!!! 1: checksum OK; -1: checksum fails!!
		corruptedPacket->len = rand() % 516;
		corruptedPacket->seqno = rand() % 1024;
		corruptedPacket->type = rand() % 1024;
		if (len > 10)
			for (i = 0; i < (len - 10); i++)
				corruptedPacket->data[i] = rand() % 256;
		n = send(nfd, corruptedPacket, len, 0);
		if (n > 0) {
			assert(sentCorruptPackets >= 0);
			assert(sentCorruptBytes >= 0);
			sentCorruptPackets++;
			sentCorruptBytes += n;
		}
	} else {
		DEBUG_ERRORS(2, "Sent packet is OK (NOT corrupted) (Probability: %f)", c.probError);
		n = send(nfd, pkt, len, 0);
		if (n > 0) {
			assert(sentCorrectPackets >= 0);
			sentCorrectPackets++;
			assert(sentCorrectBytes >= 0);
			sentCorrectBytes += n;
		}
		if (pkt->type == DATA) {
			//printf("Packet %d sent, block %u\n", pkt->seqno, (unsigned char) pkt->data[0]);
		}
	}

	if (n < 0) {
		fprintf(stderr, "Transmission error: %s\n", strerror(errno));
		continueExecution = 0;
		return n;
	}
	if (n != len) {
		//printf("Error: Sent %d bytes, but packet size is %d bytes\n", n, len);
		continueExecution = 0;
		return n;
		//assert(n==len);
	}
	assert(sentBytes >= 0);
	sentBytes += n;
	if (opt_debug > 3)
		print_pkt(pkt, "send", n);
	fflush(stdout);
	return n;
}

/*
 * Sends a packet to the other end of the connection, specifying each field of the packet in the function options.
 * This function is valid ONLY for data packets (not ACK).
 * Return value: 1 -> all the bytes were correctly sent; 0 -> Couldn't transmit all the bytes in the packet
 */
int SEND_DATA_PACKET(packetType_t pckType, uint16_t length, uint32_t ackNo, uint32_t seqno, void *data) {
	int n, dataLength;
	packet_ptr->cksum = 1;
	packet_ptr->len = (uint16_t) length;
	packet_ptr->type = (uint16_t) pckType;
	packet_ptr->ackno = (uint16_t) ackNo;
	packet_ptr->seqno = (uint16_t) seqno;
	dataLength = length - 10;	//Header length: 10 bytes
	memcpy(&(packet_ptr->data), data, dataLength);
	n = SEND_PACKET(packet_ptr, length);
	DEBUG_SEND(1, "Data packet sent, seq. index %d\n", seqno);
	return (n == length);
}

/*
 * Sends a packet to the other end of the connection, specifying each field of the packet in the function options
 * This function is valid ONLY for ACK packets (not DATA).
 * Return value: 1 -> all the bytes were correctly sent; 0 -> Couldn't transmit all the bytes in the packet
 */
int SEND_ACK_PACKET(uint32_t ackNo) {
	int n;

	packet_ptr->cksum = 1;
	packet_ptr->len = 8; //ACK packet
	packet_ptr->type = (uint16_t) ACK;
	packet_ptr->ackno = ackNo;
	n = SEND_PACKET(packet_ptr, 8);	//ACK packets are 8 bytes
	DEBUG_SEND(1, "ACK packet sent, ACK index: %d", ackNo);
	return (n == 8);
}

/*
 * Accepts the data in a packet, with size _n. Note that _buf is a pointer to the data field in the packet, and
 * _n is the size of the data field only, not the size of the whole packet
 */
int ACCEPT_DATA(const void *_buf, size_t _n) {
	const char *buf = _buf;
	int n = _n;
	int indexDiff;
	int t;
	char firstChar;
	uint8_t firstByte;

	if (log_out >= 0)
		t = write(log_out, buf, n);

	if (syntheticTraffic) {	//The first byte indicates the sequence
		assert((synthRxIndex +1)%256 == synthRxIndex_1024 % 256);
		if (_n != synthDataBlock) {
			printf("Accepted block of %d bytes, but the application sends blocks of %d bytes.\n", n, synthDataBlock);
			pause();
			exit(-1);
		}
		firstByte = (uint8_t) buf[0];
		firstByte %= 256;
		if (firstByte < 0) {
			firstByte += 255;
		}
		assert(firstByte >= 0);
		if (firstByte != synthRxIndex) {
			indexDiff = (synthRxIndex - firstByte) % 256;
			if (indexDiff == 0) {
				//printf("synthRxIndex = %d, firstByte = %d\n", synthRxIndex, firstByte);
				fflush(stdout);
			}
			assert(indexDiff != 0);
			if (indexDiff > 0) {
				printf("Error: Duplicated (or corrupt) block received. Expected index: %d, Received: %d\n", synthRxIndex, firstByte);
			} else {
				printf("Error: Missing block in the accepted data!! (or corrupted data accepted). Expected index: %d, Received: %d\n", synthRxIndex,
						firstByte);
			}
			fflush(stdout);
			continueExecution = 0;
			return (-1);
		} else {
			//printf("\t\tAccepted block %d\n", firstByte);
		}
		synthRxIndex = (synthRxIndex + 1) % 256;
		synthRxIndex_1024 = (synthRxIndex_1024 + 1) % 1024;
		assert(synthRxIndex >= 0);
		assert(synthRxIndex_1024 >= 0);
		assert((synthRxIndex +1)%256 == synthRxIndex_1024 % 256);
	} else {
		int r = write(wfd, buf, n);
		if (r < 0) {
			if (errno != EAGAIN) {
				perror("Error writing in the output buffer (console)");
				write_err = 2;
				pause();
				return -1;
			}
		} else {
			buf += r;
			n -= r;
		}
	}
	assert(acceptedApplicationBytes >= 0);
	acceptedApplicationBytes += _n;
	return n;
}

int READ_DATA_FROM_APP_LAYER(void *buf, size_t _n) {
	int r, t;
	int n;
	
	n = _n;

	if (syntheticTraffic) {
		assert((synthTxIndex +1)%256 == synthTxIndex_1024 % 256);
		if (n < synthDataBlock) {
			printf("Error: receiving buffer is smaller than the application block size. Use a buffer of at least %d bytes in your implementation\n",
					synthDataBlock);
			pause();
			exit(-1);
		}
		memset(buf, synthTxIndex, n);
		r = n;
		DEBUG_SEND(1, "Data block of %d bytes generated", n);
		DEBUG_SEND(1, "Block index: %d (%d)", synthTxIndex_1024-1, synthTxIndex);
		//printf("Block %d generated\n", synthTxIndex);
		synthTxIndex = (synthTxIndex + 1) % 256;
		synthTxIndex_1024 = (synthTxIndex_1024 + 1) % 1024;
		assert(synthTxIndex>=0);
		assert((synthTxIndex +1)%256 == synthTxIndex_1024 % 256);
	} else {
		if (read_eof)
			return -1;
		r = read(rfd, buf, n);
		if (r == 0 || (r < 0 && errno != EAGAIN)) {
			if (r == 0)
				errno = EIO;
			r = -1;
			read_eof = 1;
			return r;
		}
		if (r < 0 && errno == EAGAIN)
			r = 0;

		if (r > 0 && log_in >= 0)
			t = write(log_in, buf, r);

		xoff = 0;
		cevents[rpoll].events |= POLLIN;
	}
	assert(generatedApplicationBytes >= 0);
	if (generatedApplicationBytes == 0) {	//First packet! Start the timer for stats
		clock_gettime(CLOCK_MONOTONIC, &startTxTime);
	}
	generatedApplicationBytes += r;
	return r;

}

/*
 * Sets the timer timerIndex to expire in delay_in_ns ns.
 * If the timer is already set, it is overwritten.
 * Return value:
 * -1 if the timer was not set
 * Otherwise, the remaining time to expire (in ns) with the previous set
 */
long SET_TIMER(int timerIndex, long delay_in_ns) {
	struct timespec currentTime, timerNewTime, timerOldTime;

	clock_gettime(CLOCK_MONOTONIC, &currentTime);
	DEBUG_TIMER(1, "TIMER SET to expire in %ld ns", delay_in_ns);
	DEBUG_TIMER(2, "Current time: %d s %ld ns", (int) currentTime.tv_sec, currentTime.tv_nsec);
	timerNewTime.tv_sec = currentTime.tv_sec;
	timerNewTime.tv_nsec = currentTime.tv_nsec + delay_in_ns;
	if (timerNewTime.tv_nsec > 1000000000) {
		timerNewTime.tv_sec += timerNewTime.tv_nsec / 1000000000;
		timerNewTime.tv_nsec %= 1000000000;
	}
	timerOldTime = timerExpirationDate[timerIndex];
	timerExpirationDate[timerIndex] = timerNewTime;
	DEBUG_TIMER(2, "Expiration time: %d s %ld ns", (int) timerNewTime.tv_sec, timerNewTime.tv_nsec);
	if (timerSet[timerIndex]) { //The timer was already set!
		assert(activeTimerCount > 0);
		assert(activeTimerCount <= TIMER_COUNT);
		return compareDates(timerOldTime, currentTime);
	} else { //The timer was not set!
		assert(activeTimerCount < TIMER_COUNT);
		activeTimerCount++;
		timerSet[timerIndex] = 1;
		return -1;
	}
}

/*
 * This function clears the timer with index timerIndex
 * It returns -1 if the timer wasn't set; otherwise, it
 * returns the time remaining for the expiration date, in ns
 */
long CLEAR_TIMER(int timerIndex) {
	struct timespec currentTime;

	if (activeTimerCount == 0)
		return -1;
	assert(activeTimerCount > 0);
	if (timerSet[timerIndex]) {
		timerSet[timerIndex] = 0; //Unset the timer!
		DEBUG_TIMER(2, "Timer %d cleared", timerIndex);
		activeTimerCount--;
		clock_gettime(CLOCK_MONOTONIC, &currentTime);
		return (compareDates(timerExpirationDate[timerIndex], currentTime));
	} else {
		return -1;
	}
}

int VALIDATE_CHECKSUM(const packet_t *pkt) {
	if (pkt->cksum == 1) { //Correct packet
		DEBUG_ERRORS(2, "Packet checksum validation: OK");
	} else if (pkt->cksum == -1) {	//Packet with errors
		DEBUG_ERRORS(2, "Packet checksum validation: FAILS!!");
	} else {
		//printf("Received packet with checksum %d\n", pkt->cksum);
		//fflush(stdout);
		//continueExecution = 0;
	}
	return pkt->cksum;
}

void PAUSE_TRANSMISSION() {
	DEBUG_SEND(1, "Transmission paused");
	pausedTransmission = 1;
}
void RESUME_TRANSMISSION() {
	DEBUG_SEND(1, "Transmission resumed");
	pausedTransmission = 0;
}

static void conn_mkevents(void) {
	struct pollfd *e;
	int *r;
	size_t n = 2;
	int i;

	//printf("conn_mkevents\n");
	if (read_eof) {		//The input connection (stdin) does not work!!
		rpoll = 0;
	} else {
		rpoll = n++;
	}
	npoll = n++;
	//printf("Pollfd size n=%d\n", n);

	e = xmalloc(n * sizeof(*e));
	memset(e, 0, n * sizeof(*e));
	if (cevents) {
		//If the communication events had been defined, reuse the previous cevents[0] value
		//cevents[0] seems to include the connections whose data is finished
		e[0] = cevents[0];
	} else {
		//Define a null file descriptor in e[0]
		e[0].fd = -1;
	}
	e[1].fd = 2; /* Do catch errors on stderr */

	//if (syntheticTraffic == 0) {		//Employ the console as the input/output
	if (rpoll) { //Capture read events in the connection identified by rfd, which is
		e[rpoll].fd = rfd; //e[3]
		//printf("\tC rpoll = %d  ---- cevents[%d].fd=%d\n", rpoll, rpoll, rfd);
		if (!xoff)
			e[rpoll].events |= POLLIN;
	}
	//}

	if (npoll) {
		//printf("\tC npoll = %d ---- cevents[%d].fd=%d\n", npoll, npoll, nfd);
		e[npoll].fd = nfd; //e[4]
		e[npoll].events |= POLLIN;
	}

	r = xmalloc(n * sizeof(*r));
	memset(r, 0, n * sizeof(*r));
	if (rpoll > 0)
		r[rpoll] = 1;
	if (npoll > 0)
		r[npoll] = 1;

	free(cevents);
	free(evreaders);
	cevents = e; //Events
	ncevents = n; //Network
	evreaders = r; //Read

	netPolling.fd=nfd;
	netPolling.events = POLLOUT;

}

void generateSyntheticData() {
	if (pausedTransmission)
		return;
	send_callback();	//The application is always ready to generate a flow of data!!
}

void check_events() {
	int n, i, j;
	//static int last_cg;

	//if (last_cg != cevents_generation) {
	//	conn_mkevents();
	//	cevents_generation = last_cg;
	//}

	if (cevents[0].fd >= 0) {
		n = poll(cevents, ncevents, 0);
	} else {
		n = poll(cevents + 1, ncevents - 1, 0);
	}

	for (i = 1; i < ncevents; i++) {
		if (cevents[i].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (evreaders[i]) {
				if ((cevents[i].fd == rfd) && (!pausedTransmission)) {
					xoff = 1;
					cevents[i].events &= ~POLLIN;
					if (syntheticTraffic) {
						synthTrStart = 1;
					} else {
						send_callback();
					}
				} else if (cevents[i].fd == nfd && (cevents[i].revents & (POLLERR | POLLHUP))) {
					char addr[NI_MAXHOST] = "unknown";
					char port[NI_MAXSERV] = "unknown";
					getnameinfo((const struct sockaddr *) &peer, sizeof(peer), addr, sizeof(addr), port, sizeof(port),
							NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV);
					fprintf(stderr, "[received ICMP port unreachable;"
							" assuming peer at %s:%s is dead]\n", addr, port);
					exit(1);
				} else if (cevents[i].fd == nfd) {
					packet_t pkt;
					//printf("Packet received!!! \n");
					int len = debug_recv(nfd, &pkt, sizeof(pkt), 0, NULL );
					if (len < 0) {
						if (errno != EAGAIN) {
							perror("recv");
							pause();
						}
					} else {
						if (len != pkt.len) { //Packet was received incomplete. Corrupt!!!
							pkt.cksum = 0;	//Simple model!!! 1: checksum OK; 0: checksum fails!!
							pkt.len = rand() % 516;
							pkt.seqno = rand() % 1024;
							pkt.type = rand() % 1024;
							if (len > 10)
								for (i = 0; i < (len - 10); i++)
									pkt.data[i] = rand() % 256;

						}
						DEBUG_RECEPTION(1, "Packet received");
						if (syntheticTraffic && pkt.type == DATA) {
							DEBUG_RECEPTION(2, "Bytes received: %d, Length field: %d, SEQ index: %d, ACK index: %d, block %d\n",
									len, pkt.len, pkt.seqno, pkt.ackno, (unsigned char) pkt.data[0]);
						} else {
							DEBUG_RECEPTION(2, "Bytes received: %d, Length field: %d, SEQ index: %d, ACK index: %d",
									len, pkt.len, pkt.seqno, pkt.ackno);
						}
						assert(receivedPackets >=0);
						if (receivedPackets == 0) {	//First received packet!! Start the reception timer!!
							clock_gettime(CLOCK_MONOTONIC, &startRxTime);
						}
						receivedPackets++;
						if (pkt.cksum == 1) {
							DEBUG_ERRORS(2, "Received packet is correct (checksum OK)");
							assert(receivedCorrectPackets >= 0);
							receivedCorrectPackets++;
						} else {
							DEBUG_ERRORS(1, "Received packet is corrupted (checksum fails!)");
							assert(receivedCorruptPackets >= 0);
							receivedCorruptPackets++;
						}
						receive_callback(&pkt, len);
						//memset(&pkt, 0xc9, len); /* for debugging */
					}
				}
			} else {
				perror("evreaders");
				pause();
				exit(1);
			}
		}
		//if ((cevents[i].revents & (POLLOUT | POLLHUP | POLLERR))	&& evwriters[i])
		//	conn_drain(evwriters[i]);
		if (cevents[i].revents & (POLLHUP | POLLERR)) {
			/* If stderr has an error, the tester has probably died, so exit
			 * immediately. */
			if (cevents[i].fd == 2) {
				perror("POLLHUP | POLLERR");
				pause();
				exit(1);
			}
			cevents[i].fd = -1;
		}
		cevents[i].revents = 0;
	}
}

uint16_t cksum(const void *_data, int len) {
	const uint8_t *data = _data;
	uint32_t sum;

	for (sum = 0; len >= 2; data += 2, len -= 2)
		sum += data[0] << 8 | data[1];
	if (len > 0)
		sum += data[0] << 8;
	while (sum > 0xffff)
		sum = (sum >> 16) + (sum & 0xffff);
	sum = htons(~sum);
	return sum ? sum : 0xffff;
}

int make_async(int s) {
	int n;
	if ((n = fcntl(s, F_GETFL)) < 0 || fcntl(s, F_SETFL, n | O_NONBLOCK) < 0)
		return -1;
	return 0;
}

int addreq(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
	if (a->ss_family != b->ss_family)
		return 0;
	switch (a->ss_family) {
	case AF_INET: {
		const struct sockaddr_in *aa = (const struct sockaddr_in *) a;
		const struct sockaddr_in *bb = (const struct sockaddr_in *) b;
		return (aa->sin_addr.s_addr == bb->sin_addr.s_addr && aa->sin_port == bb->sin_port);
	}
	case AF_INET6: {
		const struct sockaddr_in6 *aa = (const struct sockaddr_in6 *) a;
		const struct sockaddr_in6 *bb = (const struct sockaddr_in6 *) b;
		return (!memcmp(&aa->sin6_addr, &bb->sin6_addr, sizeof(aa->sin6_addr)) && aa->sin6_port == bb->sin6_port);
	}
	case AF_UNIX: {
		const struct sockaddr_un *aa = (const struct sockaddr_un *) a;
		const struct sockaddr_un *bb = (const struct sockaddr_un *) b;
		return !strcmp(aa->sun_path, bb->sun_path);
	}
	}
	fprintf(stderr, "addrhash: unknown address family %d\n", a->ss_family);
	abort();
}

size_t addrsize(const struct sockaddr_storage *ss) {
	switch (ss->ss_family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
	case AF_UNIX:
		return sizeof(struct sockaddr_un);
	}
	fprintf(stderr, "addrsize: unknown address family %d\n", ss->ss_family);
	abort();
}

int get_address(struct sockaddr_storage *ss, int local, int dgram, int family, char *name) {
	struct addrinfo hints;
	struct addrinfo *ai;
	int err;
	char *host, *port;

	memset(ss, 0, sizeof(*ss));

	if (family == AF_UNIX) {
		size_t len = strlen(name);
		struct sockaddr_un *sun = (struct sockaddr_un *) ss;
		if (offsetof (struct sockaddr_un, sun_path[len]) >= sizeof(struct sockaddr_storage)) {
			fprintf(stderr, "%s: name too long\n", name);
			return -1;
		}
		sun->sun_family = AF_UNIX;
		strcpy(sun->sun_path, name);
		return 0;
	}

	assert(family == AF_UNSPEC || family == AF_INET || family || AF_INET6);

	if (name) {
		host = strsep(&name, ":");
		port = strsep(&name, ":");
		if (!port) {
			port = host;
			host = NULL;
		}
	} else {
		host = NULL;
		port = "0";
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = dgram ? SOCK_DGRAM : SOCK_STREAM;

	if (local)
		hints.ai_flags = AI_PASSIVE; /* passive means for local address */
	err = getaddrinfo(host, port, &hints, &ai);
	if (err) {
		if (local)
			fprintf(stderr, "local port %s: %s\n", port, gai_strerror(err));
		else
			fprintf(stderr, "%s:%s: %s\n", host ? host : "localhost", port, gai_strerror(err));
		return -1;
	}

	assert(ai->ai_addrlen <= sizeof(*ss));
	memcpy(ss, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	return 0;
}

int listen_on(int dgram, struct sockaddr_storage *ss) {
	int type = dgram ? SOCK_DGRAM : SOCK_STREAM;
	int s = socket(ss->ss_family, type, 0);
	int n = 1;
	socklen_t len;
	int err;
	char portname[NI_MAXSERV];

	if (s < 0) {
		perror("socket");
		return -1;
	}
	if (!dgram)
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &n, sizeof(n));
	if (bind(s, (const struct sockaddr *) ss, addrsize(ss)) < 0) {
		perror("bind");
		close(s);
		return -1;
	}
	if (!dgram && listen(s, 5) < 0) {
		perror("listen");
		close(s);
		return -1;
	}

	if (ss->ss_family == AF_UNIX) {
		fprintf(stderr, "[listening on %s]\n", ((struct sockaddr_un *) ss)->sun_path);
		return s;
	}

	/* If bound port 0, kernel selectec port, so we need to read it back. */
	len = sizeof(*ss);
	if (getsockname(s, (struct sockaddr *) ss, &len) < 0) {
		perror("getsockname");
		close(s);
		return -1;
	}
	err = getnameinfo((struct sockaddr *) ss, len, NULL, 0, portname, sizeof(portname), (dgram ? NI_DGRAM : 0) | NI_NUMERICSERV);
	if (err) {
		fprintf(stderr, "%s\n", gai_strerror(err));
		close(s);
		return -1;
	}

	fprintf(stderr, "[listening on %s port %s]\n", dgram ? "UDP" : "TCP", portname);
	return s;
}

int connect_to(int dgram, const struct sockaddr_storage *ss) {
	int type = dgram ? SOCK_DGRAM : SOCK_STREAM;
	int s = socket(ss->ss_family, type, 0);
	if (s < 0) {
		perror("socket");
		return -1;
	}
	make_async(s);
	if (connect(s, (struct sockaddr *) ss, addrsize(ss)) < 0 && errno != EINPROGRESS) {
		perror("connect");
		close(s);
		return -1;
	}

	return s;
}

static int debug_recv(int s, packet_t *buf, size_t len, int flags, struct sockaddr_storage *from) {
	socklen_t socklen = sizeof(*from);
	int n;
	if (from)
		n = recvfrom(s, buf, len, flags, (struct sockaddr *) from, &socklen);
	else
		n = recv(s, buf, len, flags);
	if (opt_debug > 3)
		print_pkt(buf, "recv", n);
	return n;
}

void initialize_timers() {
	int i;
	activeTimerCount = 0;
	for (i = 0; i < TIMER_COUNT; i++) {
		timerSet[i] = 0;
		//Note that timerExpirationDate[i] never needs to be set, since its value is only valid when timerSet[i] > 0
	}
}

/*
 * Returns the difference of time between the two dates provided, in nanoseconds
 * Return 0: Both dates are the same
 * Positive return value: The second  0, 1 or -1 depending on the two dates provided
 */
int compareDates(struct timespec time1, struct timespec time2) {
	return (time1.tv_sec - time2.tv_sec) * 1000000000 + (time1.tv_nsec - time2.tv_nsec);

	if (time1.tv_sec != time2.tv_sec) {
		return 1;
	} else if (time1.tv_sec < time2.tv_sec) {
		return -1;
	} else {
		assert(time1.tv_sec == time2.tv_sec);
		if (time1.tv_nsec > time2.tv_nsec) {
			return 1;
		} else if (time1.tv_nsec < time2.tv_nsec) {
			return -1;
		} else {
			assert(time1.tv_nsec == time2.tv_nsec);
			return 0;
		}
	}
}

float diffDatesSeconds(struct timespec time1, struct timespec time2) {
	float seconds;
	seconds = (float) (time1.tv_sec - time2.tv_sec) + (time1.tv_nsec - time2.tv_nsec) / 1e9;
	return seconds;
}

/*
 * Checks if any of the active timers in the system has expired, and calls the corresponding callback
 */
void check_timers() {
	int i;
	struct timespec current_time;

	if (activeTimerCount > 0) {
		clock_gettime(CLOCK_MONOTONIC, &current_time);
		for (i = 0; i < TIMER_COUNT; i++) {
			if (timerSet[i]) {
				if (compareDates(current_time, timerExpirationDate[i]) > 0) {		//The timer has expired!
					timerSet[i] = 0; //clear the timer, it is expiring now!
					activeTimerCount--;
					assert(activeTimerCount>=0);
					DEBUG_TIMER(1, "Timer %d expires", i);
					timer_callback(i);
				}
			}
		}
	}
}

void print_stats() {
	struct timespec current_time;
	float TxSpeed, RxSpeed, TxTime, RxTime;

	if (printedStats || receivedPackets || generatedApplicationBytes) {	//At least one timers is started!!
		clock_gettime(CLOCK_MONOTONIC, &current_time);
	} else {
		return;	//We have no stats to print yet
	}
	//diffDatesSeconds(struct timespec time1, struct timespec time2) {
	if (printedStats && diffDatesSeconds(current_time, lastStatPrintTime) < 10) {
		return;
	}
	//Soooo... it's time to print stats!
	TxTime = diffDatesSeconds(current_time, startTxTime);
	if (generatedApplicationBytes && TxTime > 10) {

		printf("\n\tTX STATS: Packets: %ld, Bytes: %lld, Aver. speed: ", sentPackets, sentBytes);
		TxSpeed = 8.0 * sentBytes / TxTime;
		if (TxSpeed <=10000){	// < 10 kbps
			printf(" %.2f bps\n", TxSpeed);
		} else if (TxSpeed <= 1000000){ // < 1 Mbps
			printf(" %.2f kbps\n", TxSpeed/1000.0);
		} else{
			printf(" %.2f Mbps\n", TxSpeed/1000000.0);
		}
	}
	RxTime = diffDatesSeconds(current_time, startRxTime);
	if (receivedPackets && RxTime > 10) {
		printf("\tRX STATS: Packets: %ld (%.1f%% corrupt), App. bytes: %lld, Aver. speed (app. level): ", receivedPackets,
				receivedCorruptPackets * 100.0 / receivedPackets, acceptedApplicationBytes);
		RxSpeed = 8.0 * acceptedApplicationBytes / RxTime;
		if (RxSpeed <=10000){	// < 10 kbps
			printf(" %.2f bps\n", RxSpeed);
		} else if (RxSpeed <= 1000000){ // < 1 Mbps
			printf(" %.2f kbps\n", RxSpeed/1000.0);
		} else{
			printf(" %.2f Mbps\n", RxSpeed/1000000.0);
		}
	}
	printedStats = 1;
	lastStatPrintTime.tv_nsec = current_time.tv_nsec;
	lastStatPrintTime.tv_sec = current_time.tv_sec;
}

static void usage(void) {
	fprintf(stderr, "usage: %s listening-udp-port [host:]destination-udp-port [options]\n", progname);
	fprintf(stderr, "\tOptions:\t-e XX: probability of packet corruption of XX%% (default: 0%%)\n");
	fprintf(stderr, "\t\t\t-w XX: Define a window of XX frames, which is passed to connection_initialization (default: 1 frame)\n");
	fprintf(stderr, "\t\t\t-t XX: Define a timeout of XX nanoseconds, which is passed to connection_initialization (default: 10000000 ns, 10ms)\n");
	fprintf(stderr, "\t\t\t-s: Use synthetic traffic, instead of the console, for input-output (default: console)\n");
	fprintf(stderr, "\t\t\t-d XX: Print debug messages, with verbosity XX (possible values 1 to 3)\n");
	exit(1);
}

int main(int argc, char **argv) {
	struct option o[] = { { "debug", no_argument, NULL, 'd' }, { "unix", no_argument, NULL, 'u' }, { "window", required_argument, NULL, 'w' }, { NULL,
			0, NULL, 0 } };
	int opt;
	int opt_unix = 0;
	char *local = NULL;
	char *remote = NULL;
	struct sockaddr_storage ss;
	struct sigaction sa;

	/* Ignore SIGPIPE, since we may get a lot of these */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL );

	memset(&c, 0, sizeof(c));
	c.window = 1;
	c.timeout = 10000000;	//default timer:10 ms
	syntheticTraffic = 0;
	synthTrStart = 0;
	synthDataBlock = 500;
	pausedTransmission = 0;

	progname = strrchr(argv[0], '/');
	if (progname)
		progname++;
	else
		progname = argv[0];

	while ((opt = getopt_long(argc, argv, "cd:ust:be:w:l", o, NULL )) != -1) {
		switch (opt) {
		case 'd':
			opt_debug = atoi(optarg);
			break;
		case 'l': {
			char name[40];
			snprintf(name, sizeof(name), "%d.in.log", (int) getpid());
			log_in = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0666);
			if (log_in < 0)
				perror(name);
			snprintf(name, sizeof(name), "%d.out.log", (int) getpid());
			log_out = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0666);
			if (log_out < 0)
				perror(name);
		}
			break;
		case 'u':
			opt_unix = 1;
			break;
		case 'w':
			c.window = atoi(optarg);
			break;
		case 't':
			c.timeout = atoi(optarg);
			break;
		case 'e':
			c.probError = atof(optarg) / 100.0;
			break;
		case 's':
			syntheticTraffic = 1;
			synthTxIndex = 1;
			synthTxIndex_1024 = 2;
			synthRxIndex = 1;
			synthRxIndex_1024 = 2;
			break;
		case 'b':
			synthDataBlock = atoi(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (optind + 2 != argc || c.window < 1 || c.timeout < 10) {
		usage();
	}

	local = argv[optind];
	remote = argv[optind + 1];

	struct sockaddr_storage sl, sr;

	if (get_address(&sr, 0, 1, AF_INET, remote) < 0 || get_address(&sl, 1, 1, sr.ss_family, local) < 0 || (nfd = listen_on(1, &sl)) < 0)
		exit(1);
	if (connect(nfd, (struct sockaddr *) &sr, addrsize(&sr)) < 0) {
		perror("connect");
		exit(1);
	}
	peer = sr;

	rfd = 0; //read file descriptor 0: stdin
	wfd = 1; //write file descriptor 1: stdout
	make_async(rfd);
	make_async(wfd);
	make_async(nfd);
	setbuf(stdout, NULL );

	initialize_timers();
	srand(time(NULL ));	//Random number generator initialization
	packet_ptr = xmalloc(sizeof(packet_t));
	memset(packet_ptr, 0, sizeof(packet_t));
	corruptedPacket = xmalloc(sizeof(packet_t));
	memset(corruptedPacket, 0, sizeof(packet_t));
	corruptedPacket->cksum = 0;	//Redundant...

//Stats
	receivedPackets = receivedCorrectPackets = receivedCorruptPackets = 0;
	sentPackets = sentCorrectPackets = sentCorruptPackets = 0;
	generatedApplicationBytes = acceptedApplicationBytes = 0;
	sentBytes = sentCorrectBytes = sentCorruptBytes = 0;	//Overall: Headers + application, including correct and corrupt packets
	printedStats = 0;

	connection_initialization(c.window, c.timeout);
	conn_mkevents();
	if (syntheticTraffic) {
		printf("Press enter to start the transmission of data (the other end must be ready)\n\n");
	}
	continueExecution = 1;

	while (continueExecution) {
		check_events();
		if (syntheticTraffic && !pausedTransmission && synthTrStart)
			generateSyntheticData();
		check_timers();
		sched_yield();
		print_stats();
	}
	printf("Application finished!\n");
	fflush(stdout);
	fflush(stderr);

	return 0;
}
