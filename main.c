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

static const char* regStr[32] = {
    "IP",        // 0  - Puntero de instrucción
    "OPC",       // 1  - Código de operación
    "OP1",       // 2  - Operando 1
    "OP2",       // 3  - Operando 2
    "LAR",       // 4  - Logic Address Register
    "MAR",       // 5  - Memory Address Register
    "MBR",       // 6  - Memory Buffer Register
    "RESERVED",  // 7  
    "RESERVED",  // 8  
    "RESERVED",  // 9  
    "EAX",       // 10 
    "EBX",       // 11
    "ECX",       // 12  
    "EDX",       // 13 - Registros de propósito general (10 a 15)
    "EEX",       // 14  
    "EFX",       // 15 
    "AC",        // 16 - Acumulador
    "CC",        // 17 - Código de condición (NZCV)
    "RESERVED",  // 18 
    "RESERVED",  // 19 
    "RESERVED",  // 20 
    "RESERVED",  // 21 
    "RESERVED",  // 22 
    "RESERVED",  // 23 
    "RESERVED",  // 24 
    "RESERVED",  // 25 
    "CS",        // 26 - Code Segment
    "DS",        // 27 - Data Segment
    "RESERVED",  // 28 
    "RESERVED",  // 29 
    "RESERVED",  // 30 
    "RESERVED"   // 31 
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

void ejecutarMV(TMV* mv) {
    
    // 1. EL BUCLE PRINCIPAL
    // La ejecución se debe repetir hasta que el registro IP apunte fuera del segmento de código, 
    // o hasta que una instrucción STOP le asigne -1 (0xFFFFFFFF) al registro IP.
    while (mv->reg[0] != 0xFFFFFFFF) { 
        
        // =====================================================================
        // ETAPA 1: FETCH (BÚSQUEDA DEL PRIMER BYTE)
        // =====================================================================
        
        // Traducimos la dirección lógica del IP a física para leer el primer byte.
        // REGLA DE ORO: La lectura de la instrucción no debe modificar LAR, MAR ni MBR[cite: 4, 5].
        int32_t dir_fisica_ip = traducirDireccion(mv, mv->reg[0], 1);
        
        if (dir_fisica_ip == -1) {
            printf("Error: Fallo de segmento leyendo instruccion en IP = %08X\n", mv->reg[0]);
            mv->errorFlag = 1;
            break; 
        }
        
        // Leemos el primer byte crudo
        uint8_t primer_byte = mv->mem[dir_fisica_ip];
        
        // =====================================================================
        // ETAPA 2: DECODE (DECODIFICACIÓN DEL OPCODE Y TIPOS)
        // =====================================================================
        
        // El primer byte de control desglosa el código de operación y el tipo de los operandos[cite: 3].
        // A) Código de Operación: 5 bits menos significativos.
        uint8_t opcode = primer_byte & 0x1F;
        
        // B) Tipo de Operando A (bits 4 y 5) y Tipo de Operando B (bits 6 y 7).
        uint8_t tipo_opA = (primer_byte >> 4) & 0x03;
        uint8_t tipo_opB = (primer_byte >> 6) & 0x03;
        
        // Almacenamos el código de operación en el registro OPC (índice 1).
        mv->reg[1] = opcode;
        
        // =====================================================================
        // ETAPA 3: EXTRACCIÓN DE LOS OPERANDOS (ORDEN INVERSO)
        // =====================================================================
        
        // Inicializamos OP1 y OP2. El byte más significativo contendrá el código binario 
        // del tipo de operando, y si no existe, el registro tendrá un 0[cite: 5].
        mv->reg[2] = (tipo_opA << 24);
        mv->reg[3] = (tipo_opB << 24);
        
        // Llevamos la cuenta del tamaño de la instrucción. Arranca en 1 (el primer byte).
        uint32_t offset_lectura = 1; 

        // LECTURA DEL OPERANDO B (Se lee primero porque se codifican en orden inverso)[cite: 3, 5].
        if (tipo_opB > 0) {
            int32_t valorB = 0;
            // El tamaño del operando en bytes coincide con su código binario[cite: 5].
            for (int i = 0; i < tipo_opB; i++) {
                int32_t dir_fis = traducirDireccion(mv, mv->reg[0] + offset_lectura, 1);
                if (dir_fis == -1) {
                    mv->errorFlag = 1; break;
                }
                valorB = (valorB << 8) | mv->mem[dir_fis];
                offset_lectura++;
            }
            if (mv->errorFlag) break;

            // Si es un inmediato (tipo 10, ocupa 2 bytes), se interpreta como entero con signo[cite: 3, 5].
            if (tipo_opB == 2) valorB = (int16_t)valorB;
            
            // Los restantes tres bytes guardan el valor del operando[cite: 5].
            mv->reg[3] |= (valorB & 0x00FFFFFF);
        }

        // LECTURA DEL OPERANDO A
        if (tipo_opA > 0) {
            int32_t valorA = 0;
            for (int i = 0; i < tipo_opA; i++) {
                int32_t dir_fis = traducirDireccion(mv, mv->reg[0] + offset_lectura, 1);
                if (dir_fis == -1) {
                    mv->errorFlag = 1; break;
                }
                valorA = (valorA << 8) | mv->mem[dir_fis];
                offset_lectura++;
            }
            if (mv->errorFlag) break;

            if (tipo_opA == 2) valorA = (int16_t)valorA;
            mv->reg[2] |= (valorA & 0x00FFFFFF);
        }

        // =====================================================================
        // ETAPA 4: AVANCE DEL INSTRUCTION POINTER (IP)
        // =====================================================================
        
        // Ubicamos el registro IP en la próxima instrucción sumando el tamaño de la instrucción actual[cite: 5].
        mv->reg[0] += offset_lectura;

        // --- ACÁ TERMINA LA BÚSQUEDA Y DECODIFICACIÓN ---
        // Lo que siga acá abajo será llamar al desensamblador o al switch de ejecución.

    } // Fin del while
}