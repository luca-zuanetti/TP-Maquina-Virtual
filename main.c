#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ========================================================================= */
/* CONSTANTES Y CONFIGURACIÓN DE HARDWARE (MV1 - VERSIÓN 2026)               */
/* ========================================================================= */

#define RAM_SIZE       16384    /* Memoria principal fija de 16 KiB */
#define SEG_AMOUNT     8        /* Entradas en la tabla de descriptores */
#define HEADER_SIZE    8        /* Cabecera del archivo binario .vmx */
#define ENTRY_INACTIVE 0xFFFF   /* Valor indicador de segmento inactivo (-1) */

/* ========================================================================= */
/* ENUMERACIONES Y DICCIONARIOS                                              */
/* ========================================================================= */

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

static const char* regStr[32] = {
    "IP",       // 0  - Puntero de instrucción
    "OPC",      // 1  - Código de operación
    "OP1",      // 2  - Operando 1
    "OP2",      // 3  - Operando 2
    "LAR",      // 4  - Logic Address Register
    "MAR",      // 5  - Memory Address Register
    "MBR",      // 6  - Memory Buffer Register
    "RESERVED", // 7  
    "RESERVED", // 8  
    "RESERVED", // 9  
    "EAX",      // 10 
    "EBX",      // 11
    "ECX",      // 12  
    "EDX",      // 13 - Registros de propósito general (10 a 15)
    "EEX",      // 14  
    "EFX",      // 15 
    "AC",       // 16 - Acumulador
    "CC",       // 17 - Código de condición (NZCV)
    "RESERVED", // 18 
    "RESERVED", // 19 
    "RESERVED", // 20 
    "RESERVED", // 21 
    "RESERVED", // 22 
    "RESERVED", // 23 
    "RESERVED", // 24 
    "RESERVED", // 25 
    "CS",       // 26 - Code Segment
    "DS",       // 27 - Data Segment
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

/* ========================================================================= */
/* ESTRUCTURAS DEL ESTADO DE LA MÁQUINA VIRTUAL                              */
/* ========================================================================= */

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

void inicializarSegmentosYRegistros(TMV* mv, uint16_t tamCod) {
    // 1. Limpiar memoria física completa (RAM) a 0
    memset(mv->mem, 0, sizeof(mv->mem));

    // 2. Marcar todos los segmentos como inactivos
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = ENTRY_INACTIVE;
        mv->seg[i].size = ENTRY_INACTIVE;
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

int cargarArchivo(TMV* mv, const char* nombreArch) {
    FILE* arch = fopen(nombreArch, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo '%s'.\n", nombreArch);
        return 0;
    }

    uint8_t header[HEADER_SIZE];
    if (fread(header, 1, HEADER_SIZE, arch) != HEADER_SIZE) {
        fprintf(stderr, "Error: Archivo incompleto o no se pudo leer la cabecera.\n");
        fclose(arch);
        return 0;
    }

    if (memcmp(header, "VMX26", 5) != 0 || header[5] != 1) {
        fprintf(stderr, "Error: Identificador invalido (se esperaba 'VMX26') o version no soportada.\n");
        fclose(arch);
        return 0;
    }

    uint16_t tamCod = ((uint16_t)header[6] << 8) | header[7];

    if (tamCod > RAM_SIZE) {
        fprintf(stderr, "Error: El tamano del codigo (%u bytes) excede la memoria RAM disponible (%d bytes).\n", 
                tamCod, RAM_SIZE);
        fclose(arch);
        return 0;
    }

    inicializarSegmentosYRegistros(mv, tamCod);

    size_t leidos = fread(mv->mem + mv->seg[0].base, 1, tamCod, arch);
    if (leidos != tamCod) {
        fprintf(stderr, "Error: El archivo termino inesperadamente al leer el codigo.\n");
        fclose(arch);
        return 0;
    }

    fclose(arch);
    return 1;
}

/* ========================================================================= */
/* EJECUCIÓN Y TRADUCCIÓN DE MEMORIA                                         */
/* ========================================================================= */

// Función que traduce una dirección lógica a física y valida los límites.
// Retorna la dirección física (>= 0) o -1 si ocurre un Fallo de Segmento.
int32_t traducirDireccion(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes_acceso) {
    
    uint16_t indice_seg = (dir_logica >> 16) & 0xFFFF;
    uint16_t offset = dir_logica & 0xFFFF;

    if (indice_seg >= SEG_AMOUNT) return -1; 
    if (mv->seg[indice_seg].base == ENTRY_INACTIVE) return -1; 

    uint32_t dir_base = mv->seg[indice_seg].base;
    uint32_t tamano_seg = mv->seg[indice_seg].size;

    uint32_t dir_fisica = dir_base + offset;

    uint32_t limite_segmento = dir_base + tamano_seg;
    uint32_t limite_acceso = dir_fisica + cant_bytes_acceso;

    if (dir_fisica < dir_base || limite_acceso > limite_segmento) {
        return -1; 
    }

    return (int32_t)dir_fisica;
} 

void ejecutarMV(TMV* mv) {
    
    while (mv->reg[IP] != 0xFFFFFFFF) { 
        
        // =====================================================================
        // ETAPA 1: FETCH (BÚSQUEDA DEL PRIMER BYTE)
        // =====================================================================
        
        int32_t dir_fisica_ip = traducirDireccion(mv, mv->reg[IP], 1);
        
        if (dir_fisica_ip == -1) {
            printf("Error: Fallo de segmento leyendo instruccion en IP = %08X\n", mv->reg[IP]);
            mv->errorFlag = 1;
            break; 
        }
        
        uint8_t primer_byte = mv->mem[dir_fisica_ip];
        
        // =====================================================================
        // ETAPA 2: DECODE (DECODIFICACIÓN DEL OPCODE Y TIPOS)
        // =====================================================================
        
        uint8_t opcode = primer_byte & 0x1F;
        uint8_t tipo_opA = (primer_byte >> 4) & 0x03;
        uint8_t tipo_opB = (primer_byte >> 6) & 0x03;
        
        mv->reg[OPC] = opcode;
        
        // =====================================================================
        // ETAPA 3: EXTRACCIÓN DE LOS OPERANDOS (ORDEN INVERSO)
        // =====================================================================
        
        mv->reg[OP1] = (tipo_opA << 24);
        mv->reg[OP2] = (tipo_opB << 24);
        
        uint32_t offset_lectura = 1; 

        // LECTURA DEL OPERANDO B
        if (tipo_opB > 0) {
            int32_t valorB = 0;
            for (int i = 0; i < tipo_opB; i++) {
                int32_t dir_fis = traducirDireccion(mv, mv->reg[IP] + offset_lectura, 1);
                if (dir_fis == -1) {
                    mv->errorFlag = 1; break;
                }
                valorB = (valorB << 8) | mv->mem[dir_fis];
                offset_lectura++;
            }
            if (mv->errorFlag) break;

            if (tipo_opB == 2) valorB = (int16_t)valorB;
            mv->reg[OP2] |= (valorB & 0x00FFFFFF);
        }

        // LECTURA DEL OPERANDO A
        if (tipo_opA > 0) {
            int32_t valorA = 0;
            for (int i = 0; i < tipo_opA; i++) {
                int32_t dir_fis = traducirDireccion(mv, mv->reg[IP] + offset_lectura, 1);
                if (dir_fis == -1) {
                    mv->errorFlag = 1; break;
                }
                valorA = (valorA << 8) | mv->mem[dir_fis];
                offset_lectura++;
            }
            if (mv->errorFlag) break;

            if (tipo_opA == 2) valorA = (int16_t)valorA;
            mv->reg[OP1] |= (valorA & 0x00FFFFFF);
        }

        // =====================================================================
        // ETAPA 4: AVANCE DEL INSTRUCTION POINTER (IP)
        // =====================================================================
        
        mv->reg[IP] += offset_lectura;

    } 
}