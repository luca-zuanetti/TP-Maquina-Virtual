#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "instrucciones.h"
#include "mv.h"  // <-- IMPORTANTE: Incluimos nuestro nuevo contrato

/* ========================================================================= */
/* DEFINICIÓN DE DICCIONARIOS (Sin la palabra 'static')                      */
/* ========================================================================= */
const char* regStr[32] = {
    "IP", "OPC", "OP1", "OP2", "LAR", "MAR", "MBR", "RESERVED", 
    "RESERVED", "RESERVED", "EAX", "EBX", "ECX", "EDX", "EEX", "EFX",
    "AC", "CC", "RESERVED", "RESERVED", "RESERVED", "RESERVED", 
    "RESERVED", "RESERVED", "RESERVED", "RESERVED", "CS", "DS", 
    "RESERVED", "RESERVED", "RESERVED", "RESERVED"
};

const char* opStr[32] = {
    "SYS", "JMP", "JP", "JN", "JZ", "JC", "JV", "JNP", "JNN", "JNZ", "NOT",
    "---", "---", "---", "---", "STOP", "MOV", "ADD", "SUB", "MUL", "DIV", 
    "CMP", "AND", "OR", "XOR", "SWAP", "SHL", "SHR", "SAR", "LDL", "LDH", "RND"
};

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
        int32_t valorB = 0;
        int32_t valorA = 0;
        // LECTURA DEL OPERANDO B
        if (tipo_opB > 0) {
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

        // =====================================================================
        // ETAPA 5: EJECUCIÓN USANDO LA TABLA DE DESPACHO
        // =====================================================================

        if (opcode >= 32 || tablaInstrucciones[opcode] == NULL) {
            printf("Error: Instruccion invalida o no implementada (opcode %02X)\n", opcode);
            mv->errorFlag = 1;
            break;
        }

        // Ejecuta directamente la función correspondiente
        tablaInstrucciones[opcode](mv, tipo_opA, valorA, tipo_opB, valorB);

        if (mv->errorFlag) {
            break; // Si hubo un error (como división por cero o fallo de segmento), corta la ejecución
        }
    } 
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Uso: %s filename.vmx [-d]\n", argv[0]);
        return 1;
    }

    TMV mv; // Instanciamos nuestra Máquina Virtual
    
    // Intentamos cargar el archivo binario
    if (!cargarArchivo(&mv, argv[1])) {
        return 1; // El error ya lo imprime la función de carga
    }



    inicializarTablaInstrucciones();

    // Verificamos si el usuario pasó el flag de desensamblador (-d)
    if (argc == 3 && strcmp(argv[2], "-d") == 0) {
        printf("Iniciando Desensamblador...\n");
        ejecutarDisassembler(&mv);
        // Ejecución normal de la máquina
    ejecutarMV(&mv);

    return 0;
}