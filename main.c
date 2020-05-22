#include "tftp.h"

void initialize_server ( tftp_t *instance ) {
    memset ( instance->file, 0, NAMESIZE );

    char *  file = NULL;
    u_char *p;
    p = instance->buf;
    p += 2;
    file = strtok ( p, "\0" );

    /*  Si instance->file es nulo significa que el
        cliente no especificó un nombre de archivo. */
    if ( instance->file ) {

        strcpy ( instance->file, file );
        p += strlen ( instance->file ) + 1;

    } else
        _err_log_exit ( LOG_ERR,
                        "Error from initialize_server() in strtok(file): File "
                        "name not specified" );

    instance->mode        = strtok ( p, "\0" );
    instance->msgerr      = NULL;
    instance->size_remote = sizeof ( struct sockaddr_in );
}

void data_send ( tftp_t *instance ) {
    ssize_t     sent;
    ssize_t     received;
    off_t       offset;
    static bool timeout = false;

    if ( timeout ) {
        /* Regresamos 512 bytes por tener que reenviar el último msg */

        lseek ( instance->fd, -BUFSIZE, SEEK_CUR );

        /* Leemos 512 bytes del archivo */

        offset = read ( instance->fd, instance->msg, BUFSIZE );

        timeout = false;

    } else
        offset = read ( instance->fd, instance->msg, BUFSIZE );

    /*Generamos el msg a enviar */

    build_data_msg ( instance );

    /* Enviamos el msg */

    sent = sendto (
        instance->local_descriptor, instance->buf, ACK_BUFSIZE + offset, 0,
        ( struct sockaddr * ) &instance->remote_addr, instance->size_remote );

    /* Verificamos que se haya enviado correctamente */

    if ( sent != ACK_BUFSIZE + offset )
        syslog ( LOG_ERR, "Error from sendto() in data_send(): %s",
                 strerror ( errno ) );

    /* Esperamos el siguiente ack */

    received = recvfrom (
        instance->local_descriptor, instance->buf, MAX_BUFSIZE, 0,
        ( struct sockaddr * ) &instance->remote_addr, &instance->size_remote );

    /*  Verificamos que haya llegado un msg válido, se debe cumplir:
        1. Que received sea distinto a -1
        2. Que received sea distinto a EWOULDBLOCK
        3. Que el OPCODE sea OPCODE_ACK
        4. Que el ack corresponda al blknum que esperamos
        5. Que el msg sea de donde lo esperamos (mismo tid del inicio de la
        transferencia) */

    if ( received != -1 && received != EWOULDBLOCK
         && ( ( instance->buf[0] << 8 ) + instance->buf[1] == OPCODE_ACK )
         && ( ( instance->buf[2] << 8 ) + instance->buf[3] == instance->blknum )
         && ( instance->tid == ntohs ( instance->remote_addr.sin_port ) ) ) {
        instance->retries = 0;

        /*  Si hemos enviado el último msg y recibido el último ack, terminamos
        */

        if ( offset < BUFSIZE ) {
            syslog ( LOG_NOTICE, "File %s sent successfully", instance->file );

            /* Cerramos el descriptor de archivo y de socket */

            close ( instance->fd );
            close ( instance->local_descriptor );

            /* Hijo finaliza */

            _exit ( EXIT_SUCCESS );
        }

        /* Limpiamos los buffers */

        memset ( instance->msg, 0, BUFSIZE );
        memset ( instance->buf, 0, MAX_BUFSIZE );

        /* Aumentamos blknum */

        instance->blknum++;

        /*  Si el blknum es 65536, le asignamos el valor 0 para no salirnos del
            rango
            de 2 bytes de la trama para el campo blknum */

        if ( instance->blknum == 65536 )
            instance->blknum = 0;

        return;

    }  // end 4-condition if

    /*  Como no hemos recibido el ack correspondiente a la última trama que
       hemos
        enviado, ha expirado el tiempo de espera */

    timeout = true;
    instance->retries++;

    if ( instance->retries == DEF_RETRIES )
        _err_log_exit ( LOG_ERR, "Retries limit reached." );

    syslog ( LOG_NOTICE, "Retry number %d in data_send(); blknum %d",
             instance->retries, instance->blknum );
}

void start_data_send ( tftp_t *instance ) {
    ssize_t sent;

    syslog ( LOG_NOTICE, "File %s requested for read ", instance->file );

    /* Comprobamos si hay errores */

    if ( access ( instance->file, F_OK ) != 0 ) {
        instance->err    = ERR_NOT_FOUND;
        instance->msgerr = "The file doesn't exist.";
        syslog ( LOG_ERR, "Error access() from start_data_send(): %s",
                 strerror ( errno ) );

    } else if ( access ( instance->file, R_OK ) != 0 ) {
        instance->err    = ERR_ACCESS_DENIED;
        instance->msgerr = "Access denied (read).";
        syslog ( LOG_ERR, "Error access() from start_data_send(): %s (read)",
                 strerror ( errno ) );

    } else
        instance->msgerr = NULL;

    /* Si hay errores, enviar msg de error y terminar */

    if ( instance->msgerr ) {
        build_error ( instance );

        sent = sendto ( instance->local_descriptor, instance->buf,
                        strlen ( instance->buf ), 0,
                        ( struct sockaddr * ) &instance->remote_addr,
                        instance->size_remote );

        /* Cerramos descriptor de socket */

        close ( instance->local_descriptor );

        /* Verificamos que hayamos enviado el msg correctamente */

        if ( sent != strlen ( instance->buf ) )
            _err_log_exit ( LOG_ERR,
                            "Error from sendto() in start_data_send(): %s",
                            strerror ( errno ) );

        /* Hijo finaliza */

        _exit ( EXIT_FAILURE );
    }

    /* Inicializamos las variables a usar */

    instance->blknum = 1;

    /* Abrimos el descriptor de archivo */

    instance->fd = open ( instance->file, O_RDONLY );

    /* Seguimos */

    syslog ( LOG_NOTICE, "Sending file %s in start_data_send()",
             instance->file );

    for ( ;; )
        data_send ( instance );
}

void ack_send ( tftp_t *instance ) {
    ssize_t sent;
    ssize_t received;
    off_t   offset;

    /* Generamos el ack */

    build_ack_msg ( instance );

    /* Enviamos el ack */

    sent = sendto ( instance->local_descriptor, instance->buf, ACK_BUFSIZE, 0,
                    ( struct sockaddr * ) &instance->remote_addr,
                    instance->size_remote );

    /* Verificamos que se haya enviado correctamente */

    if ( sent != ACK_BUFSIZE )
        _err_log_exit ( LOG_ERR, "Error from sendto() in ack_send(): %s",
                        strerror ( errno ) );

    /*  Asignamos -1 a blknum en caso de ser 65535 para evitar un rango
        incorrecto
        en los ack */

    if ( instance->blknum == 65535 )
        instance->blknum = -1;

    /* Esperamos el siguiente msg */

    received = recvfrom (
        instance->local_descriptor, instance->buf, MAX_BUFSIZE, 0,
        ( struct sockaddr * ) &instance->remote_addr, &instance->size_remote );

    /* Verificamos que haya llegado un msg válido, se debe cumplir: */
    /* 1. Que received sea distinto a -1 */
    /* 2. Que received sea distinto a EWOULDBLOCK */
    /* 3. Que el OPCODE sea OPCODE_DATA */
    /* 4. Que el blknum sea el que esperamos */
    /*  5. Que el msg sea de donde lo esperamos (mismo tid del inicio de la
        transferencia) */

    if ( received != -1 && received != EWOULDBLOCK
         && ( ( instance->buf[0] << 8 ) + instance->buf[1] == OPCODE_DATA )
         && ( ( instance->buf[2] << 8 ) + instance->buf[3]
              == instance->blknum + 1 )
         && instance->tid == ntohs ( instance->remote_addr.sin_port ) ) {
        /*  Llegando un msg válido, reiniciamos a cero el número máximo de
            reintentos
            permitidos */

        instance->retries = 0;

        /* Procesamos los datos recibidos */

        dec_data ( instance );

        /* Verificamos si es el último msg por recibir */

        if ( received < MAX_BUFSIZE ) {
            /* Escribimos en el archivo */

            write ( instance->fd, instance->msg, received - ACK_BUFSIZE );

            /* Generamos el último ack */

            instance->blknum++;
            build_ack_msg ( instance );

            /* Enviamos el último msg */

            sent = sendto ( instance->local_descriptor, instance->buf,
                            ACK_BUFSIZE, 0,
                            ( struct sockaddr * ) &instance->remote_addr,
                            instance->size_remote );

            /* Verificamos que se haya enviado correctamente */

            if ( sent != ACK_BUFSIZE )
                _err_log_exit ( LOG_ERR,
                                "Error from sendto() in ack_send(): %s",
                                strerror ( errno ) );

            /* Cerramos el descriptor de archivo y de socket */

            close ( instance->fd );
            close ( instance->local_descriptor );

            syslog ( LOG_NOTICE, "File %s received successfully",
                     instance->file );

            /* Hijo finaliza */

            _exit ( EXIT_SUCCESS );
        }

        /* Escribimos en el archivo */

        offset = write ( instance->fd, instance->msg, received - ACK_BUFSIZE );

        /* Limpiamos buffer y msg */

        memset ( instance->buf, 0, MAX_BUFSIZE );
        memset ( instance->msg, 0, BUFSIZE );

        /* Incrementamos blknum */

        instance->blknum++;
        return;

    }  // end 4-condition if

    instance->retries++;

    if ( instance->retries == DEF_RETRIES )
        _err_log_exit ( LOG_ERR, "Retries limit reached." );

    syslog ( LOG_NOTICE, "Retry number %d in ack_send(); blknum %d",
             instance->retries, instance->blknum + 1 );
}

void start_ack_send ( tftp_t *instance ) {
    ssize_t sent;

    /* Comprobamos si hay errores */

    if ( access ( ".", W_OK ) == 0 )
        instance->msgerr = NULL;

    else {
        instance->err    = ERR_ACCESS_DENIED;
        instance->msgerr = "Access denied (write).";
        syslog ( LOG_ERR, "start_ack_send(): Access denied write." );
    }

    /* Si hay errores, enviar msg de error y terminar */

    if ( instance->msgerr ) {
        build_error ( instance );
        sent = sendto ( instance->local_descriptor, instance->buf,
                        strlen ( instance->buf ), 0,
                        ( struct sockaddr * ) &instance->remote_addr,
                        sizeof ( struct sockaddr_in ) );

        /* Verificamos que hayamos enviado el msg correctamente */

        if ( sent != strlen ( instance->buf ) )
            _err_log_exit ( LOG_ERR,
                            "Error from sendto() in child start_ack_send(): %s",
                            strerror ( errno ) );

        /* Cerramos descriptor de socket */

        close ( instance->local_descriptor );
        _exit ( EXIT_FAILURE );
    }

    /* Inicializamos las variables a usar */

    instance->blknum = 0;
    instance->fd     = open ( instance->file, O_WRONLY | O_CREAT | O_TRUNC,
                          S_IRWXU | S_IRWXG | S_IRWXO );

    syslog ( LOG_NOTICE, "Receiving file %s ", instance->file );

    /* Seguimos */

    for ( ;; )
        ack_send ( instance );
}

void child ( tftp_tl *listen ) {
    tftp_t *instance;

    /* Cerramos el descriptor del padre que no usaremos en el hijo */

    close ( listen->descriptor );

    /* Alojamos memoria dinamicamente */

    instance = malloc ( sizeof ( struct tftp ) );

    if ( !instance )
        _err_log_exit ( LOG_ERR, "Error from malloc() in child(): %s",
                        strerror ( errno ) );

    /* Asignamos el valor 0 a los elementos de las estructura sockaddr_in */

    memset ( &instance->local_addr, 0, sizeof ( struct sockaddr_in ) );
    memset ( &instance->remote_addr, 0, sizeof ( struct sockaddr_in ) );

    /* Copiamos el buffer de listen en el buffer de instance */

    memcpy ( instance->buf, listen->buf, MAX_BUFSIZE );

    /* Asociamos los descriptor del socket */

    instance->local_descriptor = socket ( AF_INET, SOCK_DGRAM, 0 );

    /* Verificamos que no haya errores con la asociación del socket */

    if ( instance->local_descriptor == -1 )
        _err_log_exit ( LOG_ERR, "Error from socket() in child(): %s",
                        strerror ( errno ) );

    /* Asigmanos datos del socket local. NOTA: Usando un puerto efímero ...*/

    instance->local_addr.sin_family      = AF_INET;
    instance->local_addr.sin_addr.s_addr = INADDR_ANY;
    instance->local_addr.sin_port        = htons ( 0 );

    /*  Asigmanos datos del socket remoto; el puerto de destino debe ser el
       mismo
        de donde recibimos la petición */

    instance->remote_addr = listen->remote_addr;

    /* Guardamos el puerto a donde debemos enviar los datos */

    instance->tid = ntohs ( instance->remote_addr.sin_port );

    /* Asociamos el descriptor con el socket que queremos usar */

    if ( bind ( instance->local_descriptor,
                ( struct sockaddr * ) &instance->local_addr,
                sizeof ( struct sockaddr_in ) )
         == -1 )
        _err_log_exit ( LOG_ERR, "Error from bind() in child(): %s",
                        strerror ( errno ) );

    /*  Asignamos el temporizador para que el socket envía señal cada vez que
        llega
        (o no) algo */

    if ( setsockopt ( instance->local_descriptor, SOL_SOCKET, SO_RCVTIMEO,
                      ( char * ) &instance->timeout,
                      sizeof ( instance->timeout ) )
         < 0 )
        _err_log_exit (
            LOG_ERR,
            "Error from setsockopt() in child( SOL_SOCKET, SO_RCVTIMEO )." );

    /* Terminanos de inicializar la estructura instance */

    initialize_server ( instance );

    instance->state = listen->state;

    /*  Verificamos que el modo de transferencia sea binario para seguir con la
        transferencia y
        empezamos el envío/recibo de datos */

    if ( strcmp ( instance->mode, MODE_OCTET ) == 0 ) {
        if ( instance->state == STATE_DATA_SENT )
            start_data_send ( instance );

        else
            start_ack_send ( instance );

    } else {
        /* Enviamos trama con msg de error */

        ssize_t sent;
        instance->err    = ERR_ILLEGAL_OP;
        instance->msgerr = "Transfer mode not supported.";

        build_error ( instance );
        sent = sendto ( instance->local_descriptor, instance->buf, MAX_BUFSIZE,
                        0, ( struct sockaddr * ) &instance->remote_addr,
                        sizeof ( struct sockaddr_in ) );

        /* Verificamos que hayamos enviado el msg correctamente */

        if ( sent != strlen ( instance->buf ) )
            _err_log_exit ( LOG_ERR,
                            "Error from sendto() in child start_ack_send(): %s",
                            strerror ( errno ) );

        /* Cerramos descriptor de socket */

        close ( instance->local_descriptor );

        _err_log_exit ( LOG_ERR, "Transfer mode not supported." );
    }
}

void wait_request ( tftp_tl *listen ) {
    ssize_t received;
    pid_t   childPid;

    /* limpiamos el buffer y esperamos msg válido */

    memset ( listen->buf, 0, MAX_BUFSIZE );

    received = recvfrom ( listen->descriptor, listen->buf, MAX_BUFSIZE, 0,
                          ( struct sockaddr * ) &listen->remote_addr,
                          &listen->remote_size );

    if ( received != -1 ) {
        switch ( received = ( listen->buf[0] << 8 ) + listen->buf[1] ) {
            case OPCODE_RRQ:
                syslog ( LOG_NOTICE, "Received Read Request." );
                listen->state = STATE_DATA_SENT;
                break;

            case OPCODE_WRQ:
                syslog ( LOG_NOTICE, "Received Write Request." );
                listen->state = STATE_ACK_SENT;
                break;

            default:
                listen->state = STATE_STANDBY;
                break;
        }
    }

    if ( received == OPCODE_RRQ || received == OPCODE_WRQ ) {
        switch ( childPid = fork () ) {
            case -1:
                err_log_exit ( LOG_ERR,
                               "Error from fork() in wait_request(): %s",
                               strerror ( errno ) );

            case 0:
                syslog ( LOG_INFO, "Child created successfully with PID: %d",
                         getpid () );
                child ( listen );
                _exit ( EXIT_SUCCESS );
        }
    }
}

static void sigchld_handler ( int sig ) {
    /* Guardamos errno en caso de que haya cambiado */

    int savedErrno;

    savedErrno = errno;

    /* Ciclo para hacerse cargo de todos los zombies hijo */

    while ( waitpid ( -1, NULL, WNOHANG ) > 0 )
        continue;

    errno = savedErrno;
}

int main ( int argc, char *argv[] ) {
    /* Abrimos salida al log del sistema */

    openlog ( NULL, LOG_NOWAIT | LOG_PID, LOG_USER );

    /* Estructura para manejar a los hijos muertos */

    struct sigaction signal_reap_child;

    /* Iniciamos la señal para enterrar a los hijos muertos */

    sigemptyset ( &signal_reap_child.sa_mask );
    signal_reap_child.sa_flags   = SA_RESTART;
    signal_reap_child.sa_handler = sigchld_handler;

    if ( sigaction ( SIGCHLD, &signal_reap_child, NULL ) == -1 )
        err_log_exit ( LOG_ERR, "Error from sigaction() in main(): %s",
                       strerror ( errno ) );

    tftp_tl listen;

    /* Asignamos el valor 0 a los elementos de la estructura sockaddr_in */

    memset ( &listen.addr, 0, sizeof ( struct sockaddr_in ) );
    listen.descriptor = socket ( AF_INET, SOCK_DGRAM, 0 );

    if ( listen.descriptor == -1 )
        err_log_exit ( LOG_ERR, "Error from socket() in main(): %s",
                       strerror ( errno ) );

    /* Asignamos datos del socket escucha */

    listen.addr.sin_family      = AF_INET;
    listen.addr.sin_addr.s_addr = INADDR_ANY;
    listen.addr.sin_port        = htons ( DEFAULT_SERVER_PORT );
    listen.remote_size          = sizeof ( struct sockaddr_in );

    /* Asociamos el descriptor con el socket que queremos usar */

    if ( bind ( listen.descriptor, ( struct sockaddr * ) &listen.addr,
                sizeof ( struct sockaddr_in ) )
         == -1 )
        err_log_exit ( LOG_ERR, "Error from bind() in main(): %s",
                       strerror ( errno ) );

    syslog ( LOG_NOTICE, "tftpd started." );

    for ( ;; )
        wait_request ( &listen );
}
