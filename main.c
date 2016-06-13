#include <unistd.h> //llamadas al sistema
#include <sys/types.h> //tipos de dato *_t para el Sistema Operativo
#include <fcntl.h> //constantes tipo O_*
#include <sys/stat.h> //información sobre atributos de archivos
#include <sys/time.h> //funciones de tiempo
#include <sys/socket.h>//socket
#include <arpa/inet.h>//funciones usadas para internet
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

typedef struct tftp
{
    int                  remote_descriptor;   /* descriptor de socket remoto */
    int                  local_descriptor;    /* descriptor de socket local*/
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
    struct sockaddr_in   remote;              /* estructura remota */
    struct sockaddr_in   local;               /* estructura local */
    socklen_t            size_remote;         /* tamaño estructura remota */
    socklen_t            size_local;            /* tamaño estructura local */
    u_char               lastack[MAX_BUFSIZE];  /* arreglo último ack */
    u_char               msj[BUFSIZE];          /* tamaño max msj a enviar */
    u_char               buf[MAX_BUFSIZE];      /* tamaño max msj a recibir */
} tftp_t;



int main(int argc, char *argv[])
{
    printf("Hello World!\n");
    return  EXIT_SUCCESS;
}
