#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "mv.h"

/* ========================================================================= */
/* NOMBRES DE REGISTROS Y MNEMÓNICOS                                         */
/* ========================================================================= */

// Código de registro -> nombre
const char* regStr[32] = {
    "IP", "OPC", "OP1", "OP2", "LAR", "MAR", "MBR", "SP",
    "BP", "RESERVED", "EAX", "EBX", "ECX", "EDX", "EEX", "EFX",
    "AC", "CC", "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "RESERVED", "RESERVED", "RESERVED", "RESERVED", "CS", "DS",
    "ES", "SS", "KS", "PS"
};

// Opcode -> mnemónico
const char* opStr[32] = {
    "SYS", "JMP", "JP", "JN", "JZ", "JC", "JV", "JNP", "JNN", "JNZ", "NOT",
    "PUSH", "POP", "CALL", "RET", "STOP", "MOV", "ADD", "SUB", "MUL", "DIV",
    "CMP", "AND", "OR", "XOR", "SWAP", "SHL", "SHR", "SAR", "LDL", "LDH", "RND"
};

// Registro puntero de cada segmento (mismo orden que SegProceso)
static const RegName regSegmento[CANT_SEGMENTOS] = { PS, KS, CS, DS, ES, SS };

/* ========================================================================= */
/* MEMORIA PRINCIPAL                                                         */
/* ========================================================================= */

/* Reserva la RAM de M KiB (m=M o cabecera del .vmi). 1 = OK, 0 = error. */
int crearMemoria(TMV* mv, uint32_t tamKiB) {
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

/* Libera la RAM. */
void liberarMemoria(TMV* mv) {
    free(mv->mem);
    mv->mem = NULL;
    mv->memSize = 0;
}

/* ========================================================================= */
/* SEGMENTOS Y REGISTROS                                                     */
/* ========================================================================= */

/* Ubica los segmentos contiguos (PS, KS, CS, DS, ES, SS), arma la tabla y carga
   los registros puntero. Solo los segmentos con tamaño > 0 tienen entrada. */
int inicializarSegmentosYRegistros(TMV* mv, const uint32_t tamSeg[CANT_SEGMENTOS]) {
    // 1. El proceso tiene que entrar en la RAM...
    uint32_t total = 0;
    for (int i = 0; i < CANT_SEGMENTOS; i++) {
        total += tamSeg[i];
    }
    if (total > mv->memSize) {
        fprintf(stderr, "Error: Memoria insuficiente (el proceso requiere %u bytes y la memoria tiene %u bytes).\n",
                total, mv->memSize);
        return 0;
    }
    // ...y en los primeros 64 KiB (bases de 16 bits)
    if (total > ESPACIO_DIRECCIONABLE) {
        fprintf(stderr, "Error: Memoria insuficiente (el proceso requiere %u bytes y solo se pueden direccionar %u bytes).\n",
                total, ESPACIO_DIRECCIONABLE);
        return 0;
    }

    // 2. Limpieza: RAM y registros en 0, entradas de tabla inactivas
    memset(mv->mem, 0, mv->memSize);
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = ENTRY_INACTIVE;
        mv->seg[i].size = ENTRY_INACTIVE;
    }
    memset(mv->reg, 0, sizeof(mv->reg));
    mv->errorFlag = 0;

    // 3. Cada segmento presente toma la siguiente entrada; su registro = [entrada | offset 0].
    //    Segmento ausente: registro en -1
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

    // 4. Pila vacía: SP apunta al final del Stack Segment
    if (tamSeg[SEG_STACK] > 0) {
        mv->reg[SP] = mv->reg[SS] + (int32_t)tamSeg[SEG_STACK];
    } else {
        mv->reg[SP] = -1;
    }

    // 5. IP al inicio del Code Segment (el loader le suma el entry point)
    mv->reg[IP] = mv->reg[CS];
    return 1;
}

/* Escribe 4 bytes en Big Endian. */
static void escribirBigEndian32(uint8_t* destino, uint32_t valor) {
    destino[0] = (uint8_t)(valor >> 24);
    destino[1] = (uint8_t)(valor >> 16);
    destino[2] = (uint8_t)(valor >> 8);
    destino[3] = (uint8_t)valor;
}

/* Tamaño del Param Segment: strings (nombre del .vmx + parámetros) + arreglo argv. */
static uint32_t calcularTamParam(const char* nombreArch, int cantParam, char** params) {
    uint32_t tam = (uint32_t)strlen(nombreArch) + 1;
    for (int i = 0; i < cantParam; i++) {
        tam += (uint32_t)strlen(params[i]) + 1;
    }
    return tam + 4U * (uint32_t)(cantParam + 1);
}

/* Llena el Param Segment: primero los strings, después argv (punteros lógicos). */
static void cargarParamSegment(TMV* mv, const char* nombreArch, int cantParam, char** params) {
    uint32_t entradaPS = ((uint32_t)mv->reg[PS] >> 16) & 0xFFFF;
    uint8_t* inicio    = mv->mem + mv->seg[entradaPS].base;
    uint32_t offset    = 0;

    // 1. Strings terminados en '\0'
    for (int i = 0; i <= cantParam; i++) {
        const char* str = (i == 0) ? nombreArch : params[i - 1];
        size_t largo = strlen(str) + 1;
        memcpy(inicio + offset, str, largo);
        offset += (uint32_t)largo;
    }

    // 2. argv: un puntero [entrada PS | offset] por string
    uint32_t offsetStr = 0;
    for (int i = 0; i <= cantParam; i++) {
        const char* str = (i == 0) ? nombreArch : params[i - 1];
        escribirBigEndian32(inicio + offset, (entradaPS << 16) | offsetStr);
        offset    += 4;
        offsetStr += (uint32_t)strlen(str) + 1;
    }
}

/* Prepara la pila del "main" del programa: [SP] retorno -1, [SP+4] argc, [SP+8] argv.
   Sin parámetros: argc = 0 y argv = -1. Escribe directo en RAM (es el loader). */
static void precargarPila(TMV* mv, const char* nombreArch, int cantParam, char** params) {
    uint32_t entradaSS = ((uint32_t)mv->reg[SS] >> 16) & 0xFFFF;
    uint32_t tamSS     = mv->seg[entradaSS].size;

    int32_t argc   = 0;
    int32_t ptrArgv = -1;
    if (cantParam > 0) {
        // argv está después de todos los strings
        uint32_t offsetArgv = (uint32_t)strlen(nombreArch) + 1;
        for (int i = 0; i < cantParam; i++) {
            offsetArgv += (uint32_t)strlen(params[i]) + 1;
        }
        argc    = cantParam + 1;
        ptrArgv = (int32_t)((uint32_t)mv->reg[PS] | offsetArgv);
    }

    mv->reg[SP] -= PILA_INICIAL;
    uint8_t* tope = mv->mem + mv->seg[entradaSS].base + (tamSS - PILA_INICIAL);
    escribirBigEndian32(tope,     0xFFFFFFFFU);        // RET final -> IP = -1 -> fin
    escribirBigEndian32(tope + 4, (uint32_t)argc);
    escribirBigEndian32(tope + 8, (uint32_t)ptrArgv);
}

/* LOADER: lee el .vmx, valida la cabecera, crea los segmentos y carga código,
   constantes y parámetros. V1: solo CS + DS con el resto de la RAM.
   V2: tamaños de todos los segmentos + entry point + precarga de pila. */
int cargarArchivo(TMV* mv, const char* nombreArch, int cantParam, char** params) {
    FILE* arch = fopen(nombreArch, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo '%s'.\n", nombreArch);
        return 0;
    }

    // Cabecera común: "VMX26", versión y tamaño del código
    uint8_t header[HEADER_SIZE_V2];
    if (fread(header, 1, HEADER_SIZE_V1, arch) != HEADER_SIZE_V1) {
        fprintf(stderr, "Error: Archivo incompleto o no se pudo leer la cabecera.\n");
        fclose(arch);
        return 0;
    }

    uint8_t version = header[5];
    if (memcmp(header, "VMX26", 5) != 0 || (version != 1 && version != 2)) {
        fprintf(stderr, "Error: Identificador invalido (se esperaba 'VMX26') o version no soportada.\n");
        fclose(arch);
        return 0;
    }

    // V2: 10 bytes más de cabecera
    if (version == 2 &&
        fread(header + HEADER_SIZE_V1, 1, HEADER_SIZE_V2 - HEADER_SIZE_V1, arch) != HEADER_SIZE_V2 - HEADER_SIZE_V1) {
        fprintf(stderr, "Error: Archivo incompleto o no se pudo leer la cabecera.\n");
        fclose(arch);
        return 0;
    }

    // Tamaños de segmento (16 bits Big Endian)
    uint32_t tamSeg[CANT_SEGMENTOS] = {0};
    uint16_t entryPoint = 0;
    tamSeg[SEG_CODE] = ((uint32_t)header[6] << 8) | header[7];

    if (version == 1) {
        // V1: el Data Segment ocupa el resto de la memoria (tope 64 KiB)
        uint32_t limite = (mv->memSize < ESPACIO_DIRECCIONABLE) ? mv->memSize : ESPACIO_DIRECCIONABLE;
        uint32_t resto  = (limite > tamSeg[SEG_CODE]) ? limite - tamSeg[SEG_CODE] : 0;
        tamSeg[SEG_DATA] = (resto > 0xFFFF) ? 0xFFFF : resto;
    } else {
        tamSeg[SEG_DATA]  = ((uint32_t)header[8]  << 8) | header[9];
        tamSeg[SEG_EXTRA] = ((uint32_t)header[10] << 8) | header[11];
        tamSeg[SEG_STACK] = ((uint32_t)header[12] << 8) | header[13];
        tamSeg[SEG_CONST] = ((uint32_t)header[14] << 8) | header[15];
        entryPoint        = (uint16_t)(((uint16_t)header[16] << 8) | header[17]);

        // Param Segment solo si hay parámetros tras -p
        if (cantParam > 0) {
            tamSeg[SEG_PARAM] = calcularTamParam(nombreArch, cantParam, params);
            if (tamSeg[SEG_PARAM] > 0xFFFF) {
                fprintf(stderr, "Error: Los parametros exceden el tamano maximo de un segmento.\n");
                fclose(arch);
                return 0;
            }
        }
    }

    // Validaciones: CS obligatorio y entry point dentro de CS
    if (tamSeg[SEG_CODE] == 0) {
        fprintf(stderr, "Error: El ejecutable debe tener Code Segment de tamano mayor a cero.\n");
        fclose(arch);
        return 0;
    }

    if (entryPoint >= tamSeg[SEG_CODE]) {
        fprintf(stderr, "Error: Entry point invalido (0x%04X) para un Code Segment de %u bytes.\n",
                entryPoint, tamSeg[SEG_CODE]);
        fclose(arch);
        return 0;
    }

    // Arma segmentos y registros (falla si no hay memoria suficiente)
    if (!inicializarSegmentosYRegistros(mv, tamSeg)) {
        fclose(arch);
        return 0;
    }

    // V2: la pila debe alojar retorno, argc y argv
    if (version == 2 && tamSeg[SEG_STACK] < PILA_INICIAL) {
        fprintf(stderr, "Error: Stack overflow (la pila de %u bytes no alcanza para los %d bytes de la subrutina principal).\n",
                tamSeg[SEG_STACK], PILA_INICIAL);
        fclose(arch);
        return 0;
    }

    // Código -> Code Segment
    uint32_t entradaCS = ((uint32_t)mv->reg[CS] >> 16) & 0xFFFF;
    if (fread(mv->mem + mv->seg[entradaCS].base, 1, tamSeg[SEG_CODE], arch) != tamSeg[SEG_CODE]) {
        fprintf(stderr, "Error: El archivo termino inesperadamente al leer el codigo.\n");
        fclose(arch);
        return 0;
    }

    // Constantes (siguen al código en el archivo) -> Const Segment
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

    // Solo V2 tiene pila
    if (version == 2) {
        precargarPila(mv, nombreArch, cantParam, params);
    }

    // IP = [entrada CS | entry point]
    mv->reg[IP] = mv->reg[CS] | entryPoint;
    return 1;
}

/* ========================================================================= */
/* IMAGEN DE LA MÁQUINA (.vmi)                                               */
/* ========================================================================= */

/* Lee 4 bytes en Big Endian. */
static uint32_t leerBigEndian32(const uint8_t* origen) {
    return ((uint32_t)origen[0] << 24) | ((uint32_t)origen[1] << 16) |
           ((uint32_t)origen[2] << 8)  |  (uint32_t)origen[3];
}

/* Guarda el estado completo en el .vmi: cabecera, registros, tabla y RAM. */
int guardarImagen(TMV* mv, const char* nombreVmi) {
    FILE* arch = fopen(nombreVmi, "wb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo crear el archivo de imagen '%s'.\n", nombreVmi);
        return 0;
    }

    // 1. Cabecera: "VMI26", versión, tamaño de memoria en KiB
    uint8_t header[HEADER_SIZE_VMI];
    uint32_t tamKiB = mv->memSize / 1024U;
    memcpy(header, "VMI26", 5);
    header[5] = VERSION_VMI;
    header[6] = (uint8_t)(tamKiB >> 8);
    header[7] = (uint8_t)tamKiB;

    // 2. 32 registros x 4 bytes
    uint8_t regs[32 * 4];
    for (int i = 0; i < 32; i++) {
        escribirBigEndian32(regs + 4 * i, (uint32_t)mv->reg[i]);
    }

    // 3. Tabla: 8 x (base, tamaño)
    uint8_t tabla[SEG_AMOUNT * 4];
    for (int i = 0; i < SEG_AMOUNT; i++) {
        tabla[4 * i]     = (uint8_t)(mv->seg[i].base >> 8);
        tabla[4 * i + 1] = (uint8_t)mv->seg[i].base;
        tabla[4 * i + 2] = (uint8_t)(mv->seg[i].size >> 8);
        tabla[4 * i + 3] = (uint8_t)mv->seg[i].size;
    }

    // 4. Escritura en orden y, al final, la RAM completa
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

/* Restaura el estado desde un .vmi para retomar una ejecución pausada. */
int cargarImagen(TMV* mv, const char* nombreVmi) {
    FILE* arch = fopen(nombreVmi, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo de imagen '%s'.\n", nombreVmi);
        return 0;
    }

    // 1. Cabecera "VMI26" versión 1
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

    // 2. Registros y tabla
    if (fread(regs, 1, sizeof(regs), arch) != sizeof(regs) ||
        fread(tabla, 1, sizeof(tabla), arch) != sizeof(tabla)) {
        fprintf(stderr, "Error: Archivo de imagen incompleto.\n");
        fclose(arch);
        return 0;
    }

    // 3. RAM del tamaño indicado en la cabecera
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

    // 4. Restaura registros y tabla
    for (int i = 0; i < 32; i++) {
        mv->reg[i] = (int32_t)leerBigEndian32(regs + 4 * i);
    }
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = (uint16_t)((tabla[4 * i]     << 8) | tabla[4 * i + 1]);
        mv->seg[i].size = (uint16_t)((tabla[4 * i + 2] << 8) | tabla[4 * i + 3]);

        // Imagen dañada: segmento fuera de la RAM
        int inactiva = mv->seg[i].base == ENTRY_INACTIVE && mv->seg[i].size == ENTRY_INACTIVE;
        if (!inactiva && (uint32_t)mv->seg[i].base + mv->seg[i].size > mv->memSize) {
            fprintf(stderr, "Error: La imagen tiene un segmento fuera de la memoria (entrada %d).\n", i);
            return 0;
        }
    }

    // Imagen dañada: CS debe apuntar a una entrada activa
    uint32_t entradaCS = ((uint32_t)mv->reg[CS] >> 16) & 0xFFFF;
    if (entradaCS >= SEG_AMOUNT ||
        (mv->seg[entradaCS].base == ENTRY_INACTIVE && mv->seg[entradaCS].size == ENTRY_INACTIVE)) {
        fprintf(stderr, "Error: La imagen tiene un registro CS invalido (0x%08X).\n", (uint32_t)mv->reg[CS]);
        return 0;
    }
    mv->errorFlag = 0;
    return 1;
}

/* ========================================================================= */
/* MMU: TRADUCCIÓN DE DIRECCIONES                                            */
/* ========================================================================= */

/* Lógica [entrada (16 bits) | offset (16 bits)] -> física = base + offset.
   Devuelve -1 (fallo de segmento) si la entrada no existe o el acceso se sale del segmento. */
int32_t traducirDireccion(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes_acceso) {
    uint16_t indice_seg = (uint16_t)((dir_logica >> 16) & 0xFFFF);
    uint16_t offset     = (uint16_t)(dir_logica & 0xFFFF);

    // Entrada válida y activa
    if (indice_seg >= SEG_AMOUNT) return -1;
    if (mv->seg[indice_seg].base == ENTRY_INACTIVE &&
        mv->seg[indice_seg].size == ENTRY_INACTIVE) return -1;

    uint32_t dir_base   = mv->seg[indice_seg].base;
    uint32_t tamano_seg = mv->seg[indice_seg].size;

    uint32_t dir_fisica       = dir_base + offset;
    uint32_t limite_segmento  = dir_base + tamano_seg;
    uint32_t limite_acceso    = dir_fisica + cant_bytes_acceso;

    // Todos los bytes accedidos deben caer dentro del segmento
    if (limite_acceso > limite_segmento) {
        return -1;
    }

    return (int32_t)dir_fisica;
}

/* ========================================================================= */
/* CICLO DE EJECUCIÓN (FETCH - DECODE - EXECUTE)                             */
/* ========================================================================= */

/* Breakpoint: guarda la imagen y pregunta. g = continuar, q = salir, Enter = paso a paso. */
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

        if (c == '\n') {                        // Enter: pausa en la próxima instrucción
            mv->pasoAPaso = 1;
            return;
        }
        if (c == 'g' || c == 'G' || c == EOF) { // g: sigue sin pausar
            mv->pasoAPaso = 0;
            return;
        }
        if (c == 'q' || c == 'Q') {             // q: termina como STOP (el .vmi queda guardado)
            mv->reg[IP] = -1;
            return;
        }
        // Otra tecla: vuelve a preguntar
    }
}

/* CPU: repite fetch -> decode -> avance de IP -> execute hasta STOP, error o salir de CS. */
void ejecutarMV(TMV* mv) {
    uint16_t seg_cod = (uint16_t)(((uint32_t)mv->reg[CS] >> 16) & 0xFFFF);
    uint32_t tamCod  = mv->seg[seg_cod].size;

    // Corre mientras IP != -1 y no haya error
    while (mv->reg[IP] != (int32_t)0xFFFFFFFF && mv->errorFlag == 0) {
        uint16_t seg_ip = (uint16_t)(((uint32_t)mv->reg[IP] >> 16) & 0xFFFF);
        uint16_t off_ip = (uint16_t)((uint32_t)mv->reg[IP] & 0xFFFF);

        // IP fuera del Code Segment: fin normal
        if (seg_ip != seg_cod || off_ip >= tamCod) {
            break;
        }

        // ---------------------------------------------------------------------
        // 1. FETCH: byte de control apuntado por IP
        // ---------------------------------------------------------------------
        int32_t dir_fisica_ip = traducirDireccion(mv, (uint32_t)mv->reg[IP], 1);
        if (dir_fisica_ip < 0) {
            break;
        }

        uint8_t primer_byte = mv->mem[dir_fisica_ip];

        // ---------------------------------------------------------------------
        // 2. DECODE: opcode (5 bits bajos) y tipos de operando (2 bits c/u)
        // ---------------------------------------------------------------------
        uint8_t opcode   = primer_byte & 0x1F;        // Bits 0..4
        uint8_t tipo_opA = 0;
        uint8_t tipo_opB = 0;

        if (opcode >= 0x10 && opcode <= 0x1F) {
            // 2 operandos: B en bits 7-6, A en bits 5-4
            tipo_opA = (primer_byte >> 4) & 0x03;
            tipo_opB = (primer_byte >> 6) & 0x03;
        }
        else if (opcode <= 0x0D) {
            // 1 operando: A en bits 7-6
            tipo_opA = (primer_byte >> 6) & 0x03;
            tipo_opB = 0;
        }
        else if (opcode == 0x0F) {
            // 0 operandos (STOP; RET también queda en 0)
            tipo_opA = 0;
            tipo_opB = 0;
        }

        // Inválida si le falta un operando obligatorio
        int faltaOperando = (opcode <= 0x0D && tipo_opA == 0) || (opcode >= 0x10 && tipo_opB == 0);
        if (faltaOperando) {
            printf("Error: Instruccion invalida (Opcode: 0x%02X)\n", opcode);
            mv->errorFlag = 1;
            break;
        }

        // Inválida si la instrucción queda cortada al final del Code Segment
        if (traducirDireccion(mv, (uint32_t)mv->reg[IP], (uint16_t)(1 + tipo_opA + tipo_opB)) < 0) {
            printf("Error: Instruccion invalida (incompleta al final del Code Segment en 0x%08X).\n",
                   (uint32_t)mv->reg[IP]);
            mv->errorFlag = 1;
            break;
        }

        // OPC, OP1 y OP2 = [tipo (8 bits) | operando tal cual está codificado]
        mv->reg[OPC] = opcode;
        mv->reg[OP1] = (tipo_opA << 24);
        mv->reg[OP2] = (tipo_opB << 24);

        uint32_t offset_lectura = 1; // byte de control ya leído
        int32_t valorB = 0;
        int32_t valorA = 0;

        // Operando B (viene primero en memoria)
        if (tipo_opB > 0) {
            for (int i = 0; i < tipo_opB; i++) {
                valorB = (valorB << 8) | mv->mem[dir_fisica_ip + offset_lectura];
                offset_lectura++;
            }
            mv->reg[OP2] |= valorB;
            if (tipo_opB == 2) valorB = (int16_t)valorB; // inmediato: extensión de signo
        }

        // Operando A
        if (tipo_opA > 0) {
            for (int i = 0; i < tipo_opA; i++) {
                valorA = (valorA << 8) | mv->mem[dir_fisica_ip + offset_lectura];
                offset_lectura++;
            }
            mv->reg[OP1] |= valorA;
            if (tipo_opA == 2) valorA = (int16_t)valorA; // inmediato: extensión de signo
        }

        // ---------------------------------------------------------------------
        // 3. UPDATE IP: avanza al inicio de la próxima instrucción
        // ---------------------------------------------------------------------
        mv->reg[IP] += offset_lectura;

        // ---------------------------------------------------------------------
        // 4. EXECUTE: llama a la función del opcode por la tabla de despacho
        // ---------------------------------------------------------------------
        if (tablaInstrucciones[opcode] == NULL) {
            printf("Error: Instruccion invalida (Opcode: 0x%02X)\n", opcode);
            mv->errorFlag = 1;
            break;
        }

        tablaInstrucciones[opcode](mv, tipo_opA, valorA, tipo_opB, valorB);

        // Breakpoint (SYS F o paso a paso): pausa después de ejecutar
        if (mv->errorFlag == 0 && (mv->bpPendiente || mv->pasoAPaso)) {
            mv->bpPendiente = 0;
            realizarBreakpoint(mv);
        }
    }
}

/* ========================================================================= */
/* MAIN                                                                      */
/* ========================================================================= */

/* ¿El nombre termina en la extensión dada? (sin distinguir mayúsculas) */
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

/* Flujo: 1) leer argumentos  2) cargar .vmx o .vmi  3) armar tabla de instrucciones
          4) desensamblar si -d  5) ejecutar  6) liberar memoria. */
int main(int argc, char** argv) {
    // Uso: vmx [filename.vmx] [filename.vmi] [m=M] [-d] [-p param1 ... paramN]
    const char* archVmx  = NULL;
    const char* archVmi  = NULL;
    uint32_t    memKiB   = MEM_DEFAULT_KIB;
    int         disasm   = 0;
    int         cantParam = -1;   // -1: no se usó -p
    char**      params   = NULL;

    // 1. Lectura de argumentos
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            // -p va al final: lo que sigue son parámetros del programa
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
        } else if (tieneExtension(argv[i], ".vmx") || tieneExtension(argv[i], ".vmi")) {
            // A lo sumo un archivo de cada tipo
            const char** destino = tieneExtension(argv[i], ".vmx") ? &archVmx : &archVmi;
            if (*destino != NULL) {
                fprintf(stderr, "Error: Argumento repetido '%s' (ya se indico '%s').\n", argv[i], *destino);
                return 1;
            }
            *destino = argv[i];
        } else {
            fprintf(stderr, "Error: Argumento no reconocido '%s'.\n", argv[i]);
            return 1;
        }
    }

    // Hace falta al menos un .vmx o un .vmi
    if (!archVmx && !archVmi) {
        printf("Uso: %s [filename.vmx] [filename.vmi] [m=M] [-d] [-p param1 ... paramN]\n", argv[0]);
        return 1;
    }

    // Inicialización del generador de números aleatorios para la instrucción RND
    srand((unsigned int)time(NULL));

    TMV mv = {0};
    mv.archVmi = archVmi;  // sin .vmi: SYS F se ignora

    // 2. Carga
    if (archVmx) {
        // Programa nuevo: RAM de m=M KiB + loader del .vmx
        if (!crearMemoria(&mv, memKiB)) {
            return 1;
        }
        if (!cargarArchivo(&mv, archVmx, cantParam, params)) {
            liberarMemoria(&mv);
            return 1;
        }
    } else {
        // Solo .vmi: retoma la ejecución desde la imagen (ignora m=M y -p)
        if (!cargarImagen(&mv, archVmi)) {
            liberarMemoria(&mv);
            return 1;
        }
    }

    // 3. Inicialización de la tabla de despacho de operaciones
    inicializarTablaInstrucciones();

    // 4. Disassembler (-d)
    if (disasm) {
        ejecutarDisassembler(&mv);
    }

    // 5. Ejecución
    ejecutarMV(&mv);

    // 6. Fin: código de salida 1 si hubo error
    int codigoSalida = mv.errorFlag;
    liberarMemoria(&mv);
    return codigoSalida;
}
