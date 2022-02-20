#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <stdbool.h>

#include "library.h"
#include "global.h"

#define SIZE_BUFFER 500


/*
 * SECTION 1: GLOBAL DATA
 * ----------------------
 * Add your own data fields below this
 *
 */

int window_size;//init
long timeout_ns; //init

int indice_trama = 1; //inicializamos indice de las tramas
int size_inf=0; //tamaño trama(send_callback)

int indice_recibido = 1;
int indice_enviado, indice_ack;

int indice_timer = 0;
bool transmite = false;
char buffer[SIZE_BUFFER]="";



/*
 * SECTION 2: CALLBACK FUNCTIONS
 * -----------------------------
 * The following functions are called on the corresponding event
 *
 */


/*
 * Creates a new connection. You should declare any variable needed in the upper section, DO NOT DECLARE
 * ANY VARIABLES HERE. This function should only make initializations as required.
 *
 * You should save the input parameters in some persistant global variable (defined in Section 1) so it can be
 * accessed later
 */
void connection_initialization(int _windowSize, long timeout_in_ns) {
    window_size = _windowSize;
    timeout_ns = timeout_in_ns;
}

/* This callback is called when a packet pkt of size n is received*/
void receive_callback(packet_t *pkt, size_t n) {
	
	 if (VALIDATE_CHECKSUM(pkt)){ //comprobamos que el dato es correcto
		
		if (pkt->type == DATA) { //es trama de datos
			
			if (indice_recibido != pkt->seqno){ //si la trama recibida es nueva
				
				printf ("Trama %d recibida:\n",pkt->seqno );
				ACCEPT_DATA(pkt->data,n-10) ;//aceptamos datos de la trama
				indice_recibido = pkt->seqno;
							
				//enviamos confirmación ack con el indice correspondiente
				SEND_ACK_PACKET(indice_recibido);
			
			} else {  //si la trama es idéntica
				
				//asumimos que la trama ya está aceptada. En este caso, solo mandamos intento de confirmación.
				printf ("Confirmación ack: %d enviada de nuevo\n", pkt->seqno);
				SEND_ACK_PACKET(indice_recibido);
				
			}
				
			
				
		} else { //hemos recibido ack
			
			CLEAR_TIMER(indice_timer),
			indice_ack = pkt->ackno;
			printf ("Confirmación ACK %d recibida:\n", indice_ack);			
			RESUME_TRANSMISSION(); //reanudamos la transmisión al haber recibido el ack.
			
			
		}
		
	}
	

}

/* Callback called when the application wants to send data to the other end*/
void send_callback() {
	

    //inicializamos texto y lo mostramos por pantalla
    size_inf = READ_DATA_FROM_APP_LAYER(&buffer, SIZE_BUFFER);
    
    //invertimos los índices
    if (indice_trama == 0){
		    indice_trama++;

	  } else {
		    indice_trama--;

	  }
    //enviamos el paquete
    SEND_DATA_PACKET(DATA, size_inf +10, 0, indice_trama, &buffer);
    
    //CLEAR_TIMER(indice_timer); //limpiamos timer
    SET_TIMER(indice_timer, timeout_ns);
    
    PAUSE_TRANSMISSION(); //paramos la transmisión hasta recibir ack
    
    


}

/*
 * This function gets called when timer with index "timerNumber" expires.
 * The function of this timer depends on the protocol programmer
 */
void timer_callback(int timerNumber) {
	
	
	CLEAR_TIMER(indice_timer);
	//necesitamos reenviar la trama de nuevo
	printf ("La trama: %d está corrupta y se vuelve a enviar\n",indice_trama);
	SEND_DATA_PACKET(DATA, size_inf +10, 0, indice_trama, &buffer);
	//volvemos a programar el timer para que vuelva a saltar
	SET_TIMER(indice_timer, timeout_ns);
	
	}


