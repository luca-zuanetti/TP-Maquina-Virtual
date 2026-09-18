#include <stdio.h>
#include <stdint.h>
#include "mv.h" // Incluimos nuestro contrato de arquitectura

// Función auxiliar para imprimir un operando decodificado
void imprimirOperando(uint8_t tipo, int32_t valor) {
    if (tipo == 1) { 
        // Operando Registro (1 byte)
        int regCode = valor & 0x1F;
        if (regCode >= 0 && regCode < 32) {
            printf("%s", regStr[regCode]);
        }
    } 
    else if (tipo == 2) { 
        // Operando Inmediato (2 bytes)
        // Se castea a int16_t para forzar el signo negativo si corresponde
        printf("%d", (int16_t)valor);
    } 
    else if (tipo == 3) { 
        // Operando Memoria (3 bytes)
        // Los primeros 2 bytes (16 bits) son el offset, el último byte es el registro[cite: 9].
        int regCode = valor & 0x1F; // 5 bits menos significativos del último byte[cite: 9]
        int16_t offset = (int16_t)(valor >> 8); // Desplazamos 8 bits para quedarnos con el offset[cite: 9]
        
        if (regCode >= 0 && regCode < 32) {
            if (offset > 0) {
                printf("[%s+%d]", regStr[regCode], offset);
            } else if (offset < 0) {
                printf("[%s%d]", regStr[regCode], offset); // El %d ya imprime el signo menos
            } else {
                printf("[%s]", regStr[regCode]);
            }
        } else {
            printf("[%d]", offset);
        }
    }
}

// Función principal del desensamblador
void ejecutarDisassembler(TMV* mv) {
    uint16_t csIndex = 0; // Segmento de código
    uint32_t baseCod = mv->seg[csIndex].base;
    uint32_t tamCod  = mv->seg[csIndex].size;
    
    uint32_t offset_actual = 0;
    
    printf("Iniciando Desensamblador...\n");
    printf("--------------------------------------------------\n");
    
    while (offset_actual < tamCod) {
        // Dirección física donde comienza la instrucción actual
        uint32_t dir_fisica_inst = baseCod + offset_actual;
        uint8_t primer_byte = mv->mem[dir_fisica_inst];
        
        // Desarmamos el byte de control (Opcode y tipos)
        uint8_t opcode = primer_byte & 0x1F;
        uint8_t tipo_opA = (primer_byte >> 4) & 0x03;
        uint8_t tipo_opB = (primer_byte >> 6) & 0x03;
        
        uint32_t offset_lectura = 1; // Cuenta el primer byte
        int32_t valorB = 0;
        int32_t valorA = 0;
        
        // 1. Lectura Operando B (Orden inverso físico en la RAM)
        if (tipo_opB > 0) {
            for (int i = 0; i < tipo_opB; i++) {
                valorB = (valorB << 8) | mv->mem[dir_fisica_inst + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opB == 2) valorB = (int16_t)valorB;
        }
        
        // 2. Lectura Operando A
        if (tipo_opA > 0) {
            for (int i = 0; i < tipo_opA; i++) {
                valorA = (valorA << 8) | mv->mem[dir_fisica_inst + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opA == 2) valorA = (int16_t)valorA;
        }
        
        // 3. Imprimir Dirección Física (4 dígitos hexa)[cite: 8]
        printf("[%04X] ", dir_fisica_inst);
        
        // 4. Imprimir volcado hexadecimal de los bytes leídos
        for (uint32_t i = 0; i < offset_lectura; i++) {
            printf("%02X ", mv->mem[dir_fisica_inst + i]);
        }
        
        // 5. Alineación visual estática
        // Como la instrucción más larga ocupa 7 bytes, rellenamos los faltantes[cite: 6].
        for (uint32_t i = offset_lectura; i < 7; i++) {
            printf("   ");
        }
        printf(" | ");
        
        // 6. Imprimir el Mnemónico usando el arreglo opStr de mv.h[cite: 1]
        const char* mnem = (opcode <= 0x1F && opStr[opcode] != NULL) ? opStr[opcode] : "UNKNOWN";
        printf("%-5s", mnem);
        
        // 7. Imprimir los Operandos (En Assembler se escriben en orden: OP_A, OP_B)[cite: 8]
        if (tipo_opA > 0) {
            imprimirOperando(tipo_opA, valorA);
        }
        if (tipo_opB > 0) {
            if (tipo_opA > 0) printf(", ");
            imprimirOperando(tipo_opB, valorB);
        }
        
        printf("\n"); // Salto de línea final para la siguiente instrucción
        
        // Avanzamos al siguiente bloque
        offset_actual += offset_lectura;
    }
    
    printf("--------------------------------------------------\n");
}