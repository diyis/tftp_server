#include <unistd.h>     //llamadas al sistema
#include <sys/types.h>  //tipos de dato *_t para el Sistema Operativo
#include <fcntl.h>      //constantes tipo O_*
#include <sys/stat.h>   //información sobre atributos de archivos
#include <sys/time.h>   //funciones de tiempo
#include <sys/socket.h> //socket
#include <arpa/inet.h>  //funciones usadas para internet
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define OPCODE_RRQ      1
#define OPCODE_WRQ      2
#define OPCODE_DATA     3
#define OPCODE_ACK      4
#define OPCODE_ERROR    5

#define DEF_RETRIES      6
#define DEF_TIMEOUT_SEC  0
#define DEF_TIMEOUT_USEC 1000000
#define BUFSIZE          512
#define MAX_BUFSIZE     (4 + BUFSIZE)

#define MODE_OCTET      "octet"
#define MODE_NETASCII   "netascii"

#define ERR_NOT_DEFINED     0
#define ERR_NOT_FOUND       1
#define ERR_ACCESS_DENIED   2
#define ERR_DISK_FULL       3
#define ERR_ILLEGAL_OP      4
#define ERR_UNKNOWN_TID     5
#define ERR_FILE_EXISTS     6
#define ERR_NO_SUCH_USER    7
#define ERR_NOT_ERR         8 //no debería estar

#define STATE_STANDBY       0
#define STATE_DATA_SENT     1
#define STATE_ACK_SENT      2

#define SENT_READY       0
#define SENT_SENDING     1

#define ACK_READY       0
#define ACK_SENDING     1

typedef struct tftp {

    int                  local_descriptor;    /* descriptor de socket local */
    int                  fd;                  /* descriptor de archivo */
    uint16_t             state;               /* estado */
    uint16_t             tid;                 /* id de transferencia */
    uint16_t             err;                 /* tipo de error */
    uint16_t             option;              /* opción de lectura y escritura ???? */
    int32_t              blknum;              /* numero de bloque */
    char                 *msjerr;             /*  msj de error  */
    char                 *mode;               /* modo de transferencia */
    char                 *file;               /* nombre del archivo */
    struct timeval       now;                 /* temporizador inicial */
    struct timeval       timer;               /* temporizador final */
    struct sockaddr_in   remote_addr;              /* estructura remota */
    struct sockaddr_in   local_addr;               /* estructura local */
    socklen_t            size_remote;         /* tamaño estructura remota */
    socklen_t            size_local;            /* tamaño estructura local */
    u_char               lastack[MAX_BUFSIZE];  /* arreglo último ack */
    u_char               msj[BUFSIZE];          /* tamaño max msj a enviar */
    u_char               buf[MAX_BUFSIZE];      /* tamaño max msj a recibir */

} tftp_t;

typedef struct tftp_listen {

    int                  descriptor;           /* descriptor de socket escucha */
    uint16_t             state;                /* estado */
    char                 *mode;                /* modo de transferencia */
    struct sockaddr_in   addr;                 /* estructura escucha */
    struct sockaddr_in   remote_addr;          /* estructura remota */
    socklen_t            size;                 /* tamaño estructura escucha */
    socklen_t            remote_size;                 /* tamaño estructura escucha */
    u_char               buf[MAX_BUFSIZE];     /* tamaño max msj a recibir */

} tftp_tl;

void initialize_server( tftp_t * instance ) {

    u_char *p;
    p = instance->buf;
    p++;
    p++;
    instance->file = strtok(p,"\0");
    instance->mode = strtok(NULL,"\0");
    instance->err = NULL;

}

void wait_request(tftp_tl * listen) {

    int received;
    tftp_t * instance;
    pid_t childPid;

    /* limpiamos el buffer */
    memset( listen->buf, 0, MAX_BUFSIZE );

    received = recvfrom( listen->descriptor, listen->buf, MAX_BUFSIZE, MSG_DONTWAIT, (struct sockaddr *) listen->remote, instance->size_remote );

    if( received != -1 ) {

        switch ( listen->buf[0] << 8 + listen->buf[1] ) {

        case OPCODE_RRQ:
            listen->state = STATE_DATA_SENT;
            break;
        case OPCODE_WRQ:
            listen->state = STATE_ACK_SENT;
            break;
        default:
            listen->state = STATE_STANDBY;

        }
    }

    if ( received == OPCODE_RRQ || received == OPCODE_WRQ ) {

        switch ( childPid = fork() ) {

        case -1://error
            perror("fork");
            //falta qué hacer en caso de error
            break;
        case 0://hijo

            /* Alojamos memoria dinamicamente */
            instance = malloc( sizeof( struct tftp ) );
            //revisar si malloc ha fallado

            /* Asignamos el valor 0 a los elementos de las estructura sockaddr_in */

            memset( instance->local_addr, 0, sizeof( struct sockaddr_in ) );
            memset( instance->remote_addr, 0, sizeof( struct sockaddr_in ) );

            /* Copiamos el buffer de listen en el buffer de instance */
            memcpy( instance->buf, listen->buf, MAX_BUFSIZE );

            /* Asociamos los descriptor del socket */

            instance->local_descriptor = socket( AF_INET, SOCK_DGRAM, 0 );

            /* Verificamos que no haya habido errores con la asociación del socket */

            if ( instance->local_descriptor == -1 )
                perror("instance local socket");

            /* Asigmanos datos del socket local */

            instance->local_addr.sin_family = AF_INET;
            instance->local_addr.sin_addr.s_addr = INADDR_ANY;

            /* Asigmanos datos del socket remoto; el puerto de destino debe ser el mismo de donde recibimos la petición */

            instance->remote_addr.sin_family = AF_INET;
            instance->remote_addr.sin_addr.s_addr = INADDR_ANY;
            instance->remote_addr.sin_port = listen->remote_addr.sin_port;

            /* Guardamos el puerto a donde debemos enviar los datos */

            instance->tid = ntohs( instance->remote_addr.sin_port );

            /* Asociamos el descriptor con el socket que queremos usar */

            if ( bind( instance->local_descriptor, (struct sockaddr *) instance->local_addr, sizeof(struct sockaddr_in))  == -1 )
                perror("bind local descriptor");

            /* Termianos de inicializar la estructura instance */

            initialize_server( instance );

        default://padre
            wait_request( listen );

        }
    }
}

int main ( int argc, char *argv[] ) {

    tftp_tl listen;

    /* Asignamos el valor 0 a los elementos de la estructura sockaddr_in */

    memset( &listen.addr, 0, sizeof( struct sockaddr_in ) );

    listen.descriptor = socket(AF_INET, SOCK_DGRAM, 0);

    if ( listen.descriptor == -1 )
        perror("listen socket");

    /* Asignamos datos del socket escucha */

    listen.addr.sin_family = AF_INET;
    listen.addr.sin_addr.s_addr = INADDR_ANY;
    listen.addr.sin_port = htons(69);

    /* Asociamos el descriptor con el socket que queremos usar */
    if ( bind( listen.descriptor, (struct sockaddr *) &listen.addr, sizeof(struct sockaddr_in))  == -1 )
        perror("bind listen");

    wait_request(&listen);

    return EXIT_SUCCESS;
}
