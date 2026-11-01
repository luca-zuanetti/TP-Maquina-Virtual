#include <stdio.h>
#include <stdint.h>
#include "mv.h"

/* Texto de un operando: registro (EAX/AL/AH/AX), inmediato o memoria (b/w/l[reg+off]). */
static void formatearOperando(char* buf, size_t tam, uint8_t tipo, int32_t valor) {
    buf[0] = '\0';
    if (tipo == 1) {
        // Registro: código (5 bits) + sector (2 bits)
        uint8_t regCode = valor & 0x1F;
        uint8_t sector = (valor >> 5) & 0x03;

        // EAX..EFX admiten sectores: EAX, AL, AH, AX
        if (regCode >= 10 && regCode <= 15) {
            char letra = regStr[regCode][1]; // 'A' de "EAX", 'B' de "EBX"...

            if (sector == 0) snprintf(buf, tam, "E%cX", letra);      // EAX
            else if (sector == 1) snprintf(buf, tam, "%cL", letra);  // AL
            else if (sector == 2) snprintf(buf, tam, "%cH", letra);  // AH
            else snprintf(buf, tam, "%cX", letra);                   // AX
        } else {
            snprintf(buf, tam, "%s", regStr[regCode]); // resto de los registros
        }
    }
    else if (tipo == 2) {
        // Inmediato: 16 bits con signo
        snprintf(buf, tam, "%d", (int16_t)valor);
    }
    else if (tipo == 3) {
        // Memoria: [registro + desplazamiento]
        uint8_t regCode = valor & 0x1F;
        int16_t offset = (int16_t)((valor >> 8) & 0xFFFF);
        uint8_t mod = (valor >> 5) & 0x07;

        // Prefijo de tamaño
        const char* prefijo = (mod == 1) ? "b" : (mod == 2) ? "w" : (mod == 4) ? "l" : "";

        // El desplazamiento siempre con signo, incluso +0
        snprintf(buf, tam, "%s[%s%s%d]", prefijo, regStr[regCode], (offset < 0) ? "" : "+", offset);
    }
}

/* Instrucción más larga: 1 + 3 + 3 = 7 bytes (ancho de la columna hexa) */
#define MAX_BYTES_LINEA 7

/* Lista las cadenas del Const Segment:  [XXXX] XX XX ... | "cadena"
   (más de 7 bytes: muestra 6 y ".."; no imprimibles como '.'). */
static void imprimirConstantes(TMV* mv) {
    if (mv->reg[KS] == -1) return; // sin Const Segment

    uint16_t segCons = (uint16_t)(((uint32_t)mv->reg[KS] >> 16) & 0xFFFF);
    if (segCons >= SEG_AMOUNT) return;
    // Entrada inactiva (imagen dañada): nada que mostrar
    if (mv->seg[segCons].base == ENTRY_INACTIVE && mv->seg[segCons].size == ENTRY_INACTIVE) return;

    uint32_t baseCons = mv->seg[segCons].base;
    uint32_t tamCons  = mv->seg[segCons].size;
    uint32_t offset_actual = 0;

    while (offset_actual < tamCons) {
        uint32_t dir_fisica_cad = baseCons + offset_actual;

        // 1. Largo de la cadena con su '\0'
        uint32_t largo = 0;
        while (offset_actual + largo < tamCons && mv->mem[dir_fisica_cad + largo] != 0) {
            largo++;
        }
        if (offset_actual + largo < tamCons) largo++; // cuenta el '\0'

        // 2. Dirección física
        printf(" [%04X] ", dir_fisica_cad);

        // 3. Bytes en hexa (hasta 7, si no 6 + "..")
        if (largo <= MAX_BYTES_LINEA) {
            for (uint32_t i = 0; i < largo; i++) {
                printf("%02X ", mv->mem[dir_fisica_cad + i]);
            }
            for (uint32_t i = largo; i < MAX_BYTES_LINEA; i++) {
                printf("   ");
            }
        } else {
            for (uint32_t i = 0; i < MAX_BYTES_LINEA - 1; i++) {
                printf("%02X ", mv->mem[dir_fisica_cad + i]);
            }
            printf(".. ");
        }
        printf("| \"");

        // 4. Cadena entre comillas
        for (uint32_t i = 0; i < largo && mv->mem[dir_fisica_cad + i] != 0; i++) {
            uint8_t c = mv->mem[dir_fisica_cad + i];
            putchar((c >= 32 && c <= 126) ? c : '.');
        }
        printf("\"\n");

        offset_actual += largo;
    }
}

/* DISASSEMBLER (-d): muestra las constantes y luego cada instrucción del Code Segment.
   Formato: >[XXXX] XX XX XX | MNEM  OP_A, OP_B   ('>' = entry point) */
void ejecutarDisassembler(TMV* mv) {
    imprimirConstantes(mv);

    uint16_t segCod  = (uint16_t)(((uint32_t)mv->reg[CS] >> 16) & 0xFFFF);
    uint32_t baseCod = mv->seg[segCod].base;
    uint32_t tamCod  = mv->seg[segCod].size;
    uint32_t offset_actual = 0;

    // Antes de ejecutar, IP = entry point
    uint16_t segIP       = (uint16_t)(((uint32_t)mv->reg[IP] >> 16) & 0xFFFF);
    uint16_t entryOffset = (uint16_t)((uint32_t)mv->reg[IP] & 0xFFFF);
    int hayEntryPoint    = (segIP == segCod);

    while (offset_actual < tamCod) {
        uint32_t dir_fisica_inst = baseCod + offset_actual;
        uint8_t primer_byte = mv->mem[dir_fisica_inst];

        // Decode del byte de control (igual que en ejecutarMV)
        uint8_t opcode   = primer_byte & 0x1F;
        uint8_t tipo_opA = 0;
        uint8_t tipo_opB = 0;

        if (opcode >= 0x10 && opcode <= 0x1F) {
            tipo_opA = (primer_byte >> 4) & 0x03;
            tipo_opB = (primer_byte >> 6) & 0x03;
        } else if (opcode <= 0x0D) {
            tipo_opA = (primer_byte >> 6) & 0x03;
            tipo_opB = 0;
        } else if (opcode == 0x0F) {
            tipo_opA = 0;
            tipo_opB = 0;
        }

        // Instrucción cortada al final de CS: solo se vuelcan sus bytes
        uint32_t largo_inst = 1U + tipo_opA + tipo_opB;
        int incompleta = (offset_actual + largo_inst > tamCod);
        if (incompleta) {
            tipo_opA = 0;
            tipo_opB = 0;
        }

        uint32_t offset_lectura = 1;
        int32_t valorB = 0;
        int32_t valorA = 0;

        // 1. Operando B (viene primero)
        if (tipo_opB > 0) {
            for (int i = 0; i < tipo_opB; i++) {
                valorB = (valorB << 8) | mv->mem[dir_fisica_inst + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opB == 2) valorB = (int16_t)valorB;
        }

        // 2. Operando A
        if (tipo_opA > 0) {
            for (int i = 0; i < tipo_opA; i++) {
                valorA = (valorA << 8) | mv->mem[dir_fisica_inst + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opA == 2) valorA = (int16_t)valorA;
        }

        if (incompleta) offset_lectura = tamCod - offset_actual;

        // 3. Marca '>' y dirección física
        int esEntryPoint = hayEntryPoint && offset_actual == entryOffset;
        printf("%c[%04X] ", esEntryPoint ? '>' : ' ', dir_fisica_inst);

        // 4. Bytes de la instrucción en hexa
        for (uint32_t i = 0; i < offset_lectura; i++) {
            printf("%02X ", mv->mem[dir_fisica_inst + i]);
        }

        // 5. Relleno hasta 7 bytes para alinear
        for (uint32_t i = offset_lectura; i < MAX_BYTES_LINEA; i++) {
            printf("   ");
        }
        printf("| ");

        // 6. Mnemónico ("???" si está cortada)
        const char* mnem = (!incompleta && opcode < 32 && opStr[opcode] != NULL) ? opStr[opcode] : "???";
        printf("%4s", mnem);

        // 7. Operandos A, B alineados a derecha
        char textoA[24], textoB[24];
        formatearOperando(textoA, sizeof(textoA), tipo_opA, valorA);
        formatearOperando(textoB, sizeof(textoB), tipo_opB, valorB);
        if (tipo_opA > 0 && tipo_opB > 0) {
            printf(" %10s, %10s", textoA, textoB);
        } else if (tipo_opA > 0) {
            printf(" %10s", textoA);
        }

        printf("\n");
        offset_actual += offset_lectura;
    }
}
