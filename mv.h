#ifndef MV_H
#define MV_H

#include <stdint.h>

/* ========================================================================= */
/* CONSTANTES Y CONFIGURACIÓN DE HARDWARE (MV1 - VERSIÓN 2026)               */
/* ========================================================================= */
#define RAM_SIZE       16384    /* Memoria principal fija de 16 KiB */
#define SEG_AMOUNT     8        /* Entradas en la tabla de descriptores */
#define HEADER_SIZE    8        /* Cabecera del archivo binario .vmx */
#define ENTRY_INACTIVE 0xFFFF   /* Valor indicador de segmento inactivo (-1) */

/* ========================================================================= */
/* ENUMERACIONES                                                             */
/* ========================================================================= */
typedef enum {
    IP = 0, OPC, OP1, OP2,
    LAR = 4, MAR, MBR,
    EAX = 10, EBX, ECX, EDX, EEX, EFX,
    AC = 16,
    CC = 17,
    CS = 26, DS = 27
} RegName;

typedef enum {
    SYS = 0x00, JMP, JP, JN, JZ, JC, JV, JNP, JNN, JNZ, NOT,
    STOP = 0x0F,
    MOV = 0x10, ADD, SUB, MUL, DIV, CMP,
    AND = 0x16, OR, XOR, SWAP,
    SHL = 0x1A, SHR, SAR,
    LDL = 0x1D, LDH, RND
} OpCode;

/* ========================================================================= */
/* ESTRUCTURAS DE LA MÁQUINA VIRTUAL                                         */
/* ========================================================================= */
typedef struct {
    uint16_t base;  
    uint16_t size;  
} TableSeg;

typedef struct {
    uint8_t   mem[RAM_SIZE];       
    int32_t   reg[32];             
    TableSeg  seg[SEG_AMOUNT];     
    int       errorFlag;           
} TMV;

/* ========================================================================= */
/* DECLARACIÓN DE VARIABLES GLOBALES (Definidas en main.c)                   */
/* ========================================================================= */
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

#endif // MV_H