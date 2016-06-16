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
//#define ERR_NOT_ERR         8 //no debería estar

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

void build_data_msg( tftp_t * instance ) {

    u_char *p;
    memset( &instance->buf, 0, MAX_BUFSIZE );
     p = instance->buf;
    *p = ( OPCODE_DATA >> 8 ) & 0xff;      p++;
    *p =   OPCODE_DATA & 0xff;             p++;
    *p = ( instance->blknum >> 8 ) & 0xff; p++;
    *p = ( instance->blknum ) & 0xff;      p++;
    memcpy( p, instance->msj, BUFSIZE );
}

void build_error( tftp_t * instance) {

    u_char *p;
    memset( &instance->buf, 0, MAX_BUFSIZE );
    p = instance->buf;
    *p = ( OPCODE_ERROR >> 8 )  & 0xff;   p++;
    *p =   OPCODE_ERROR & 0xff;           p++;
    *p = ( instance->err >> 8 ) & 0xff;   p++;
    *p =   instance->err & 0xff;          p++;
    memcpy( p, instance->msjerr, strlen( instance->msjerr ) );

}

void build_ack_msg( tftp_t * instance ) {

    u_char *p;
    memset( instance->buf, 0, MAX_BUFSIZE );
    p = instance->buf;
    *p = ( OPCODE_ACK >> 8 ) & 0xff;        p++;
    *p =   OPCODE_ACK & 0xff;               p++;
    *p = ( instance->blknum >> 8 ) & 0xff;  p++;
    *p =   instance->blknum & 0xff;         p++;
    memset( p, 0, BUFSIZE );

}

void dec_data( tftp_t * instance ) {

    u_char *p;
    p = instance->buf;
    p += 4;
    memcpy( instance->msj, p, BUFSIZE );

}

void initialize_server( tftp_t * instance ) {

    u_char *p;
    p = instance->buf;
    p++;
    p++;
    instance->file = strtok(p,"\0");
    instance->mode = strtok(NULL,"\0");
    instance->err = NULL;

}

void data_send( tftp_t * instance ) {

    int sent,received;
    off_t offset;
    static bool timeout = false;

    if ( timeout ) {

        /* Regresamos 512 bytes por tener que reenviar el último msj */
        lseek( instance->fd, -BUFSIZE, SEEK_CUR );

        /* Leemos 512 bytes del archivo */

        offset = read( instance->fd, instance->msj, BUFSIZE );

        timeout = false;

    }else
        offset = read( instance->fd, instance->msj, BUFSIZE );

    /* Generamos el msj a enviar */

    build_data_msg( instance );

    /* Enviamos el msj */

    sent = sendto( instance->local_descriptor, instance->buf, 4 + offset , 0, (struct sockaddr *) instance->remote, sizeof( struct sockaddr_in ) );

    /* Verificamos que se haya enviado correctamente */

    if( sent != 4 + offset )
        perror("sent ack");

    /* Iniciamos los temporizadores */

    gettimeofday( instance->now, 0 );
    gettimeofday( instance->timer, 0 );

    /* Esperamos el siguiente ack */

    while ( instance->timer.tv_usec - instance->now.tv_usec < DEF_TIMEOUT_USEC ) {

        received = recvfrom( instance->local_descriptor, instance->buf, MAX_BUFSIZE, MSG_DONTWAIT, (struct sockaddr *) instance->remote, instance->size_remote );

        /* Verificamos que haya llegado un msj válido, se debe cumplir: */
        /* 1. Que received sea distinto a -1 */
        /* 2. Que el OPCODE sea OPCODE_ACK */
        /* 3. Que el ack corresponda al blknum que esperamos */
        /* 4. Que el msj sea de donde lo esperamos (mismo tid del inicio de la transferencia) */

        if( received != -1 && ( instance->buf[0] << 8 + instance->buf[1] == OPCODE_ACK ) && ( instance->buf[2] << 8 + instance->buf[3]  == instance->blknum ) && ( instance->tid == ntohs( instance->remote.sin_port ) ) ) {

            /* Si hemos enviado el último msj y recibido el último ack, terminamos */

            if( offset < BUFSIZE ) {

                /* Cerramos el descriptor de archivo y de socket */

                close( instance->fd );
                close( instance->local_descriptor );

                /* Hijo finaliza */

                exit( EXIT_SUCCESS );
            }

            /* Aumentamos blknum y limpiamos los buffers */

            instance->blknum++;
            memset( instance->msj, 0, BUFSIZE );
            memset( instance->buf, 0, MAX_BUFSIZE );

            /* Si el blknum es 65536, le asignamos el valor 0 para no salirnos del rango de 2 bytes de la trama para el campo blknum */

            if ( instance->blknum  == 65536 )
                instance->blknum = 0;

            send_data( instance );

        } //end 4-condition if

        instance->timer.tv_usec++;

    }//end while

    /* Como no hemos recibido el ack correspondiente a la última trama que hemos enviado, ha expirado el tiempo de espera */

    timeout = true;
    send_data( instance );


}
void start_data_send( tftp_t * instance ) {

    /* Comprobamos si hay errores */

    if ( access( instance->file, F_OK ) != 0 ) {

        instance->err = ERR_NOT_FOUND;
        instance->msjerr = "No existe el archivo.";

    }else if ( access( instance->file, R_OK ) != 0 ) {

        instance->err = ERR_ACCESS_DENIED;
        instance->msjerr = "No hay permisos de lectura.";

    }else
        instance->err = NULL;

    /* Si hay errores, enviar msj de error y terminar */

    if ( !instance->err ) {

        build_error( instance );
        sent = sendto( instance->local_descriptor, instance->buf, MAX_BUFSIZE, 0, (struct sockaddr *) instance->remote, sizeof( struct sockaddr_in ) );

        /* Verificamos que hayamos enviado el msj correctamente */

        if ( sent != strlen( instance->buf ) )
            perror("sent child");

        /* Cerramos descriptor de socket */

        close( instance->local_descriptor );

        /* Hijo finaliza */

        exit(EXIT_SUCCESS);

    }

    /* Inicializamos las variables a usar */

    instance->blknum = 1;

    /* Verificamos que el descriptor de archivo se haya abierto correctamente */

    instance->fd = open( instance->file, O_RDONLY );

    if ( instance->fd == -1 ) {

        /* Enviamos msj de error */

        instance->err = "No se puede abrir el archivo para lectura.";

        build_error( instance );
        sent = sendto( instance->local_descriptor, instance->buf, MAX_BUFSIZE, 0, (struct sockaddr *) instance->remote, sizeof( struct sockaddr_in ) );

        /* Verificamos que hayamos enviado el msj correctamente */

        if ( sent != strlen( instance->buf ) )
            perror("sent child");

        /* Cerramos descriptor de socket */

        close( instance->local_descriptor );

        /* Hijo finaliza */

        exit(EXIT_SUCCESS);

    }

    /* Seguimos */

    data_send( instance );

}
void ack_send( tftp_t * instance ) {
    int sent,received;
    off_t offset;

    /* Generamos el ack */

    build_ack_msg( instance );

    /* Enviamos el ack */

    sent = sendto( instance->local_descriptor, instance->buf, 4, 0, (struct sockaddr *) instance->remote, sizeof( struct sockaddr_in ) );

    /* Verificamos que se haya enviado correctamente */

    if( sent != 4 )
        perror("sent ack");

    /* Iniciamos los temporizadores */

    gettimeofday( instance->now, 0 );
    gettimeofday( instance->timer, 0 );

    /* Asignamos -1 a blknum en caso de ser 65535 para evitar un rango incorrecto en los ack */

    if( instance->blknum == 65535 ) instance->blknum = -1;

    /* Esperamos el siguiente msj */

    while ( instance->timer.tv_usec - instance->now.tv_usec < DEF_TIMEOUT_USEC ) {

        received = recvfrom( instance->local_descriptor, instance->buf, MAX_BUFSIZE, MSG_DONTWAIT, (struct sockaddr *) instance->remote, instance->size_remote );

        /* Verificamos que haya llegado un msj válido, se debe cumplir: */
        /* 1. Que received sea distinto a -1 */
        /* 2. Que el OPCODE sea OPCODE_DATA */
        /* 3. Que el blknum sea el que esperamos */
        /* 4. Que el msj sea de donde lo esperamos (mismo tid del inicio de la transferencia) */

        if ( received != -1 && ( instance->buf[0] + instance->buf[1]  == OPCODE_DATA ) && ( instance->buf[2] << 8 + instance->buf[3]  == instance->blknum + 1 ) && instance->tid == ntohs(instance->remote.sin_port) ) {

            /* Procesamos los datos recibidos */
            dec_data( instance );

            /* Verificamos si es el último msj por recibir */

            if( received < MAX_BUFSIZE ) {

                /* Escribimos en el archivo */

                write( instance->file, instance->msj, received-4 );

                /* Generamos el último ack */

                instance->blknum++;
                build_ack_msg( instance );

                /* Enviamos el último msj */

                sent = sendto( instance->local_descriptor, instance->buf, 4, 0, (struct sockaddr *) instance->remote, sizeof( struct sockaddr_in ) );

                /* Verificamos que se haya enviado correctamente */

                if( sent != 4 )
                    perror("sent ack");

                /* Cerramos el descriptor de archivo y de socket */

                close( instance->fd );
                close( instance->local_descriptor );

                /* Hijo finaliza */

                exit(EXIT_SUCCESS);

            }

            /* Escribimos en el archivo */
            offset = write(instance->fd ,instance->msj, BUFSIZE);

            /* Limpiamos buffer y msj */
            memset( instance->buf, 0, MAX_BUFSIZE);
            memset( instance->msj, 0, BUFSIZE);

            /* Incrementamos blknum */
            instance->blknum++;
            ack_send(instance);

        }//end 4-condition if
        instance->timer.tv_usec++;
    }//end while
    ack_send(instance);
}
void start_ack_send( tftp_t * instance ) {
    int sent;

    /* Comprobamos si hay errores */

    if ( access( ".", W_OK ) == 0 )
        instance->err = NULL;
    else {
        instance->err = ERR_ACCESS_DENIED;
        instance->msjerr = "No hay permisos.";
    }

    /* Si hay errores, enviar msj de error y terminar */

    if ( !instance->err ) {

        build_error( instance );
        sent = sendto( instance->local_descriptor, instance->buf, MAX_BUFSIZE, 0, (struct sockaddr *) instance->remote, sizeof( struct sockaddr_in ) );

        /* Verificamos que hayamos enviado el msj correctamente */

        if ( sent != strlen( instance->buf ) )
            perror("sent child");

        /* Cerramos descriptor de socket */

        close( instance->local_descriptor );

        /* Hijo finaliza */

        exit(EXIT_SUCCESS);

    }
    /* Inicializamos las variables a usar */
    instance->blknum = 0;
    instance->fd = open( instance->file, O_WRONLY | O_CREAT | O_TRUNC );
    /* Seguimos */
    ack_send( instance );

}
void child(tftp_tl * listen ) {

    tftp_t * instance;

    /* Cerramos el descriptor del padre que no usaremos en el hijo */
    close(listen->descriptor);

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

    /* Verificamos que no haya errores con la asociación del socket */

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

    /* Terminanos de inicializar la estructura instance */

    initialize_server( instance );

    /* Verificamos que el modo de transferencia sea binario para seguir con la transferencia */

    if ( strcmp(instance->mode, MODE_OCTET) == 0 ) {

        if ( instance->state == STATE_DATA_SENT) start_data_send(instance);
        else start_ack_send(instance);

    } else {
        //enviamos trama con msj de error: "Modo de transferencia no soportado".

        /* El hijo finaliza */

        exit(EXIT_SUCCESS);
    }

}
void wait_request(tftp_tl * listen) {

    int received;
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
            child( listen );
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
