#ifndef MV_H
#define MV_H

#include <stdint.h>

/* ========================================================================= */
/* CONSTANTES DE ARQUITECTURA                                                */
/* ========================================================================= */
#define MEM_DEFAULT_KIB 16      /* Memoria por defecto si no se indica m=M */
#define MEM_MAX_KIB     65535   /* Máximo que entra en la cabecera .vmi (2 bytes) */
#define ESPACIO_DIRECCIONABLE 0x10000U /* Bases de 16 bits: el proceso entra en 64 KiB */
#define SEG_AMOUNT      8       /* Entradas de la tabla de segmentos */
#define HEADER_SIZE_V1  8       /* Cabecera .vmx versión 1 */
#define HEADER_SIZE_V2  18      /* Cabecera .vmx versión 2 */
#define ENTRY_INACTIVE  0xFFFF  /* Marca de entrada de tabla sin usar */
#define PILA_INICIAL    12      /* Precarga de pila: retorno, argc y argv */
#define HEADER_SIZE_VMI 8       /* Cabecera .vmi */
#define VERSION_VMI     1       /* Versión del formato de imagen */

/* ========================================================================= */
/* BANDERAS DEL REGISTRO CC (bits 31 a 28)                                   */
/* ========================================================================= */
#define CC_N 0x80000000U  /* Negativo */
#define CC_Z 0x40000000U  /* Cero */
#define CC_C 0x20000000U  /* Carry */
#define CC_V 0x10000000U  /* Overflow */

/* ========================================================================= */
/* REGISTROS (índice = código del registro en la instrucción)                */
/* ========================================================================= */
typedef enum {
    IP = 0,     /* Próxima instrucción */
    OPC,        /* Opcode decodificado */
    OP1,        /* Operando A decodificado */
    OP2,        /* Operando B decodificado */
    LAR = 4,    /* Dirección lógica accedida */
    MAR,        /* Bytes accedidos | dirección física */
    MBR,        /* Dato leído o escrito */
    SP = 7,     /* Tope de la pila */
    BP = 8,     /* Base del marco de pila */
    EAX = 10,   /* Propósito general */
    EBX,
    ECX,
    EDX,
    EEX,
    EFX,
    AC = 16,    /* Acumulador (resto de DIV) */
    CC = 17,    /* Códigos de condición NZCV */
    CS = 26,    /* Punteros a segmento */
    DS = 27,
    ES = 28,
    SS = 29,
    KS = 30,
    PS = 31
} RegName;

/* ========================================================================= */
/* SEGMENTOS DEL PROCESO (en su orden físico en la RAM)                      */
/* ========================================================================= */
typedef enum {
    SEG_PARAM = 0,  /* PS */
    SEG_CONST,      /* KS */
    SEG_CODE,       /* CS */
    SEG_DATA,       /* DS */
    SEG_EXTRA,      /* ES */
    SEG_STACK,      /* SS */
    CANT_SEGMENTOS
} SegProceso;

/* ========================================================================= */
/* CÓDIGOS DE OPERACIÓN                                                      */
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

/* Entrada de la tabla de segmentos */
typedef struct {
    uint16_t base;  /* Dirección física de inicio */
    uint16_t size;  /* Tamaño en bytes */
} TableSeg;

/* Estado completo de la máquina virtual */
typedef struct {
    uint8_t*  mem;             /* RAM (reservada dinámicamente) */
    uint32_t  memSize;         /* Tamaño de la RAM en bytes */
    int32_t   reg[32];         /* Banco de registros */
    TableSeg  seg[SEG_AMOUNT]; /* Tabla de segmentos */
    int       errorFlag;       /* 1 = error: corta la ejecución */
    const char* archVmi;       /* Imagen para breakpoints (NULL = sin breakpoints) */
    int       bpPendiente;     /* SYS F pidió pausa */
    int       pasoAPaso;       /* Modo paso a paso activo */
} TMV;

/* ========================================================================= */
/* TABLA DE DESPACHO DE INSTRUCCIONES                                        */
/* ========================================================================= */

/* Firma común de las instrucciones: tipo (0 ninguno, 1 reg, 2 inm, 3 mem) y valor de A y B */
typedef void (*InstruccionFunc)(TMV* mv, uint8_t tipo_opA, int32_t valA, uint8_t tipo_opB, int32_t valB);

/* opcode -> función que lo ejecuta */
extern InstruccionFunc tablaInstrucciones[32];

/* Nombres de registros y mnemónicos (para el disassembler) */
extern const char* regStr[32];
extern const char* opStr[32];

/* ========================================================================= */
/* PROTOTIPOS                                                                */
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
