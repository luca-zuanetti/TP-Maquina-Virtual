#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "mv.h"

/* ========================================================================= */
/* DICCIONARIOS GLOBALES DE NOMBRES Y MNEMÓNICOS                             */
/* ========================================================================= */

// Mapeo de nombres para los 32 registros según la arquitectura 2026 (MV2)
const char* regStr[32] = {
    "IP", "OPC", "OP1", "OP2", "LAR", "MAR", "MBR", "SP",
    "BP", "RESERVED", "EAX", "EBX", "ECX", "EDX", "EEX", "EFX",
    "AC", "CC", "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "RESERVED", "RESERVED", "RESERVED", "RESERVED", "CS", "DS",
    "ES", "SS", "KS", "PS"
};

// Mapeo de mnemónicos para los 32 posibles códigos de operación (Opcodes)
const char* opStr[32] = {
    "SYS", "JMP", "JP", "JN", "JZ", "JC", "JV", "JNP", "JNN", "JNZ", "NOT",
    "PUSH", "POP", "CALL", "RET", "STOP", "MOV", "ADD", "SUB", "MUL", "DIV", 
    "CMP", "AND", "OR", "XOR", "SWAP", "SHL", "SHR", "SAR", "LDL", "LDH", "RND"
};

// Registro puntero asociado a cada segmento del proceso (mismo orden que SegProceso)
static const RegName regSegmento[CANT_SEGMENTOS] = { PS, KS, CS, DS, ES, SS };

/* ========================================================================= */
/* MEMORIA PRINCIPAL DE TAMAÑO VARIABLE                                      */
/* ========================================================================= */

/**
 * @brief Reserva la memoria principal de la Máquina Virtual.
 *
 * @param mv     Puntero a la Máquina Virtual.
 * @param tamKiB Tamaño de la memoria expresado en KiB (parámetro m=M).
 * @return int   1 si se pudo reservar, 0 ante error.
 */
int crearMemoria(TMV* mv, uint32_t tamKiB) {
    // El tamaño puede venir de m=M o de la cabecera de un .vmi: se valida aquí
    if (tamKiB == 0 || tamKiB > MEM_MAX_KIB) {
        fprintf(stderr, "Error: Tamano de memoria invalido (%u KiB, debe ser entre 1 y %d KiB).\n",
                tamKiB, MEM_MAX_KIB);
        mv->mem = NULL;
        mv->memSize = 0;
        return 0;
    }

    mv->memSize = tamKiB * 1024U;
    mv->mem = (uint8_t*)calloc(mv->memSize, sizeof(uint8_t));
    if (!mv->mem) {
        fprintf(stderr, "Error: No se pudo reservar la memoria principal (%u KiB).\n", tamKiB);
        mv->memSize = 0;
        return 0;
    }
    return 1;
}

/**
 * @brief Libera la memoria principal reservada por crearMemoria.
 */
void liberarMemoria(TMV* mv) {
    free(mv->mem);
    mv->mem = NULL;
    mv->memSize = 0;
}

/* ========================================================================= */
/* INICIALIZACIÓN DE SEGMENTOS Y REGISTROS                                   */
/* ========================================================================= */

/**
 * @brief Ubica los segmentos del proceso en la RAM, arma la tabla de
 *        descriptores e inicializa los registros punteros a segmento.
 *
 * Los segmentos se ubican de forma contigua respetando el orden
 * PS, KS, CS, DS, ES, SS. Solo se crean entradas en la tabla para los
 * segmentos de tamaño mayor a cero, en el mismo orden que en la RAM.
 * Los registros de los segmentos inexistentes quedan cargados con -1.
 *
 * @param mv     Puntero a la Máquina Virtual (con la memoria ya creada).
 * @param tamSeg Tamaño en bytes de cada segmento, indexado por SegProceso.
 * @return int   1 si el proceso entra en memoria, 0 si la memoria es insuficiente.
 */
int inicializarSegmentosYRegistros(TMV* mv, const uint32_t tamSeg[CANT_SEGMENTOS]) {
    // 1. Verificación de capacidad: el proceso completo debe entrar en la RAM
    uint32_t total = 0;
    for (int i = 0; i < CANT_SEGMENTOS; i++) {
        total += tamSeg[i];
    }
    if (total > mv->memSize) {
        fprintf(stderr, "Error: Memoria insuficiente (el proceso requiere %u bytes y la memoria tiene %u bytes).\n",
                total, mv->memSize);
        return 0;
    }

    // 2. Limpieza de la memoria, la tabla de descriptores y los registros.
    //    Las entradas inactivas deben contener 0xFFFF en base y tamaño.
    memset(mv->mem, 0, mv->memSize);
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = ENTRY_INACTIVE;
        mv->seg[i].size = ENTRY_INACTIVE;
    }
    memset(mv->reg, 0, sizeof(mv->reg));
    mv->errorFlag = 0;

    // 3. Ubicación contigua de los segmentos presentes y carga de sus punteros
    //    lógicos (16 bits índice en la tabla | 16 bits offset = 0)
    uint32_t base = 0;
    int entrada = 0;
    for (int i = 0; i < CANT_SEGMENTOS; i++) {
        if (tamSeg[i] > 0) {
            mv->seg[entrada].base = (uint16_t)base;
            mv->seg[entrada].size = (uint16_t)tamSeg[i];
            mv->reg[regSegmento[i]] = (int32_t)((uint32_t)entrada << 16);
            base += tamSeg[i];
            entrada++;
        } else {
            mv->reg[regSegmento[i]] = -1;
        }
    }

    // 4. La pila arranca vacía: SP apunta justo después del final del Stack Segment
    if (tamSeg[SEG_STACK] > 0) {
        mv->reg[SP] = mv->reg[SS] + (int32_t)tamSeg[SEG_STACK];
    } else {
        mv->reg[SP] = -1;
    }

    // 5. IP comienza en el inicio del Code Segment (el loader le suma el entry point)
    mv->reg[IP] = mv->reg[CS];
    return 1;
}

/**
 * @brief Escribe un valor de 4 bytes en formato Big Endian (el byte más
 *        significativo queda en la dirección más baja).
 */
static void escribirBigEndian32(uint8_t* destino, uint32_t valor) {
    destino[0] = (uint8_t)(valor >> 24);
    destino[1] = (uint8_t)(valor >> 16);
    destino[2] = (uint8_t)(valor >> 8);
    destino[3] = (uint8_t)valor;
}

/**
 * @brief Calcula el tamaño del Param Segment: los strings (nombre del .vmx y
 *        parámetros, cada uno con su '\0') seguidos del arreglo argv de punteros.
 */
static uint32_t calcularTamParam(const char* nombreArch, int cantParam, char** params) {
    uint32_t tam = (uint32_t)strlen(nombreArch) + 1;
    for (int i = 0; i < cantParam; i++) {
        tam += (uint32_t)strlen(params[i]) + 1;
    }
    return tam + 4U * (uint32_t)(cantParam + 1);
}

/**
 * @brief Escribe el contenido del Param Segment: primero los strings y al
 *        final el arreglo argv de punteros lógicos de 4 bytes (Big Endian).
 */
static void cargarParamSegment(TMV* mv, const char* nombreArch, int cantParam, char** params) {
    uint32_t entradaPS = ((uint32_t)mv->reg[PS] >> 16) & 0xFFFF;
    uint8_t* inicio    = mv->mem + mv->seg[entradaPS].base;
    uint32_t offset    = 0;

    // 1. Strings: nombre del archivo .vmx seguido de cada parámetro
    for (int i = 0; i <= cantParam; i++) {
        const char* str = (i == 0) ? nombreArch : params[i - 1];
        size_t largo = strlen(str) + 1;
        memcpy(inicio + offset, str, largo);
        offset += (uint32_t)largo;
    }

    // 2. Arreglo argv: punteros [entrada de PS | offset del string]
    uint32_t offsetStr = 0;
    for (int i = 0; i <= cantParam; i++) {
        const char* str = (i == 0) ? nombreArch : params[i - 1];
        escribirBigEndian32(inicio + offset, (entradaPS << 16) | offsetStr);
        offset    += 4;
        offsetStr += (uint32_t)strlen(str) + 1;
    }
}

/**
 * @brief Precarga la pila para la subrutina principal. Desde el tope:
 *        [SP] dirección de retorno -1, [SP+4] argc y [SP+8] puntero a argv.
 *
 * Sin parámetros (sin -p, o -p sin nada detrás): argc = 0 y argv = -1. Con parámetros: argc cuenta el nombre del .vmx
 * más los parámetros (tamaño del arreglo argv) y argv apunta al arreglo,
 * ubicado en el Param Segment después de todos los strings.
 * La escritura es directa sobre la RAM: es el loader, no una instrucción,
 * por lo que no pasa por LAR, MAR ni MBR.
 * Requiere un Stack Segment de al menos PILA_INICIAL bytes (lo valida cargarArchivo).
 */
static void precargarPila(TMV* mv, const char* nombreArch, int cantParam, char** params) {
    uint32_t entradaSS = ((uint32_t)mv->reg[SS] >> 16) & 0xFFFF;
    uint32_t tamSS     = mv->seg[entradaSS].size;

    int32_t argc   = 0;
    int32_t ptrArgv = -1;
    if (cantParam > 0) {
        // argv comienza al final de los strings: su offset es la suma de sus largos
        uint32_t offsetArgv = (uint32_t)strlen(nombreArch) + 1;
        for (int i = 0; i < cantParam; i++) {
            offsetArgv += (uint32_t)strlen(params[i]) + 1;
        }
        argc    = cantParam + 1;
        ptrArgv = (int32_t)((uint32_t)mv->reg[PS] | offsetArgv);
    }

    // SP pasa de SS + tamaño (pila vacía) a SS + tamaño - 12
    mv->reg[SP] -= PILA_INICIAL;
    uint8_t* tope = mv->mem + mv->seg[entradaSS].base + (tamSS - PILA_INICIAL);
    escribirBigEndian32(tope,     0xFFFFFFFFU);        // Dirección de retorno: fuera del Code Segment
    escribirBigEndian32(tope + 4, (uint32_t)argc);
    escribirBigEndian32(tope + 8, (uint32_t)ptrArgv);
}

/**
 * @brief Lee el archivo binario ejecutable (.vmx), crea los segmentos del
 *        proceso y carga el código, las constantes y los parámetros en RAM.
 *
 * Soporta ambas versiones de cabecera:
 * - Versión 1 (8 bytes):  solo Code Segment; el Data Segment ocupa el resto de la RAM.
 * - Versión 2 (18 bytes): tamaños de CS, DS, ES, SS, KS y offset del entry point;
 *   además precarga la pila de la subrutina principal (retorno -1, argc y argv).
 *
 * @param mv         Puntero a la Máquina Virtual (con la memoria ya creada).
 * @param nombreArch Nombre o ruta del archivo binario a ejecutar.
 * @param cantParam  Cantidad de parámetros indicados con -p (-1 si no se usó -p, 0 si -p no tiene parámetros).
 * @param params     Parámetros indicados con -p.
 * @return int       1 si la carga fue exitosa, 0 ante cualquier error.
 */
int cargarArchivo(TMV* mv, const char* nombreArch, int cantParam, char** params) {
    FILE* arch = fopen(nombreArch, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo '%s'.\n", nombreArch);
        return 0;
    }

    // Lectura de la parte común de la cabecera (identificador, versión y tamaño del código)
    uint8_t header[HEADER_SIZE_V2];
    if (fread(header, 1, HEADER_SIZE_V1, arch) != HEADER_SIZE_V1) {
        fprintf(stderr, "Error: Archivo incompleto o no se pudo leer la cabecera.\n");
        fclose(arch);
        return 0;
    }

    // Verificación de la firma "VMX26" y versión (1 o 2)
    uint8_t version = header[5];
    if (memcmp(header, "VMX26", 5) != 0 || (version != 1 && version != 2)) {
        fprintf(stderr, "Error: Identificador invalido (se esperaba 'VMX26') o version no soportada.\n");
        fclose(arch);
        return 0;
    }

    // Resto de la cabecera de la versión 2
    if (version == 2 &&
        fread(header + HEADER_SIZE_V1, 1, HEADER_SIZE_V2 - HEADER_SIZE_V1, arch) != HEADER_SIZE_V2 - HEADER_SIZE_V1) {
        fprintf(stderr, "Error: Archivo incompleto o no se pudo leer la cabecera.\n");
        fclose(arch);
        return 0;
    }

    // Tamaños de los segmentos (campos de 16 bits en formato Big Endian)
    uint32_t tamSeg[CANT_SEGMENTOS] = {0};
    uint16_t entryPoint = 0;
    tamSeg[SEG_CODE] = ((uint32_t)header[6] << 8) | header[7];

    if (version == 1) {
        // Compatibilidad MV1: el Data Segment ocupa toda la memoria restante
        uint32_t resto = (mv->memSize > tamSeg[SEG_CODE]) ? mv->memSize - tamSeg[SEG_CODE] : 0;
        tamSeg[SEG_DATA] = (resto > 0xFFFF) ? 0xFFFF : resto;
    } else {
        tamSeg[SEG_DATA]  = ((uint32_t)header[8]  << 8) | header[9];
        tamSeg[SEG_EXTRA] = ((uint32_t)header[10] << 8) | header[11];
        tamSeg[SEG_STACK] = ((uint32_t)header[12] << 8) | header[13];
        tamSeg[SEG_CONST] = ((uint32_t)header[14] << 8) | header[15];
        entryPoint        = (uint16_t)(((uint16_t)header[16] << 8) | header[17]);

        // El Param Segment solo existe si hay parámetros (un -p sin nada detrás no lo crea)
        if (cantParam > 0) {
            tamSeg[SEG_PARAM] = calcularTamParam(nombreArch, cantParam, params);
            if (tamSeg[SEG_PARAM] > 0xFFFF) {
                fprintf(stderr, "Error: Los parametros exceden el tamano maximo de un segmento.\n");
                fclose(arch);
                return 0;
            }
        }
    }

    // El Code Segment siempre debe existir; sin él el registro CS quedaría en -1
    // y no habría entrada válida en la tabla
    if (tamSeg[SEG_CODE] == 0) {
        fprintf(stderr, "Error: El ejecutable debe tener Code Segment de tamano mayor a cero.\n");
        fclose(arch);
        return 0;
    }

    // El entry point debe apuntar dentro del Code Segment
    if (entryPoint >= tamSeg[SEG_CODE]) {
        fprintf(stderr, "Error: Entry point invalido (0x%04X) para un Code Segment de %u bytes.\n",
                entryPoint, tamSeg[SEG_CODE]);
        fclose(arch);
        return 0;
    }

    // Primero se valida que el proceso entre en la RAM (Memoria insuficiente)
    if (!inicializarSegmentosYRegistros(mv, tamSeg)) {
        fclose(arch);
        return 0;
    }

    // Recién después, el Stack Segment de la versión 2 debe existir y poder alojar la
    // precarga de la subrutina principal (retorno, argc y argv); la versión 1 no tiene pila
    if (version == 2 && tamSeg[SEG_STACK] < PILA_INICIAL) {
        fprintf(stderr, "Error: Stack overflow (la pila de %u bytes no alcanza para los %d bytes de la subrutina principal).\n",
                tamSeg[SEG_STACK], PILA_INICIAL);
        fclose(arch);
        return 0;
    }

    // Carga del código a partir de la base del Code Segment
    uint32_t entradaCS = ((uint32_t)mv->reg[CS] >> 16) & 0xFFFF;
    if (fread(mv->mem + mv->seg[entradaCS].base, 1, tamSeg[SEG_CODE], arch) != tamSeg[SEG_CODE]) {
        fprintf(stderr, "Error: El archivo termino inesperadamente al leer el codigo.\n");
        fclose(arch);
        return 0;
    }

    // Carga de las constantes, ubicadas en el archivo a continuación del código
    if (tamSeg[SEG_CONST] > 0) {
        uint32_t entradaKS = ((uint32_t)mv->reg[KS] >> 16) & 0xFFFF;
        if (fread(mv->mem + mv->seg[entradaKS].base, 1, tamSeg[SEG_CONST], arch) != tamSeg[SEG_CONST]) {
            fprintf(stderr, "Error: El archivo termino inesperadamente al leer las constantes.\n");
            fclose(arch);
            return 0;
        }
    }
    fclose(arch);

    if (tamSeg[SEG_PARAM] > 0) {
        cargarParamSegment(mv, nombreArch, cantParam, params);
    }

    // La versión 1 no tiene pila: solo la versión 2 arranca en la subrutina principal
    if (version == 2) {
        precargarPila(mv, nombreArch, cantParam, params);
    }

    // El IP arranca en el entry point: [entrada de CS | offset del entry point]
    mv->reg[IP] = mv->reg[CS] | entryPoint;
    return 1;
}

/* ========================================================================= */
/* IMAGEN DE LA MÁQUINA VIRTUAL (.vmi)                                       */
/* ========================================================================= */

/**
 * @brief Lee un valor de 4 bytes en formato Big Endian (operación inversa
 *        de escribirBigEndian32).
 */
static uint32_t leerBigEndian32(const uint8_t* origen) {
    return ((uint32_t)origen[0] << 24) | ((uint32_t)origen[1] << 16) |
           ((uint32_t)origen[2] << 8)  |  (uint32_t)origen[3];
}

/**
 * @brief Guarda el estado completo de la máquina en un archivo de imagen (.vmi):
 *        cabecera, 32 registros, tabla de segmentos y memoria principal.
 *        Todos los valores de más de un byte se guardan en Big Endian.
 *
 * @param mv        Puntero a la Máquina Virtual.
 * @param nombreVmi Nombre o ruta del archivo de imagen (se sobrescribe si existe).
 * @return int      1 si se pudo escribir, 0 ante error.
 */
int guardarImagen(TMV* mv, const char* nombreVmi) {
    FILE* arch = fopen(nombreVmi, "wb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo crear el archivo de imagen '%s'.\n", nombreVmi);
        return 0;
    }

    // 1. Cabecera: identificador, versión y tamaño de la memoria en KiB
    uint8_t header[HEADER_SIZE_VMI];
    uint32_t tamKiB = mv->memSize / 1024U;
    memcpy(header, "VMI26", 5);
    header[5] = VERSION_VMI;
    header[6] = (uint8_t)(tamKiB >> 8);
    header[7] = (uint8_t)tamKiB;

    // 2. Registros: 32 x 4 bytes
    uint8_t regs[32 * 4];
    for (int i = 0; i < 32; i++) {
        escribirBigEndian32(regs + 4 * i, (uint32_t)mv->reg[i]);
    }

    // 3. Tabla de descriptores: 8 x (base 2 bytes, tamaño 2 bytes)
    uint8_t tabla[SEG_AMOUNT * 4];
    for (int i = 0; i < SEG_AMOUNT; i++) {
        tabla[4 * i]     = (uint8_t)(mv->seg[i].base >> 8);
        tabla[4 * i + 1] = (uint8_t)mv->seg[i].base;
        tabla[4 * i + 2] = (uint8_t)(mv->seg[i].size >> 8);
        tabla[4 * i + 3] = (uint8_t)mv->seg[i].size;
    }

    // 4. Escritura de las secciones y, al final, la memoria completa
    int ok = fwrite(header, 1, sizeof(header), arch) == sizeof(header) &&
             fwrite(regs,   1, sizeof(regs),   arch) == sizeof(regs)   &&
             fwrite(tabla,  1, sizeof(tabla),  arch) == sizeof(tabla)  &&
             fwrite(mv->mem, 1, mv->memSize, arch) == mv->memSize;
    if (fclose(arch) != 0) ok = 0;

    if (!ok) {
        fprintf(stderr, "Error: No se pudo escribir el archivo de imagen '%s'.\n", nombreVmi);
    }
    return ok;
}

/**
 * @brief Carga el estado de la máquina desde un archivo de imagen (.vmi):
 *        crea la memoria con el tamaño de la cabecera y restaura registros,
 *        tabla de segmentos y memoria tal como están en el archivo.
 *
 * @param mv        Puntero a la Máquina Virtual (sin memoria creada).
 * @param nombreVmi Nombre o ruta del archivo de imagen.
 * @return int      1 si la carga fue exitosa, 0 ante cualquier error.
 */
int cargarImagen(TMV* mv, const char* nombreVmi) {
    FILE* arch = fopen(nombreVmi, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo de imagen '%s'.\n", nombreVmi);
        return 0;
    }

    // 1. Cabecera: identificador "VMI26" y versión 1
    uint8_t header[HEADER_SIZE_VMI];
    uint8_t regs[32 * 4];
    uint8_t tabla[SEG_AMOUNT * 4];
    if (fread(header, 1, sizeof(header), arch) != sizeof(header) ||
        memcmp(header, "VMI26", 5) != 0 || header[5] != VERSION_VMI) {
        fprintf(stderr, "Error: '%s' no es una imagen valida (se esperaba 'VMI26' version %d).\n",
                nombreVmi, VERSION_VMI);
        fclose(arch);
        return 0;
    }

    // 2. Registros y tabla de segmentos
    if (fread(regs, 1, sizeof(regs), arch) != sizeof(regs) ||
        fread(tabla, 1, sizeof(tabla), arch) != sizeof(tabla)) {
        fprintf(stderr, "Error: Archivo de imagen incompleto.\n");
        fclose(arch);
        return 0;
    }

    // 3. Memoria principal con el tamaño indicado en la cabecera (crearMemoria lo valida)
    uint32_t tamKiB = ((uint32_t)header[6] << 8) | header[7];
    if (!crearMemoria(mv, tamKiB)) {
        fclose(arch);
        return 0;
    }
    if (fread(mv->mem, 1, mv->memSize, arch) != mv->memSize) {
        fprintf(stderr, "Error: Archivo de imagen incompleto (memoria principal).\n");
        fclose(arch);
        return 0;
    }
    fclose(arch);

    // 4. Restauración de registros y tabla
    for (int i = 0; i < 32; i++) {
        mv->reg[i] = (int32_t)leerBigEndian32(regs + 4 * i);
    }
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = (uint16_t)((tabla[4 * i]     << 8) | tabla[4 * i + 1]);
        mv->seg[i].size = (uint16_t)((tabla[4 * i + 2] << 8) | tabla[4 * i + 3]);

        // Cada segmento activo debe estar dentro de la memoria: si no, la imagen está dañada
        int inactiva = mv->seg[i].base == ENTRY_INACTIVE && mv->seg[i].size == ENTRY_INACTIVE;
        if (!inactiva && (uint32_t)mv->seg[i].base + mv->seg[i].size > mv->memSize) {
            fprintf(stderr, "Error: La imagen tiene un segmento fuera de la memoria (entrada %d).\n", i);
            return 0;
        }
    }
    mv->errorFlag = 0;
    return 1;
}

/* ========================================================================= */
/* UNIDAD DE MANEJO DE MEMORIA (MMU)                                         */
/* ========================================================================= */

/**
 * @brief Traduce una dirección lógica a física y valida los límites de acceso.
 * 
 * Estructura de la dirección lógica:
 * - Bits 31..16: Índice en la tabla de descriptores de segmentos (0..7).
 * - Bits 15..0:  Desplazamiento (offset) relativo al inicio del segmento.
 * 
 * @param mv                Puntero a la Máquina Virtual.
 * @param dir_logica        Dirección de 32 bits a traducir.
 * @param cant_bytes_acceso Cantidad de bytes que se planean leer o escribir.
 * @return int32_t          Dirección física en RAM (>= 0) o -1 ante fallo de segmento.
 */
int32_t traducirDireccion(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes_acceso) {
    uint16_t indice_seg = (uint16_t)((dir_logica >> 16) & 0xFFFF);
    uint16_t offset     = (uint16_t)(dir_logica & 0xFFFF);

    // Validación de descriptor de segmento válido y activo
    if (indice_seg >= SEG_AMOUNT) return -1; 
    // (inactiva = base y tamaño en 0xFFFF; un segmento activo puede empezar en 0xFFFF)
    if (mv->seg[indice_seg].base == ENTRY_INACTIVE &&
        mv->seg[indice_seg].size == ENTRY_INACTIVE) return -1;

    uint32_t dir_base   = mv->seg[indice_seg].base;
    uint32_t tamano_seg = mv->seg[indice_seg].size;

    uint32_t dir_fisica       = dir_base + offset;
    uint32_t limite_segmento  = dir_base + tamano_seg;
    uint32_t limite_acceso    = dir_fisica + cant_bytes_acceso;

    // El acceso debe estar estrictamente dentro del tamaño asignado al segmento
    if (limite_acceso > limite_segmento) {
        return -1; 
    }

    return (int32_t)dir_fisica;
}

/* ========================================================================= */
/* CICLO DE EJECUCIÓN (FETCH - DECODE - EXECUTE)                             */
/* ========================================================================= */

/**
 * @brief Breakpoint: guarda la imagen en el .vmi y espera la acción del usuario.
 * - g:     continúa hasta el próximo breakpoint o el final.
 * - q:     aborta la ejecución (el .vmi queda intacto para retomarla).
 * - Enter: ejecuta la siguiente instrucción y vuelve a pausar (paso a paso).
 */
static void realizarBreakpoint(TMV* mv) {
    if (!guardarImagen(mv, mv->archVmi)) {
        mv->errorFlag = 1;
        return;
    }

    for (;;) {
        printf("[Breakpoint] g: continuar | q: salir | Enter: siguiente instruccion > ");
        fflush(stdout);

        int c = getchar();
        if (c != '\n' && c != EOF) {
            int resto;
            while ((resto = getchar()) != '\n' && resto != EOF);   // descarta el resto de la línea
        }

        if (c == '\n') {                        // Enter: paso a paso
            mv->pasoAPaso = 1;
            return;
        }
        if (c == 'g' || c == 'G' || c == EOF) { // go (o fin de la entrada): sigue sin pausar
            mv->pasoAPaso = 0;
            return;
        }
        if (c == 'q' || c == 'Q') {             // quit: termina como un STOP; la imagen ya quedó guardada
            mv->reg[IP] = -1;
            return;
        }
        // Cualquier otra tecla: se vuelve a preguntar
    }
}

/**
 * @brief Bucle principal de ejecución del procesador virtual.
 */
void ejecutarMV(TMV* mv) {
    // El Code Segment ya no ocupa siempre la entrada 0 de la tabla
    uint16_t seg_cod = (uint16_t)(((uint32_t)mv->reg[CS] >> 16) & 0xFFFF);
    uint32_t tamCod  = mv->seg[seg_cod].size;

    // El ciclo se repite mientras IP no sea -1 (0xFFFFFFFF) y no ocurra ningún error
    while (mv->reg[IP] != (int32_t)0xFFFFFFFF && mv->errorFlag == 0) {
        uint16_t seg_ip = (uint16_t)(((uint32_t)mv->reg[IP] >> 16) & 0xFFFF);
        uint16_t off_ip = (uint16_t)((uint32_t)mv->reg[IP] & 0xFFFF);

        // Fin normal de la ejecución si el puntero IP sale del Code Segment
        if (seg_ip != seg_cod || off_ip >= tamCod) {
            break;
        }

        // ---------------------------------------------------------------------
        // 1. FETCH: Búsqueda del primer byte de la instrucción apuntado por IP
        // ---------------------------------------------------------------------
        int32_t dir_fisica_ip = traducirDireccion(mv, (uint32_t)mv->reg[IP], 1);
        if (dir_fisica_ip < 0) {
            break; // Si no puede leer por límites, termina pacíficamente
        }

        uint8_t primer_byte = mv->mem[dir_fisica_ip];

        // ---------------------------------------------------------------------
        // 2. DECODE: Extracción de Opcode y tipos de operando
        // ---------------------------------------------------------------------
        uint8_t opcode   = primer_byte & 0x1F;        // Bits 0..4
        uint8_t tipo_opA = 0;
        uint8_t tipo_opB = 0;

        // Evaluación de aridad según el rango del OpCode
        if (opcode >= 0x10 && opcode <= 0x1F) {
            // 2 Operandos (MOV a RND): A en bits 4..5, B en bits 6..7
            tipo_opA = (primer_byte >> 4) & 0x03;
            tipo_opB = (primer_byte >> 6) & 0x03;
        } 
        else if (opcode <= 0x0D) {
            // 1 Operando (SYS a NOT, PUSH, POP y CALL): Único operando (A) en bits 6..7
            tipo_opA = (primer_byte >> 6) & 0x03;
            tipo_opB = 0;
        } 
        else if (opcode == 0x0F) {
            // 0 Operandos (STOP)
            tipo_opA = 0;
            tipo_opB = 0;
        }

        // La instrucción completa (byte de control + operandos) debe estar dentro
        // del Code Segment; si está cortada no se lee fuera de él
        if (traducirDireccion(mv, (uint32_t)mv->reg[IP], (uint16_t)(1 + tipo_opA + tipo_opB)) < 0) {
            printf("Error: Fallo de segmento (instruccion incompleta en 0x%08X).\n", (uint32_t)mv->reg[IP]);
            mv->errorFlag = 1;
            break;
        }

        // Los registros de instrucción almacenan la información decodificada
        mv->reg[OPC] = opcode;
        mv->reg[OP1] = (tipo_opA << 24);
        mv->reg[OP2] = (tipo_opB << 24);

        uint32_t offset_lectura = 1; // Ya leímos el byte de control
        int32_t valorB = 0;
        int32_t valorA = 0;

        // Lectura del Operando B (en memoria máquina viene primero por orden inverso)
        if (tipo_opB > 0) {
            for (int i = 0; i < tipo_opB; i++) {
                valorB = (valorB << 8) | mv->mem[dir_fisica_ip + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opB == 2) valorB = (int16_t)valorB; // Extensión si es inmediato
            mv->reg[OP2] |= (valorB & 0x00FFFFFF);
        }

        // Lectura del Operando A
        if (tipo_opA > 0) {
            for (int i = 0; i < tipo_opA; i++) {
                valorA = (valorA << 8) | mv->mem[dir_fisica_ip + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opA == 2) valorA = (int16_t)valorA; // Extensión si es inmediato
            mv->reg[OP1] |= (valorA & 0x00FFFFFF);
        }

        // ---------------------------------------------------------------------
        // 3. UPDATE IP: El IP avanza sumando los bytes de la instrucción actual
        // ---------------------------------------------------------------------
        mv->reg[IP] += offset_lectura;

        // ---------------------------------------------------------------------
        // 4. EXECUTE: Despacho a través de la tabla de punteros a función
        // ---------------------------------------------------------------------
        if (opcode >= 32 || tablaInstrucciones[opcode] == NULL) {
            printf("Error: Instruccion invalida (Opcode: 0x%02X)\n", opcode);
            mv->errorFlag = 1;
            break;
        }

        tablaInstrucciones[opcode](mv, tipo_opA, valorA, tipo_opB, valorB);

        // Breakpoint pedido por SYS F o modo paso a paso: se pausa después de la instrucción
        if (mv->errorFlag == 0 && (mv->bpPendiente || mv->pasoAPaso)) {
            mv->bpPendiente = 0;
            realizarBreakpoint(mv);
        }
    }
}

/* ========================================================================= */
/* PUNTO DE ENTRADA PRINCIPAL (MAIN)                                         */
/* ========================================================================= */

/**
 * @brief Indica si un nombre de archivo termina con la extensión dada
 *        (sin distinguir mayúsculas de minúsculas).
 */
static int tieneExtension(const char* nombre, const char* ext) {
    size_t largoNom = strlen(nombre);
    size_t largoExt = strlen(ext);
    if (largoNom < largoExt) return 0;

    const char* fin = nombre + largoNom - largoExt;
    for (size_t i = 0; i < largoExt; i++) {
        if (tolower((unsigned char)fin[i]) != tolower((unsigned char)ext[i])) return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    // Sintaxis: vmx [filename.vmx] [filename.vmi] [m=M] [-d] [-p param1 ... paramN]
    const char* archVmx  = NULL;
    const char* archVmi  = NULL;
    uint32_t    memKiB   = MEM_DEFAULT_KIB;
    int         disasm   = 0;
    int         cantParam = -1;   // -1: no se indicó el flag -p
    char**      params   = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            // -p siempre va al final: todo lo que sigue son parámetros del proceso
            params    = &argv[i + 1];
            cantParam = argc - i - 1;
            break;
        } else if (strcmp(argv[i], "-d") == 0) {
            disasm = 1;
        } else if (strncmp(argv[i], "m=", 2) == 0) {
            char* fin;
            long valor = strtol(argv[i] + 2, &fin, 10);
            if (*(argv[i] + 2) == '\0' || *fin != '\0' || valor <= 0 || valor > MEM_MAX_KIB) {
                fprintf(stderr, "Error: Tamano de memoria invalido '%s' (debe ser entre 1 y %d KiB).\n",
                        argv[i], MEM_MAX_KIB);
                return 1;
            }
            memKiB = (uint32_t)valor;
        } else if (tieneExtension(argv[i], ".vmx")) {
            archVmx = argv[i];
        } else if (tieneExtension(argv[i], ".vmi")) {
            archVmi = argv[i];
        } else {
            fprintf(stderr, "Error: Argumento no reconocido '%s'.\n", argv[i]);
            return 1;
        }
    }

    // Es obligatorio al menos uno de los dos archivos
    if (!archVmx && !archVmi) {
        printf("Uso: %s [filename.vmx] [filename.vmi] [m=M] [-d] [-p param1 ... paramN]\n", argv[0]);
        return 1;
    }

    // Inicialización del generador de números aleatorios para la instrucción RND
    srand((unsigned int)time(NULL));

    TMV mv = {0};          // mv.mem nace en NULL: liberarMemoria es seguro en cualquier salida
    mv.archVmi = archVmi;  // NULL si no se indicó: los breakpoints se ignoran

    if (archVmx) {
        // Programa nuevo: memoria de m=M KiB y carga del .vmx (el .vmi es solo para breakpoints)
        if (!crearMemoria(&mv, memKiB)) {
            return 1;
        }
        if (!cargarArchivo(&mv, archVmx, cantParam, params)) {
            liberarMemoria(&mv);
            return 1;
        }
    } else {
        // Solo .vmi: se retoma la ejecución; memoria, tabla y registros salen de la imagen
        // (el tamaño m=M y los parámetros -p se ignoran)
        if (!cargarImagen(&mv, archVmi)) {
            liberarMemoria(&mv);
            return 1;
        }
    }

    // Inicialización de la tabla de despacho de operaciones
    inicializarTablaInstrucciones();

    // Flag opcional de desensamblado (-d)
    if (disasm) {
        ejecutarDisassembler(&mv);
    }

    // Ejecución del programa en código máquina
    ejecutarMV(&mv);

    int codigoSalida = mv.errorFlag;
    liberarMemoria(&mv);
    return codigoSalida;
}