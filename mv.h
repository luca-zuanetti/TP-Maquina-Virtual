#ifndef MV_H
#define MV_H

#include <stdint.h>

/* ========================================================================= */
/* CONSTANTES DE ARQUITECTURA                            */
/* ========================================================================= */
#define MEM_DEFAULT_KIB 16      /* Tamaño por defecto de la memoria principal (m=M omitido) */
#define MEM_MAX_KIB     64      /* Máximo direccionable con bases/offsets de 16 bits */
#define SEG_AMOUNT      8       /* Entradas en la tabla de descriptores de segmentos */
#define HEADER_SIZE_V1  8       /* Tamaño en bytes de la cabecera .vmx versión 1 */
#define HEADER_SIZE_V2  18      /* Tamaño en bytes de la cabecera .vmx versión 2 */
#define ENTRY_INACTIVE  0xFFFF  /* Valor indicador de descriptor inactivo (-1) */
#define PILA_INICIAL    12      /* Bytes precargados en la pila: retorno, argc y argv */
#define HEADER_SIZE_VMI 8       /* Tamaño en bytes de la cabecera .vmi */
#define VERSION_VMI     1       /* Versión del formato de imagen */

/* ========================================================================= */
/* BANDERAS DEL REGISTRO CC (Bits 31 a 28 de los 32 bits)                    */
/* ========================================================================= */
#define CC_N 0x80000000U  /* Bit 31: Signo / Negativo                        */
#define CC_Z 0x40000000U  /* Bit 30: Cero                                    */
#define CC_C 0x20000000U  /* Bit 29: Acarreo (Carry)                         */
#define CC_V 0x10000000U  /* Bit 28: Desbordamiento (Overflow C2)            */

/* ========================================================================= */
/* ENUMERACIÓN DE REGISTROS                                                  */
/* ========================================================================= */
typedef enum {
    IP = 0,     /* Puntero de instrucción */
    OPC,        /* Código de operación */
    OP1,        /* Operando 1 (destino) */
    OP2,        /* Operando 2 (fuente) */
    LAR = 4,    /* Registro de dirección lógica */
    MAR,        /* Registro de dirección física de memoria */
    MBR,        /* Registro de búfer de datos de memoria */
    SP = 7,     /* Puntero al tope de la pila (Stack Pointer) */
    BP = 8,     /* Puntero base de la pila (Base Pointer) */
    EAX = 10,   /* Registros de propósito general */
    EBX,
    ECX,
    EDX,
    EEX,
    EFX,
    AC = 16,    /* Registro Acumulador */
    CC = 17,    /* Registro de Código de Condición */
    CS = 26,    /* Puntero al comienzo del Code Segment */
    DS = 27,    /* Puntero al comienzo del Data Segment */
    ES = 28,    /* Puntero al comienzo del Extra Segment */
    SS = 29,    /* Puntero al comienzo del Stack Segment */
    KS = 30,    /* Puntero al comienzo del Const Segment */
    PS = 31     /* Puntero al comienzo del Param Segment */
} RegName;

/* ========================================================================= */
/* SEGMENTOS DEL PROCESO (en su orden físico obligatorio dentro de la RAM)   */
/* ========================================================================= */
typedef enum {
    SEG_PARAM = 0,  /* Param Segment  -> PS */
    SEG_CONST,      /* Const Segment  -> KS */
    SEG_CODE,       /* Code Segment   -> CS */
    SEG_DATA,       /* Data Segment   -> DS */
    SEG_EXTRA,      /* Extra Segment  -> ES */
    SEG_STACK,      /* Stack Segment  -> SS */
    CANT_SEGMENTOS
} SegProceso;

/* ========================================================================= */
/* ENUMERACIÓN DE CÓDIGOS DE OPERACIÓN (OPCODES)                             */
/* ========================================================================= */
typedef enum {
    SYS = 0x00, 
    JMP, JP, JN, JZ, JC, JV, JNP, JNN, JNZ, 
    NOT = 0x0A,
    PUSH = 0x0B, POP, CALL, RET,
    STOP = 0x0F,
    MOV = 0x10, ADD, SUB, MUL, DIV, CMP,
    AND = 0x16, OR, XOR, SWAP,
    SHL = 0x1A, SHR, SAR,
    LDL = 0x1D, LDH, RND
} OpCode;

/* ========================================================================= */
/* ESTRUCTURAS DE DATOS                                                      */
/* ========================================================================= */

/* Descriptor de segmento: almacena la base física y su tamaño en bytes */
typedef struct {
    uint16_t base;  /* Dirección física de inicio en la RAM */
    uint16_t size;  /* Cantidad de bytes asignados al segmento */
} TableSeg;

/* Estado global de la Máquina Virtual */
typedef struct {
    uint8_t*  mem;             /* Memoria física lineal (reservada dinámicamente) */
    uint32_t  memSize;         /* Tamaño de la memoria física en bytes (M * 1024) */
    int32_t   reg[32];         /* Banco de 32 registros de 32 bits */
    TableSeg  seg[SEG_AMOUNT]; /* Tabla de descriptores de segmentos */
    int       errorFlag;       /* Bandera de control de errores (0 = OK, 1 = Error) */
    const char* archVmi;       /* Archivo de imagen para los breakpoints (NULL si no se indicó) */
    int       bpPendiente;     /* 1 si SYS F pidió un breakpoint al terminar la instrucción */
    int       pasoAPaso;       /* 1 si se está ejecutando de a una instrucción (Enter) */
} TMV;

/* ========================================================================= */
/* TABLA DE DESPACHO DE INSTRUCCIONES                                        */
/* ========================================================================= */

/* Firma estándar para los manejadores de cada instrucción:
   - mv: puntero a la máquina virtual
   - tipo_opA: tipo de operando A (0: ninguno, 1: registro, 2: inmediato, 3: memoria)
   - valA: valor decodificado bruto de A
   - tipo_opB: tipo de operando B
   - valB: valor decodificado bruto de B */
typedef void (*InstruccionFunc)(TMV* mv, uint8_t tipo_opA, int32_t valA, uint8_t tipo_opB, int32_t valB);

/* Arreglo global de funciones para despachar instrucciones según su opcode */
extern InstruccionFunc tablaInstrucciones[32];

/* Diccionarios de texto para registros y mnemónicos */
extern const char* regStr[32];
extern const char* opStr[32];

/* ========================================================================= */
/* PROTOTIPOS DE FUNCIONES                                                   */
/* ========================================================================= */
int crearMemoria(TMV* mv, uint32_t tamKiB);
void liberarMemoria(TMV* mv);
int inicializarSegmentosYRegistros(TMV* mv, const uint32_t tamSeg[CANT_SEGMENTOS]);
int cargarArchivo(TMV* mv, const char* nombreArch, int cantParam, char** params);
int guardarImagen(TMV* mv, const char* nombreVmi);
int cargarImagen(TMV* mv, const char* nombreVmi);
int32_t traducirDireccion(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes_acceso);
void ejecutarMV(TMV* mv);
void ejecutarDisassembler(TMV* mv);
void inicializarTablaInstrucciones(void);

#endif /* MV_H */