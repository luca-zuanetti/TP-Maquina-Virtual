#ifndef MV_H
#define MV_H

#include <stdint.h>

/* ========================================================================= */
/* CONSTANTES DE ARQUITECTURA                            */
/* ========================================================================= */
#define RAM_SIZE       16384    /* Memoria principal fija de 16 KiB (16384 bytes) */
#define SEG_AMOUNT     8        /* Entradas en la tabla de descriptores de segmentos */
#define HEADER_SIZE    8        /* Tamaño en bytes de la cabecera del archivo .vmx */
#define ENTRY_INACTIVE 0xFFFF   /* Valor indicador de descriptor inactivo (-1) */

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
    EAX = 10,   /* Registros de propósito general */
    EBX,
    ECX,
    EDX,
    EEX,
    EFX,
    AC = 16,    /* Registro Acumulador */
    CC = 17,    /* Registro de Código de Condición */
    CS = 26,    /* Registro base del Segmento de Código */
    DS = 27     /* Registro base del Segmento de Datos */
} RegName;

/* ========================================================================= */
/* ENUMERACIÓN DE CÓDIGOS DE OPERACIÓN (OPCODES)                             */
/* ========================================================================= */
typedef enum {
    SYS = 0x00, 
    JMP, JP, JN, JZ, JC, JV, JNP, JNN, JNZ, 
    NOT = 0x0A,
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
    uint8_t   mem[RAM_SIZE];   /* Memoria física lineal de 16 KiB */
    int32_t   reg[32];         /* Banco de 32 registros de 32 bits */
    TableSeg  seg[SEG_AMOUNT]; /* Tabla de descriptores de segmentos */
    int       errorFlag;       /* Bandera de control de errores (0 = OK, 1 = Error) */
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
void inicializarSegmentosYRegistros(TMV* mv, uint16_t tamCod);
int cargarArchivo(TMV* mv, const char* nombreArch);
int32_t traducirDireccion(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes_acceso);
void ejecutarMV(TMV* mv);
void ejecutarDisassembler(TMV* mv);
void inicializarTablaInstrucciones(void);

#endif /* MV_H */