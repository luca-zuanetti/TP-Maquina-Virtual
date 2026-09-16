#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>


/* CONSTANTES Y CONFIGURACIÓN DE HARDWARE */

#define RAM_SIZE       16384    /* Memoria principal fija de 16 KiB */
#define SEG_AMOUNT     8        /* Entradas en la tabla de descriptores */
#define HEADER_SIZE    8        /* Cabecera del archivo binario .vmx */

/* ENUMERACIONES (Identificadores de registros según consigna 2026)          */


typedef enum {
    /* Instrucción y control (0..3) */
    IP = 0, OPC, OP1, OP2,

    /* Acceso a memoria / Bus (4..6) */
    LAR = 4, MAR, MBR,

    /* Propósito general (10..15) */
    EAX = 10, EBX, ECX, EDX, EEX, EFX,

    /* Acumulador - Cod. de condicion (16..17) */
    AC = 16,
    CC = 17,

    /* Punteros de segmento (26..27) */
    CS = 26, DS
} RegName;

/* Nombres legibles para el desensamblador (-d) */
static const char* regStr[32] = {
    "IP",       // 0
    "OPC",      // 1
    "OP1",      // 2
    "OP2",      // 3
    "LAR",      // 4
    "MAR",      // 5
    "MBR",      // 6
    "RESERVED", // 7
    "RESERVED", // 8
    "RESERVED", // 9
    "EAX",      // 10
    "EBX",      // 11
    "ECX",      // 12
    "EDX",      // 13
    "EEX",      // 14
    "EFX",      // 15
    "AC",       // 16
    "CC",       // 17
    "RESERVED", // 18
    "RESERVED", // 19
    "RESERVED", // 20
    "RESERVED", // 21
    "RESERVED", // 22
    "RESERVED", // 23
    "RESERVED", // 24
    "RESERVED", // 25
    "CS",       // 26
    "DS",       // 27
    "RESERVED", // 28
    "RESERVED", // 29
    "RESERVED", // 30
    "RESERVED"  // 31
};

/* ENUMERACIÓN DE INSTRUCCIONES (Opcodes 2026) */
typedef enum {
    SYS = 0x00, JMP, JP, JN, JZ, JC, JV, JNP, JNN, JNZ, NOT,
    STOP = 0x0F,
    MOV = 0x10, ADD, SUB, MUL, DIV, CMP,
    AND = 0x16, OR, XOR, SWAP,
    SHL = 0x1A, SHR, SAR,
    LDL = 0x1D, LDH, RND
} OpCode;

/* Vector de mnemónicos para el desensamblador (-d) */
static const char* opStr[32] = {
    "SYS",      /* 0x00 */
    "JMP",      /* 0x01 */
    "JP",       /* 0x02 */
    "JN",       /* 0x03 */
    "JZ",       /* 0x04 */
    "JC",       /* 0x05 */
    "JV",       /* 0x06 */
    "JNP",      /* 0x07 */
    "JNN",      /* 0x08 */
    "JNZ",      /* 0x09 */
    "NOT",      /* 0x0A */
    "---",      /* 0x0B (Inválido/Vacío) */
    "---",      /* 0x0C (Inválido/Vacío) */
    "---",      /* 0x0D (Inválido/Vacío) */
    "---",      /* 0x0E (Inválido/Vacío) */
    "STOP",     /* 0x0F */
    "MOV",      /* 0x10 */
    "ADD",      /* 0x11 */
    "SUB",      /* 0x12 */
    "MUL",      /* 0x13 */
    "DIV",      /* 0x14 */
    "CMP",      /* 0x15 */
    "AND",      /* 0x16 */
    "OR",       /* 0x17 */
    "XOR",      /* 0x18 */
    "SWAP",     /* 0x19 */
    "SHL",      /* 0x1A */
    "SHR",      /* 0x1B */
    "SAR",      /* 0x1C */
    "LDL",      /* 0x1D */
    "LDH",      /* 0x1E */
    "RND"       /* 0x1F */
};

/* ESTRUCTURAS DEL ESTADO DE LA MÁQUINA VIRTUAL*/


/* Descriptor de segmento: define la ubicación física y tamaño en RAM */
typedef struct {
    uint16_t base;  /* Dirección física de inicio */
    uint16_t size;  /* Cantidad de bytes que ocupa el segmento */
} TableSeg;

/* Estado completo de la máquina virtual (TMV) */
typedef struct {
    uint8_t   mem[RAM_SIZE];       /* Memoria física de 16 KiB (bytes sin signo) */
    int32_t   reg[32];             /* 32 registros de 32 bits con signo */
    TableSeg  seg[SEG_AMOUNT];     /* Tabla de 8 descriptores de segmentos */
    int       errorFlag;           /* Estado de error que detiene la máquina */
} TMV;




/* ========================================================================= */
/* FUNCIONES DE INICIALIZACIÓN Y CARGA                                       */
/* ========================================================================= */

/**
 * @brief Configura la memoria segmentada y los registros iniciales.
 * 
 * En la Parte 1:
 * - El segmento 0 (CS) inicia en 0 con el tamaño exacto del código.
 * - El segmento 1 (DS) ocupa el resto de la memoria disponible.
 * - Las entradas 2 a 7 quedan inactivas con valor -1 (0xFFFF).
 */
void inicializarSegmentosYRegistros(TMV* mv, uint16_t tamCod) {
    // 1. Limpiar memoria física completa (RAM) a 0
    memset(mv->mem, 0, sizeof(mv->mem));

    // 2. Marcar todos los segmentos como inactivos (0xFFFF)
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = 0xFFFF;
        mv->seg[i].size = 0xFFFF;
    }

    // 3. Configurar Segmento de Código (Entrada 0)
    mv->seg[0].base = 0;
    mv->seg[0].size = tamCod;

    // 4. Configurar Segmento de Datos (Entrada 1)
    mv->seg[1].base = tamCod;
    mv->seg[1].size = RAM_SIZE - tamCod;

    // 5. Limpiar registros y banderas
    memset(mv->reg, 0, sizeof(mv->reg));
    mv->errorFlag = 0;

    // 6. Configurar punteros lógicos (16 bits segmento | 16 bits offset)
    mv->reg[CS] = 0x00000000;              /* Segmento 0, Offset 0 */
    mv->reg[DS] = 0x00010000;              /* Segmento 1, Offset 0 */
    mv->reg[IP] = mv->reg[CS];             /* El punto de entrada arranca en CS */
}

/**
 * @brief Abre el archivo .vmx, valida la cabecera y carga el código en RAM.
 * 
 * @return 1 si la carga fue exitosa, 0 si ocurrió un error.
 */
int cargarArchivo(TMV* mv, const char* nombreArch) {
    FILE* arch = fopen(nombreArch, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo '%s'.\n", nombreArch);
        return 0;
    }

    // Lectura de los 8 bytes de cabecera
    uint8_t header[HEADER_SIZE];
    if (fread(header, 1, HEADER_SIZE, arch) != HEADER_SIZE) {
        fprintf(stderr, "Error: Archivo incompleto o no se pudo leer la cabecera.\n");
        fclose(arch);
        return 0;
    }

    // Validación 1: Firma "VMX26" (bytes 0-4) y Versión 1 (byte 5)
    if (memcmp(header, "VMX26", 5) != 0 || header[5] != 1) {
        fprintf(stderr, "Error: Identificador invalido (se esperaba 'VMX26') o version no soportada.\n");
        fclose(arch);
        return 0;
    }

    // Reconstrucción del tamaño del código (bytes 6 y 7 en Big Endian)
    uint16_t tamCod = ((uint16_t)header[6] << 8) | header[7];

    // Validación 2: El código no puede superar la memoria física disponible
    if (tamCod > RAM_SIZE) {
        fprintf(stderr, "Error: El tamano del codigo (%u bytes) excede la memoria RAM disponible (%d bytes).\n", 
                tamCod, RAM_SIZE);
        fclose(arch);
        return 0;
    }

    // Inicializar hardware con las dimensiones del código cargado
    inicializarSegmentosYRegistros(mv, tamCod);

    // Carga física del código en memoria principal (a partir de la base de CS que es 0)
    size_t leidos = fread(mv->mem + mv->seg[0].base, 1, tamCod, arch);
    if (leidos != tamCod) {
        fprintf(stderr, "Error: El archivo termino inesperadamente al leer el codigo.\n");
        fclose(arch);
        return 0;
    }

    fclose(arch);
    return 1;
}