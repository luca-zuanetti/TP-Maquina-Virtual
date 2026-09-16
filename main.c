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
/* ENUMERACIONES (Identificadores de registros según consigna 2026)          */
/* ========================================================================= */

typedef enum {
    /* Registros de control e instrucción (0 a 3) */
    REG_IP = 0,     /* Instruction Pointer: apunta a la próxima instrucción */
    REG_OPC,        /* Operation Code: código de operación actual */
    REG_OP1,        /* Operando A decodificado */
    REG_OP2,        /* Operando B decodificado */

    /* Registros de interfaz de memoria / Bus (4 a 6) */
    REG_LAR,        /* Logic Address Register: dirección lógica a acceder */
    REG_MAR,        /* Memory Address Register: parte alta bytes, baja dir. física */
    REG_MBR,        /* Memory Buffer Register: dato transferido con la memoria */

    /* Registros de propósito general (10 a 15) */
    REG_EAX = 10,
    REG_EBX,
    REG_ECX,
    REG_EDX,
    REG_EEX,
    REG_EFX,

    /* Registros especiales (16 y 17) */
    REG_AC = 16,    /* Acumulador: operaciones auxiliares y resto de división */
    REG_CC = 17,    /* Condition Code: banderas de estado NZCV */

    /* Punteros de segmento (26 y 27) */
    REG_CS = 26,    /* Code Segment: puntero lógico al segmento de código */
    REG_DS = 27     /* Data Segment: puntero lógico al segmento de datos */
} RegName;

/* Nombres legibles para el desensamblador (-d) */
static const char* regStr[32] = {
    [REG_IP]  = "IP",  [REG_OPC] = "OPC", [REG_OP1] = "OP1", [REG_OP2] = "OP2",
    [REG_LAR] = "LAR", [REG_MAR] = "MAR", [REG_MBR] = "MBR",
    [REG_EAX] = "EAX", [REG_EBX] = "EBX", [REG_ECX] = "ECX",
    [REG_EDX] = "EDX", [REG_EEX] = "EEX", [REG_EFX] = "EFX",
    [REG_AC]  = "AC",  [REG_CC]  = "CC",
    [REG_CS]  = "CS",  [REG_DS]  = "DS"
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

/**
 * @brief Configura la memoria segmentada y los registros iniciales.
 * 
 * En la Parte 1:
 * - El segmento 0 (CS) inicia en 0 con el tamaño exacto del código.
 * - El segmento 1 (DS) ocupa el resto de la memoria disponible.
 * - Las entradas 2 a 7 quedan inactivas con valor -1 (0xFFFF).
 */
void inicializarSegmentosYRegistros(TMV* mv, uint16_t tamCod) {
    // 1. Marcar todos los segmentos como inactivos (0xFFFF)
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = ENTRY_INACTIVE;
        mv->seg[i].size = ENTRY_INACTIVE;
    }

    // 2. Configurar Segmento de Código (Entrada 0)
    mv->seg[0].base = 0;
    mv->seg[0].size = tamCod;

    // 3. Configurar Segmento de Datos (Entrada 1)
    mv->seg[1].base = tamCod;
    mv->seg[1].size = RAM_SIZE - tamCod;

    // 4. Limpiar todos los registros a 0
    memset(mv->reg, 0, sizeof(mv->reg));

    // 5. Configurar punteros lógicos (16 bits segmento | 16 bits offset)
    mv->reg[REG_CS] = 0x00000000;              /* Segmento 0, Offset 0 */
    mv->reg[REG_DS] = 0x00010000;              /* Segmento 1, Offset 0 */
    mv->reg[REG_IP] = mv->reg[REG_CS];         /* El punto de entrada arranca en CS */
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

// Función que traduce una dirección lógica a física y valida los límites.
// Retorna la dirección física (>= 0) o -1 si ocurre un Fallo de Segmento.
int32_t traducirDireccion(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes_acceso) {
    
    // 1. Extraemos el índice del segmento (16 bits más significativos) y el offset (16 bits menos significativos).
    uint16_t indice_seg = (dir_logica >> 16) & 0xFFFF;
    uint16_t offset = dir_logica & 0xFFFF;

    // 2. Verificamos que el segmento apuntado no exceda las entradas de la tabla (0 a 7).
    if (indice_seg >= SEG_AMOUNT) {
        return -1; // Error: Fallo de segmento
    }

    // 3. Verificamos que el segmento no apunte a una entrada inactiva (-1 o 0xFFFF).
    if (mv->seg[indice_seg].base == ENTRY_INACTIVE) {
        return -1; // Error: Fallo de segmento
    }

    uint32_t dir_base = mv->seg[indice_seg].base;
    uint32_t tamano_seg = mv->seg[indice_seg].size;

    // 4. Calculamos la Dirección Física real (Base + Offset)[cite: 2].
    uint32_t dir_fisica = dir_base + offset;

    // 5. Calculamos los límites para proteger la memoria[cite: 4].
    uint32_t limite_segmento = dir_base + tamano_seg;
    uint32_t limite_acceso = dir_fisica + cant_bytes_acceso;

    // 6. Verificamos las dos condiciones obligatorias de protección de memoria[cite: 4].
    //    a) Dirección Base <= Dirección Física
    //    b) Límite del Segmento >= Límite de Acceso
    if (dir_fisica < dir_base || limite_acceso > limite_segmento) {
        return -1; // Error: Fallo de segmento por desbordamiento
    }

    // Si pasó todas las validaciones de seguridad, devolvemos la dirección física lista para usar en la RAM.
    return (int32_t)dir_fisica;
} 