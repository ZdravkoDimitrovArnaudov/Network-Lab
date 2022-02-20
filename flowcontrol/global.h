#ifndef _GLOBALH_
#define _GLOBALH_

/*
 * SECTION 1: SYSTEM DESCRIPTION
 * ------------------------

 There are two kinds of packets, Data packets and Ack-only packets.
 There is a type field which differentiates. Ack packets are 8
 bytes, while Data packets vary from 10 to 510 bytes.

 Every Data packet contains a sequence number as well as 0 or
 more bytes of payload.

 Both Data and Ack packets contain the following fields.

 - cksum: 16-bit checksum. This checksum is automatically calculated
 for each packet transmitted. However, when the packet is received, 
 you have to validate if the frame is corrupt with the VALIDATE_CHECKSUM
 function.

 - len: 16-bit total length of the packet.  This will be 8 for Ack
 packets, and 10 + payload-size for data packets (since 10
 bytes are used for the header of data packets).

 - ackno: acknowledgement index. The meaning of this index is up to the
 programmer. Essentially, there are two different options:
 	 · Confirms the reception of the frame with index "ackno".
 	 · Comfirms the reception of the frames previous to "ackno".
 	   and that the system is waiting for the frame "ackno".
 Note that data packets do not need to use this value.

 The following fields only exist in a data packet:

 - seqno: Each packet transmitted in a stream of data must be numbered
 with a sequence number, "seqno". The first packet in a stream has
 seqno 1.

 - data:  Contains (len - 10) bytes of payload data for the
 application.


-----------------------------------------------------------------------


 Important notes about the system:

 * The program, once completed, models a link with flow-control between to
 endpoints (different PCs). Real messages are sent between these PCs over the
 network. These messages are sent over UDP, IP (and Ethernet); however, this is
 not visible to the programmer.

 * There are two modes of operation. The default mode employs the console for input
 and output of data: when you write something on an endpoint, this string is sent to
 the other endpoint and printed on the console. Alternatively, the runtime can model
 synthetic traffic; you have to use the flag -s to activate this mode. In this model,
 packets of fixed size are generated as fast as possible.

 * You can pass some variables to the program, which are passed to the
 "connection_initialization" function:
 - window:  Tells you the size of the sliding window (which will
 be 1 for stop-and-wait). You can ignore the case of window > 1 (sliding window).
 - timeout: Tells you what your retransmission timer should be, in nanoseconds.
 If after these many nanoseconds a packet you sent has still not been acknowledged,
 you should retransmit the packet. You can use the function SET_TIMER to activate
 a timer. The default timer is 10 ms (10000000 ns).

 * Your task is to implement the following functions:

 "connection_initialization", "recv_callback", "send_callback", "timer_callback"

 as well to describe the connection state data.  All the
 changes you need to make are in the file eventCallbacks.c.
 YOU DO NOT HAVE TO CHANGE ANYTHING IN LIBRARY.C, LIBRARY.H or GLOBAL.H

 * Additionally, there are several API functions that are used to interact
 with the system, such as send or receive packets. The name of these functions
 employs CAPITAL LETTERS. The prototypes of these functions are in this file
 global.h. You do not need to understand the implementation in library.[ch]

 * When a packet is received, the library will call "recv_callback".
To accept the data you have received in a frame, call ACCEPT_DATA.

 * When the application has data to be sent, the library will call
 "send_callback". To get the input data that you must send in your packets,
 use READ_DATA_FROM_APP_LAYER. To transmit a packet, call SEND_ACK_PACKET
 or SEND_DATA_PACKET.

 * When the window closes (you cannot send more frames because you are
 waiting for ACK's) you have to notify the runtime using PAUSE_TRANSMISSION.
 This will pause the data generation, so "send_callback" will not be called.
 When the transmission window opens (when you receive the expected ACK's)
 you have to call RESUME_TRANSMISSION.

 * The program can model frame errors; you can specify the frame corruption
 probability using the -e flag. By default, there is no error corruption (although
 the real-world network that transmits the packets might loss some traffic).
 The frames have a checksum field in the header, which is automatically
 calculated and set by the library. However, you have to manually verify it
 using VALIDATE_CHECKSUM.

 * When a corruption occurs and a data packet does not receive the corresponding
 ACK, it has to be retransmitted. You should have activated a timer for this. There
 are up to 16 different timers (0 to 15) which can be active concurrently.
 Timers are set with SET_TIMER and cleared with CLEAR_TIMER. When a timer expires,
 "timer_callback" is called. The timeout of the timers is defined in nanoseconds;
 you have to compile with -lrt for this to work (this is included in the Makefile).

 * After transmission starts, the application displays statistics every 10 seconds.
 Note that these stats can consider either the "sent bytes" (including data and headers,
 and possibly from a duplicated frame), or "application bytes" (only the accepted data
 fields, without any duplication). Note that the transmission speed (in the sender side)
 and the average speed in the application level (in the receiver side) can differ
 significantly, especially when there are errors.

 * To aid debugging, you can use the -d flag, specifying a verbosity level.
 There are 3 verbosity levels, 1 to 3. When debugging is active, some messages
 are printed regarding the transmission and reception of packets, accepted data in
 the application layer, and errors. Each of these messages is displayed in a
 different colour. You can also print messages from your functions to help
 debugging (use "printf" for that)

 */


/*
 * SECTION 2: DECLARATION OF STRUCTURES
 * -------------------------------------*/


enum packetType {
	ACK, DATA
};
typedef enum packetType packetType_t;

/* Ack-only packets are only 8 bytes */
struct ack_packet {
	int16_t cksum;	//2 bytes
	int16_t len;	//2 bytes
	int16_t type;	//2 bytes
	int16_t ackno;	//2 bytes
};

/* The header of the data packet is 10 bytes */
struct packet {
	int16_t cksum;	//2 bytes
	int16_t len;	//2 bytes
	int16_t type;	//2 bytes
	int16_t ackno;	//2 bytes
	int16_t seqno;  //2 bytes -- Only valid if length > 8 and the type is correct */
	char data[500]; //Up to 500 bytes -- Only valid if length > 8 and the type is correct */
};
typedef struct packet packet_t;



/*
 * SECTION 3: CALLBACKS DECLARATION AND DESCRIPTION
 * ------------------------------------------------*/

/* These are the functions you must provide (in eventcallbacks.c). */

/*
 * "connection_initialization" is called only once at the beginning of
 * the execution. It should initialize all the necessary variables for the protocol.
 * Note that you should define these variables in the upper section of eventCallbacks.c,
 * so they are visible to the four callback functions. Do NOT declare any of these
 * variables inside connection_initialization, in such case they would not be visible
 * from any other function.
 *
 * Input parameters:
 * - nt windowSize: The size of the window, as declared by the -w flag. This window
 * size can be ignored in stop & wait protocol.
 * - long timeout_in_ns: The timeout value to be used in the system. When
 *
 */
void connection_initialization(int windowSize, long timeout_in_ns);
/* "recv_callback" gets called when a packet arrives. The packet can be accessed
 * with the pkt pointer. The length (number of bytes received) is also passed.
 * Note that you should validate the checksum of the packet using the VALIDATE_CHECKSUM
 * API call; if the checksum fails, no field can be trusted (including the "length" field
 * of the packet header) */
void receive_callback(packet_t *pkt, size_t len);
/* "send_callback" gets called when the application has data to be sent. Note that
 * if you call PAUSE_TRANSMISSION this function is never called, until you call
 * RESUME_TRANSMISSION
 */
void send_callback();
/* Invoked when a timer expires */
void timer_callback(int timer_number);


/*
 * SECTION 4: API FUNCTIONS
 * ------------------------*/


/* Read a block of data from the higher level of the protocol stack.
 * (the console or the synthetic traffic generator). This
 * function returns the number of bytes received, 0 if there is no
 * data currently available, and -1 on EOF or error.
 *
 * Parameters:
 * - *buf is a pointer to an area of memory where the data will be copied
 * - len is the maximum amount of data that can be accepted, in bytes.
 *  */
int READ_DATA_FROM_APP_LAYER(void *buf, size_t len);

/* Call SEND_DATA_PACKET to send a complete packet to the other side. You have to
 * provide all the fields in the packet header, and a POINTER to the data stream
 * to be sent. Note that "length" refers to the complete packet length in bytes,
 * including the header fields plus the data field.
 *
 * Parameters:
 * - type: DATA or ACK, as defined in Section 2 of this file.
 * - length: The length of the whole packet (header + data) in bytes.
 * - ackNo: The value to be sent in the ACK field of the data packet.
 * - seqNo: The sequence index to use.
 * - *data: A pointer to the array of data, which should be (length-10) bytes
 *          (the header of data frames is 10 bytes)
 *
 * */
int SEND_DATA_PACKET(packetType_t type, uint16_t length, uint32_t ackNo, uint32_t seqNo, void *data);

/*
 * Call SEND_ACK_PACKET to send an ACK packet to the other side. The packet type and length
 * are implicit, and the sequence number and data fields are omitted.
 *
 * Parameters:
 * - ackNo: The value to be sent in the ACK field of the data packet.
 * */
int SEND_ACK_PACKET(uint32_t ackNo);

/* API functions: You can call these functions from your code to interact with
 * the lower layer (physical: send and receive data) and the upper layer
 * (application: stop/resume transmission, accept data to be sent, pass
 * data that has been correctly received from the network */

/* Call this function to accept the data from the packets you have
 * received.
 * The function returns number of bytes written accepted on
 * success, or -1 if there has been an error.
 *
 * Parameters:
 * - *buf is a pointer to the location of the array of data.
 * - len is the length of the data, in bytes
 *   NOTE: this is not the length of the PACKET, only the DATA)
 */
int ACCEPT_DATA(const void *buf, size_t len);

/*
 * SET_TIMER activates a timer that will expire in timerDelayNsec nanoseconds.
 * There are 16 different timers available, using numbers 0 to 15. You can set
 * multiple timers concurrently, each of them with its own deadline
 */
long SET_TIMER(int timerNumber, long timerDelayNsec);

/*
 * This function clears the timer with index timerIndex
 * It returns -1 if the timer wasn't set; otherwise, it
 * returns the time remaining for the expiration date, in ns
 */
long CLEAR_TIMER(int timerIndex);

/*
 * VALIDATE_CHECKSUM validates the checksum to determine if the packet has been corrupted in transit.
 * 0: The packet is corrupted!!
 * 1: The packet is OK!!
 *
 * Parameters:
 * - *pkt is a POINTER to the packet to be validated
 *
 */
int VALIDATE_CHECKSUM(const packet_t *pkt);

/*
 * Call PAUSE_TRANSMISSION and RESUME_TRANSMISSION to temporarily pause or continue the transmission
 * of data. When PAUSE_TRANSMISSION is called, the upper layer will not generate any more data to be sent
 * (so send_callback is never called), until RESUME_TRANSMISSION is used. You typically need to PAUSE
 * the transmission when the flow control does not allow to send more frames, and RESUME when the
 * corresponding ACKs are received
 */
void PAUSE_TRANSMISSION();
void RESUME_TRANSMISSION();

#endif //_GLOBALH_
