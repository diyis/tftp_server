#include <unistd.h>     //llamadas al sistema
#include <sys/types.h>  //tipos de dato *_t para el Sistema Operativo
#include <fcntl.h>      //constantes tipo O_*
#include <sys/stat.h>   //información sobre atributos de archivos
#include <sys/time.h>   //funciones de tiempo
#include <sys/socket.h> //socket
#include <arpa/inet.h>  //funciones usadas para internet
#include <signal.h>     //señales
#include <syslog.h>     //log del sistema
#include <sys/wait.h>   //
#include <errno.h>
#include <string.h>
#include <stdarg.h>     //Manejar argumentos del tipo " ... "
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define OPCODE_RRQ      1
#define OPCODE_WRQ      2
#define OPCODE_DATA     3
#define OPCODE_ACK      4
#define OPCODE_ERROR    5

#define DEF_RETRIES      20
#define DEF_TIMEOUT_SEC  0
#define DEF_TIMEOUT_USEC 10000000
#define BUFSIZE          512
#define MAX_BUFSIZE     (4 + BUFSIZE)
#define ACK_BUFSIZE      4

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
    int                  retries;
    uint16_t             state;               /* estado */
    uint16_t             tid;                 /* id de transferencia */
    uint16_t             err;                 /* tipo de error */
    int32_t              blknum;              /* numero de bloque */
    char                 *msgerr;             /*  msg de error  */
    char                 *mode;               /* modo de transferencia */
    char                 *file;               /* nombre del archivo */
    struct timeval       now;                 /* temporizador inicial */
    struct timeval       timer;               /* temporizador final */
    struct timeval       timeout;
    struct sockaddr_in   remote_addr;              /* estructura remota */
    struct sockaddr_in   local_addr;               /* estructura local */
    socklen_t            size_remote;         /* tamaño estructura remota */
    socklen_t            size_local;            /* tamaño estructura local */
    u_char               msg[BUFSIZE];          /* tamaño max msg a enviar */
    u_char               buf[MAX_BUFSIZE];      /* tamaño max msg a recibir */

} tftp_t;

typedef struct tftp_listen {

    int                  descriptor;           /* descriptor de socket escucha */
    uint16_t             state;                /* estado */
    char                 *mode;                /* modo de transferencia */
    struct sockaddr_in   addr;                 /* estructura escucha */
    struct sockaddr_in   remote_addr;          /* estructura remota */
    socklen_t            size;                 /* tamaño estructura escucha */
    socklen_t            remote_size;                 /* tamaño estructura escucha */
    u_char               buf[MAX_BUFSIZE];     /* tamaño max msg a recibir */

} tftp_tl;

void err_log_exit( int priority, const char *format, ... ){

    va_list args;

    va_start(args, format);
    vsyslog(priority, format, args );
    va_end(args);

    syslog(LOG_CRIT, "FATAL ERROR. Shutting down server ...");
    _exit(EXIT_FAILURE);
}

void _err_log_exit( int priority, const char *format, ... ){

    va_list args;
    
    va_start(args, format);
    vsyslog(priority, format, args);
    va_end(args);
    
    syslog(LOG_CRIT, "FATAL ERROR. Killing child ...");
    _exit(EXIT_FAILURE);
}

void build_data_msg( tftp_t * instance ) {

    u_char *p;
    memset( &instance->buf, 0, MAX_BUFSIZE );
    p = instance->buf;
    *p = ( OPCODE_DATA >> 8 ) & 0xff;      p++;
    *p =   OPCODE_DATA & 0xff;             p++;
    *p = ( instance->blknum >> 8 ) & 0xff; p++;
    *p = ( instance->blknum ) & 0xff;      p++;
    memcpy( p, instance->msg, BUFSIZE );
}

void build_error( tftp_t * instance) {

    u_char *p;
    memset( &instance->buf, 0, MAX_BUFSIZE );
    p = instance->buf;
    *p = ( OPCODE_ERROR >> 8 )  & 0xff;   p++;
    *p =   OPCODE_ERROR & 0xff;           p++;
    *p = ( instance->err >> 8 ) & 0xff;   p++;
    *p =   instance->err & 0xff;          p++;
    memcpy( p, instance->msgerr, strlen( instance->msgerr ) );

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
    memcpy( instance->msg, p, BUFSIZE );

}

void initialize_server( tftp_t * instance ) {
    u_char *p;
    p = instance->buf;
    p+=2; //PP: Tal vez y sólo tal vez quedaría mejor como p+=2;
    instance->file = strtok(p,"\0");

    /*PP: VALIDACIÓN: Si instance->file es nulo significa que el
      cliente no especificó un nombre de archivo.
      NOTA: Tal vez sería una buena idea enviar un paquetito de error al cliente con
      el código 0 : File not found*/
    
    if( instance->file ) p+= strlen(instance->file) + 1;
    else _err_log_exit( LOG_ERR, "Error from initialize_server() in strtok(file): "
                                 "File name not specified");
    
    instance->mode = strtok(p,"\0");
    instance->msgerr = NULL;
    instance->size_remote = sizeof(struct sockaddr_in);
}

void data_send( tftp_t * instance ) {

    ssize_t sent;
    ssize_t received;
    off_t offset;
    static bool timeout = false;

    
    if ( timeout ) {
        /* Regresamos 512 bytes por tener que reenviar el último msg */
        lseek( instance->fd, -BUFSIZE, SEEK_CUR );

        /* Leemos 512 bytes del archivo */
        offset = read( instance->fd, instance->msg, BUFSIZE );

        timeout = false;
    }else
        offset = read( instance->fd, instance->msg, BUFSIZE );

    /* Generamos el msg a enviar */

    build_data_msg( instance );

    /* Enviamos el msg */

    sent = sendto( instance->local_descriptor,
                   instance->buf,
                   ACK_BUFSIZE + offset,
                   0,
                   (struct sockaddr *) &instance->remote_addr,
                   instance->size_remote );

    /* Verificamos que se haya enviado correctamente */

    if ( sent != ACK_BUFSIZE + offset )
        syslog(LOG_ERR, "Error from sendto() in data_send(): %s", strerror(errno));

    /* Iniciamos los temporizadores */

    //gettimeofday( &instance->now, 0 );
    //gettimeofday( &instance->timer, 0 );


    /* Esperamos el siguiente ack */

//    while ( instance->timer.tv_usec - instance->now.tv_usec < DEF_TIMEOUT_USEC ) {

        received = recvfrom( instance->local_descriptor,
                             instance->buf,
                             MAX_BUFSIZE,
                             0,
                             (struct sockaddr *) &instance->remote_addr,
                             &instance->size_remote );

        /* Verificamos que haya llegado un msg válido, se debe cumplir:
       1. Que received sea distinto a -1
       2. Que el OPCODE sea OPCODE_ACK
       3. Que el ack corresponda al blknum que esperamos
       4. Que el msg sea de donde lo esperamos (mismo tid del inicio de la transferencia) */

        if ( received != -1
             && received != EWOULDBLOCK
             && ( (instance->buf[0] << 8) + instance->buf[1] == OPCODE_ACK )
             && ( (instance->buf[2] << 8) + instance->buf[3]  == instance->blknum )
             && ( instance->tid == ntohs( instance->remote_addr.sin_port ) ) ) {

            instance->retries = 0;

            /* Si hemos enviado el último msg y recibido el último ack, terminamos */

            if ( offset < BUFSIZE ) {

                syslog( LOG_NOTICE , "File %s sent successful", instance->file );

                /* Cerramos el descriptor de archivo y de socket */

                close( instance->fd );
                close( instance->local_descriptor );

                /* Hijo finaliza */

                _exit( EXIT_SUCCESS );
            }

            /* Limpiamos los buffers */

            memset( instance->msg, 0, BUFSIZE );
            memset( instance->buf, 0, MAX_BUFSIZE );

            /* Aumentamos blknum */

            instance->blknum++;

            /* Si el blknum es 65536, le asignamos el valor 0 para no salirnos del rango de 2 bytes de la trama para el campo blknum */

            if ( instance->blknum  == 65536 )
                instance->blknum = 0;

            return;

        } //end 4-condition if
        //instance->timer.tv_usec++;
//    }//end while

    /* Como no hemos recibido el ack correspondiente a la última trama que hemos enviado, ha expirado el tiempo de espera */

    timeout = true;
    instance->retries++;

    
    if ( instance->retries == DEF_RETRIES )
        _err_log_exit( LOG_ERR, "Retries limit reached.");

    
    syslog( LOG_NOTICE, "Retrie number %d in data_send(); blknum %d", instance->retries,instance->blknum);
}

void start_data_send( tftp_t * instance ) {
    ssize_t sent;
    
    syslog( LOG_NOTICE, "File requested for read: %s", instance->file);

    /* Comprobamos si hay errores */
    if ( access( instance->file, F_OK ) != 0 ) {
        instance->err = ERR_NOT_FOUND;
        instance->msgerr = "No existe el archivo.";
        syslog( LOG_ERR, "Error access() on start_data_send(): %s", strerror(errno));
    }else if ( access( instance->file, R_OK ) != 0 ) {
        instance->err = ERR_ACCESS_DENIED;
        instance->msgerr = "No hay permisos de lectura.";
        syslog( LOG_ERR, "Error access() on start_data_send(): %s", strerror(errno));
    }else
        instance->msgerr = NULL;

    /* Si hay errores, enviar msg de error y terminar */
    if ( instance->msgerr ) {

        build_error( instance );

        sent = sendto( instance->local_descriptor,
                       instance->buf,
                       strlen( instance->buf ),
                       0,
                       (struct sockaddr *) &instance->remote_addr,
                       instance->size_remote);

        /* Cerramos descriptor de socket */
        close( instance->local_descriptor );

        /* Verificamos que hayamos enviado el msg correctamente */
        if ( sent != strlen( instance->buf ) )
            _err_log_exit(LOG_ERR, "Error from sendto() in start_data_send(): %s", strerror(errno));

        /* Hijo finaliza */
        _exit(EXIT_FAILURE);
    }

    /* Inicializamos las variables a usar */
    instance->blknum = 1;

    /* Abrimos el descriptor de archivo */
    instance->fd = open( instance->file, O_RDONLY );

    /* Iniciamos el temporizador */
    instance->timeout.tv_sec = 1;
    instance->timeout.tv_usec = 0;

    if ( setsockopt( instance->local_descriptor, SOL_SOCKET, SO_RCVTIMEO, (char *)&instance->timeout, sizeof(instance->timeout)) < 0 )
        _err_log_exit( LOG_ERR, "Error setsockopt() in data_send().");

    /* Seguimos */
    syslog( LOG_NOTICE, "Sending file %s in start_data_send()", instance->file );
    for(;;)
        data_send( instance );
}

void ack_send( tftp_t * instance ) {
    
    ssize_t  sent;
    ssize_t received;
    off_t offset;


    /* Generamos el ack */
    build_ack_msg( instance );

    /* Enviamos el ack */
    sent = sendto( instance->local_descriptor,
                   instance->buf,
                   ACK_BUFSIZE,
                   0,
                   (struct sockaddr *) &instance->remote_addr,
                   instance->size_remote);

    /* Verificamos que se haya enviado correctamente */

    if ( sent != ACK_BUFSIZE )
        _err_log_exit( LOG_ERR, "Error from sendto() in ack_send(): %s", strerror(errno));

    /* Iniciamos los temporizadores */

    //gettimeofday( &instance->now, 0 );
    //gettimeofday( &instance->timer, 0 );

    /* Asignamos -1 a blknum en caso de ser 65535 para evitar un rango incorrecto en los ack */
    if ( instance->blknum == 65535 ) instance->blknum = -1;

    /* Esperamos el siguiente msg */
    //while ( instance->timer.tv_usec - instance->now.tv_usec < DEF_TIMEOUT_USEC ) {
        received = recvfrom( instance->local_descriptor,
                             instance->buf,
                             MAX_BUFSIZE,
                             0,
                             (struct sockaddr *) &instance->remote_addr,
                             &instance->size_remote );

        /* Verificamos que haya llegado un msg válido, se debe cumplir: */
        /* 1. Que received sea distinto a -1 */
        /* 2. Que el OPCODE sea OPCODE_DATA */
        /* 3. Que el blknum sea el que esperamos */
        /* 4. Que el msg sea de donde lo esperamos (mismo tid del inicio de la transferencia) */

        if ( received != -1
             && received != EWOULDBLOCK
             && ( (instance->buf[0] << 8) + instance->buf[1]  == OPCODE_DATA )
             && ( (instance->buf[2] << 8) + instance->buf[3]  == instance->blknum + 1 )
             && instance->tid == ntohs(instance->remote_addr.sin_port) ) {
            instance->retries = 0;

            /* Procesamos los datos recibidos */
            dec_data( instance );

            /* Verificamos si es el último msg por recibir */
            if ( received < MAX_BUFSIZE ) {

                /* Escribimos en el archivo */

                write( instance->fd, instance->msg, received - ACK_BUFSIZE );


                /* Generamos el último ack */
                instance->blknum++;
                build_ack_msg( instance );

                /* Enviamos el último msg */
                sent = sendto( instance->local_descriptor,
                               instance->buf,
                               ACK_BUFSIZE,
                               0,
                               (struct sockaddr *) &instance->remote_addr,
                               instance->size_remote );

                /* Verificamos que se haya enviado correctamente */
                if ( sent != ACK_BUFSIZE )
                    _err_log_exit(LOG_ERR, "Error from sendto() in ack_send(): %s", strerror(errno));

                /* Cerramos el descriptor de archivo y de socket */

                close( instance->fd );
                close( instance->local_descriptor );

                /* */

                syslog(LOG_NOTICE, "Transfer successful: %s", instance->file );

                /* Hijo finaliza */

                _exit(EXIT_SUCCESS);
            }

            /* Escribimos en el archivo */
            offset = write(instance->fd ,instance->msg, received - ACK_BUFSIZE);

            /* Limpiamos buffer y msg */
            memset( instance->buf, 0, MAX_BUFSIZE);
            memset( instance->msg, 0, BUFSIZE);

            /* Incrementamos blknum */
            instance->blknum++;
            return;

        }//end 4-condition if
        //instance->timer.tv_usec++;
   // }//end while
    //instance->retries++;

    if ( instance->retries == DEF_RETRIES )
        _err_log_exit( LOG_ERR, "Retries limit reached.");

    syslog(LOG_NOTICE, "Retrie number %d in ack_send(); blknum %d",instance->retries,instance->blknum+1);
}

void start_ack_send( tftp_t * instance ) {

    ssize_t sent;

    /* Comprobamos si hay errores */
    if ( access( ".", W_OK ) == 0 ) instance->msgerr = NULL;
    else {
        instance->err = ERR_ACCESS_DENIED;
        instance->msgerr = "No hay permisos.";
        syslog( LOG_ERR, "start_ack_send(): No hay permisos");
    }

    /* Si hay errores, enviar msg de error y terminar */
    if ( instance->msgerr ) {

        build_error( instance );
        sent = sendto( instance->local_descriptor,
                       instance->buf,
                       MAX_BUFSIZE,
                       0,
                       (struct sockaddr *) &instance->remote_addr,
                       sizeof( struct sockaddr_in ) );

        /* Verificamos que hayamos enviado el msg correctamente */

        if ( sent != strlen( instance->buf ) )
            _err_log_exit(LOG_ERR, "Error from sendto() in child start_ack_send(): %s", strerror(errno));

        /* Cerramos descriptor de socket */

        close( instance->local_descriptor );
        _exit(EXIT_FAILURE);
    }
    
    /* Inicializamos las variables a usar */
    
    instance->blknum = 0;
    instance->fd = open( instance->file, O_WRONLY | O_CREAT | O_TRUNC , S_IRWXU | S_IRWXG | S_IRWXO);

    /* Iniciamos el temporizador */
    instance->timeout.tv_sec = 1;
    instance->timeout.tv_usec = 0;

    if ( setsockopt( instance->local_descriptor, SOL_SOCKET, SO_RCVTIMEO, (char *)&instance->timeout, sizeof(instance->timeout)) < 0 )
        _err_log_exit( LOG_ERR, "Error setsockopt() in data_send().");

    /* Seguimos */
    for(;;)
        ack_send( instance );
}

void child(tftp_tl * listen ) {

    tftp_t * instance;

    /* Cerramos el descriptor del padre que no usaremos en el hijo */

    close(listen->descriptor);

    /* Alojamos memoria dinamicamente */

    instance = malloc( sizeof( struct tftp ) );
    
    if ( !instance )
        _err_log_exit(LOG_ERR, "Error from malloc() in child(): %s", strerror(errno));

    /* Asignamos el valor 0 a los elementos de las estructura sockaddr_in */

    memset( &instance->local_addr, 0, sizeof( struct sockaddr_in ) );
    memset( &instance->remote_addr, 0, sizeof( struct sockaddr_in ) );

    /* Copiamos el buffer de listen en el buffer de instance */

    memcpy( instance->buf, listen->buf, MAX_BUFSIZE );

    /* Asociamos los descriptor del socket */

    instance->local_descriptor = socket( AF_INET, SOCK_DGRAM, 0 );

    /* Verificamos que no haya errores con la asociación del socket */

    if ( instance->local_descriptor == -1 )
        _err_log_exit(LOG_ERR, "Error from socket() in child(): %s", strerror(errno));

    /* Asigmanos datos del socket local. NOTA: Usando un puerto efímero ...*/

    instance->local_addr.sin_family = AF_INET;
    instance->local_addr.sin_addr.s_addr = INADDR_ANY;
    instance->local_addr.sin_port = htons(0);

    /* Asigmanos datos del socket remoto; el puerto de destino debe ser el mismo de donde recibimos la petición */

    instance->remote_addr = listen->remote_addr;

    /* Guardamos el puerto a donde debemos enviar los datos */

    instance->tid = ntohs( instance->remote_addr.sin_port );

    /* Asociamos el descriptor con el socket que queremos usar */

    if ( bind( instance->local_descriptor,
               (struct sockaddr *) &instance->local_addr,
               sizeof(struct sockaddr_in))  == -1 )
        _err_log_exit(LOG_ERR, "Error from bind() in child(): %s", strerror(errno));

    /* Terminanos de inicializar la estructura instance */

    initialize_server( instance );

    instance->state = listen->state;

    /* Verificamos que el modo de transferencia sea binario para seguir con la transferencia */

    if ( strcmp(instance->mode, MODE_OCTET) == 0 )
        if ( instance->state == STATE_DATA_SENT) start_data_send(instance);
        else start_ack_send(instance);
    
    else {
        /* Enviamos trama con msg de error */
        ssize_t sent;
        instance->err = ERR_ILLEGAL_OP;
        instance->msgerr = "Modo de transferencia no soportado.";

        build_error( instance );
        sent = sendto( instance->local_descriptor,
                       instance->buf,
                       MAX_BUFSIZE,
                       0,
                       (struct sockaddr *) &instance->remote_addr,
                       sizeof( struct sockaddr_in ) );

        /* Verificamos que hayamos enviado el msg correctamente */

        if ( sent != strlen( instance->buf ) )
            _err_log_exit(LOG_ERR, "Error from sendto() in child start_ack_send(): %s", strerror(errno));

        /* Cerramos descriptor de socket */

        close( instance->local_descriptor );


        _err_log_exit( LOG_ERR, "Transfer mode not supported  ...");

    }
}

void wait_request(tftp_tl * listen) {

    ssize_t received;
    pid_t childPid;

    /* limpiamos el buffer y esperamos msg válido */

    memset( listen->buf, 0, MAX_BUFSIZE );

    received = recvfrom( listen->descriptor,
                         listen->buf,
                         MAX_BUFSIZE,
                         0,
                         (struct sockaddr *) &listen->remote_addr,
                         &listen->remote_size );
    
    if( received != -1 ) {

        switch ( received = (listen->buf[0] << 8) + listen->buf[1] ) {
        case OPCODE_RRQ:
            syslog( LOG_NOTICE, "Received Read Request.");
            listen->state = STATE_DATA_SENT;
            break;
        case OPCODE_WRQ:
            syslog( LOG_NOTICE, "Received Write Request.");
            listen->state = STATE_ACK_SENT;
            break;
        default:
            listen->state = STATE_STANDBY;
        }

    }
    
    if ( received == OPCODE_RRQ || received == OPCODE_WRQ )  {

        switch ( childPid = fork() ) {
        case -1:
            err_log_exit(LOG_ERR, "Error from fork() in wait_request(): %s", strerror(errno));
            break;
        case 0:
            syslog(LOG_INFO, "Hijo creado correctamente con PID: %d", getpid());
            child( listen );
            _exit(EXIT_SUCCESS);
        }
    }
}

static void sigchld_handler(int sig) {

    /* Guardamos errno en caso de que haya cambiado */

    int savedErrno;

    savedErrno = errno;
    /* ciclo para hacerse cargo de todos los zombies hijo */

    while (waitpid(-1, NULL, WNOHANG) > 0)
        continue;

    errno = savedErrno;
}


int main ( int argc, char *argv[] ) {

    /* Abrimos salida al log del sistema */

    openlog( NULL, LOG_NOWAIT | LOG_PID, LOG_USER );

    syslog(LOG_NOTICE, "Starting TFTP Server ...");

    /* Estructura para manejar a los hijos muertos */

    struct sigaction signal_reap_child;

    /* Iniciamos la señal para enterrar a los hijos muertos */

    sigemptyset(&signal_reap_child.sa_mask);
    signal_reap_child.sa_flags = SA_RESTART;
    signal_reap_child.sa_handler = sigchld_handler;

    if ( sigaction(SIGCHLD, &signal_reap_child, NULL) == -1 )
        err_log_exit(LOG_ERR, "Error from sigaction() in main(): %s",  strerror(errno));

    tftp_tl listen;

    /* Asignamos el valor 0 a los elementos de la estructura sockaddr_in */

    memset( &listen.addr, 0, sizeof( struct sockaddr_in ) );
    listen.descriptor = socket(AF_INET, SOCK_DGRAM, 0);

    if ( listen.descriptor == -1 )
        err_log_exit(LOG_ERR, "Error from socket() in main(): %s", strerror(errno));

    /* Asignamos datos del socket escucha */

    listen.addr.sin_family = AF_INET;
    listen.addr.sin_addr.s_addr = INADDR_ANY;
    listen.addr.sin_port = htons(69);
    listen.remote_size = sizeof(struct sockaddr_in);

    /* Asociamos el descriptor con el socket que queremos usar */

    if ( bind( listen.descriptor, (struct sockaddr *) &listen.addr, sizeof(struct sockaddr_in))  == -1 )
        err_log_exit(LOG_ERR, "Error from bind() in main(): %s", strerror(errno));

    for(;;)
        wait_request(&listen);
}
