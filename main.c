#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "mv.h"

/* ========================================================================= */
/* DICCIONARIOS GLOBALES DE NOMBRES Y MNEMÓNICOS                             */
/* ========================================================================= */

// Mapeo de nombres para los 32 registros según la arquitectura 2026
const char* regStr[32] = {
    "IP", "OPC", "OP1", "OP2", "LAR", "MAR", "MBR", "RESERVED", 
    "RESERVED", "RESERVED", "EAX", "EBX", "ECX", "EDX", "EEX", "EFX",
    "AC", "CC", "RESERVED", "RESERVED", "RESERVED", "RESERVED", 
    "RESERVED", "RESERVED", "RESERVED", "RESERVED", "CS", "DS", 
    "RESERVED", "RESERVED", "RESERVED", "RESERVED"
};

// Mapeo de mnemónicos para los 32 posibles códigos de operación (Opcodes)
const char* opStr[32] = {
    "SYS", "JMP", "JP", "JN", "JZ", "JC", "JV", "JNP", "JNN", "JNZ", "NOT",
    "---", "---", "---", "---", "STOP", "MOV", "ADD", "SUB", "MUL", "DIV", 
    "CMP", "AND", "OR", "XOR", "SWAP", "SHL", "SHR", "SAR", "LDL", "LDH", "RND"
};

/* ========================================================================= */
/* INICIALIZACIÓN DE LA MÁQUINA VIRTUAL                                      */
/* ========================================================================= */

/**
 * @brief Configura la memoria RAM física, la tabla de segmentos y los registros.
 * 
 * @param mv     Puntero a la Máquina Virtual.
 * @param tamCod Tamaño en bytes que ocupa el código cargado desde el archivo.
 */
void inicializarSegmentosYRegistros(TMV* mv, uint16_t tamCod) {
    // 1. Limpieza total de la memoria RAM (16 KiB a ceros)
    memset(mv->mem, 0, sizeof(mv->mem));

    // 2. Inicialización de la tabla de descriptores de segmentos:
    //    Las entradas inactivas deben contener 0xFFFF en base y tamaño.
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = ENTRY_INACTIVE;
        mv->seg[i].size = ENTRY_INACTIVE;
    }

    // 3. Segmento 0 (Código - CS): Base 0, tamaño = tamaño del código
    mv->seg[0].base = 0;
    mv->seg[0].size = tamCod;

    // 4. Segmento 1 (Datos - DS): Base contigua al código, tamaño = RAM restante
    mv->seg[1].base = tamCod;
    mv->seg[1].size = RAM_SIZE - tamCod;

    // 5. Reinicio del banco de registros y estado de error
    memset(mv->reg, 0, sizeof(mv->reg));
    mv->errorFlag = 0;

    // 6. Configuración de punteros lógicos (16 bits índice de segmento | 16 bits offset)
    mv->reg[CS] = 0x00000000;  // Segmento 0, Offset 0
    mv->reg[DS] = 0x00010000;  // Segmento 1, Offset 0
    mv->reg[IP] = mv->reg[CS]; // El puntero de instrucción comienza en la base de CS
}

/**
 * @brief Lee el archivo binario ejecutable (.vmx) y carga el código en RAM.
 * 
 * @param mv         Puntero a la Máquina Virtual.
 * @param nombreArch Nombre o ruta del archivo binario a ejecutar.
 * @return int       1 si la carga fue exitosa, 0 ante cualquier error.
 */
int cargarArchivo(TMV* mv, const char* nombreArch) {
    FILE* arch = fopen(nombreArch, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo '%s'.\n", nombreArch);
        return 0;
    }

    // Lectura de la cabecera obligatoria de 8 bytes
    uint8_t header[HEADER_SIZE];
    if (fread(header, 1, HEADER_SIZE, arch) != HEADER_SIZE) {
        fprintf(stderr, "Error: Archivo incompleto o no se pudo leer la cabecera.\n");
        fclose(arch);
        return 0;
    }

    // Verificación de la firma "VMX26" y versión 1
    if (memcmp(header, "VMX26", 5) != 0 || header[5] != 1) {
        fprintf(stderr, "Error: Identificador invalido (se esperaba 'VMX26') o version no soportada.\n");
        fclose(arch);
        return 0;
    }

    // Tamaño del código en bytes (formato Big Endian)
    uint16_t tamCod = ((uint16_t)header[6] << 8) | header[7];

    if (tamCod > RAM_SIZE) {
        fprintf(stderr, "Error: El tamano del codigo (%u bytes) excede la memoria RAM (%d bytes).\n", 
                tamCod, RAM_SIZE);
        fclose(arch);
        return 0;
    }

    inicializarSegmentosYRegistros(mv, tamCod);

    // Carga de las instrucciones en la memoria física a partir de la base de CS
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
    uint16_t indice_seg = (dir_logica >> 16) & 0xFFFF;
    uint16_t offset     = dir_logica & 0xFFFF;

    // Validación de descriptor de segmento válido y activo
    if (indice_seg >= SEG_AMOUNT) return -1; 
    if (mv->seg[indice_seg].base == ENTRY_INACTIVE) return -1; 

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
 * @brief Bucle principal de ejecución del procesador virtual.
 */
void ejecutarMV(TMV* mv) {
    uint32_t tamCod = mv->seg[0].size;

    // El ciclo se repite mientras IP no sea -1 (0xFFFFFFFF) y no ocurra ningún error
    while (mv->reg[IP] != (int32_t)0xFFFFFFFF && mv->errorFlag == 0) {
        uint16_t seg_ip = ((uint32_t)mv->reg[IP] >> 16) & 0xFFFF;
        uint16_t off_ip = (uint32_t)mv->reg[IP] & 0xFFFF;

        // Fin normal de la ejecución si el puntero IP sobrepasa el tamaño del código
        if (seg_ip != 0 || off_ip >= tamCod) {
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
        uint8_t tipo_opA = (primer_byte >> 4) & 0x03; // Bits 4..5
        uint8_t tipo_opB = (primer_byte >> 6) & 0x03; // Bits 6..7

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
    }
}

/* ========================================================================= */
/* PUNTO DE ENTRADA PRINCIPAL (MAIN)                                         */
/* ========================================================================= */

int main(int argc, char** argv) {
    // Validación de argumentos mínimos por consola
    if (argc < 2) {
        printf("Uso: %s filename.vmx [-d]\n", argv[0]);
        return 1;
    }

    // Inicialización del generador de números aleatorios para la instrucción RND
    srand((unsigned int)time(NULL));

    TMV mv;

    // Carga del programa en la memoria de la máquina virtual
    if (!cargarArchivo(&mv, argv[1])) {
        return 1;
    }

    // Inicialización de la tabla de despacho de operaciones
    inicializarTablaInstrucciones();

    // Verificación del flag opcional de desensamblado (-d)
    if (argc >= 3 && strcmp(argv[2], "-d") == 0) {
        ejecutarDisassembler(&mv);
    }

    // Ejecución del programa en código máquina
    ejecutarMV(&mv);

    return mv.errorFlag;
}