#include <stdio.h>
#include <stdint.h>
#include "mv.h"

/**
 * @brief Imprime la representación en texto de un operando según su tipo.
 * 
 * @param tipo  Tipo de operando (1: Registro, 2: Inmediato, 3: Memoria).
 * @param valor Valor bruto leído de los bytes de la instrucción.
 */
static void imprimirOperando(uint8_t tipo, int32_t valor) {
    if (tipo == 1) { 
        // Operando Registro (1 byte): índice de 0 a 31
        uint8_t regCode = valor & 0x1F;
        uint8_t sector = (valor >> 5) & 0x03;
        
        // Si es un registro de propósito general (EAX a EFX), evaluamos el pseudónimo
        if (regCode < 32) {
            if (regCode >= 10 && regCode <= 15) { 
                char letra = regStr[regCode][1]; // Extrae la letra (A, B, C, D, E, F) de "EAX", "EBX", etc.
                
                if (sector == 0) printf("E%cX", letra);      // Ejemplo: EAX
                else if (sector == 1) printf("%cL", letra);  // Ejemplo: AL
                else if (sector == 2) printf("%cH", letra);  // Ejemplo: AH
                else printf("%cX", letra);                   // Ejemplo: AX
            } else {
                printf("%s", regStr[regCode]); // Imprime punteros y registros especiales normal
            }
        }
    } 
    else if (tipo == 2) { 
        // Operando Inmediato (2 bytes): constante entera con signo
        printf("%d", (int16_t)valor);
    } 
    else if (tipo == 3) { 
        // Operando Memoria (3 bytes): [registro + desplazamiento]
        uint8_t regCode = valor & 0x1F;
        int16_t offset = (int16_t)((valor >> 8) & 0xFFFF);
        uint8_t mod = (valor >> 5) & 0x07;
        
        // Aplica el modificador de tamaño a la cadena de salida
        if (mod == 1) printf("b");
        else if (mod == 2) printf("w");
        else if (mod == 4) printf("l");
        
        if (regCode < 32) {
            // Formato oficial: el desplazamiento siempre lleva signo, incluso el cero ([DS+0])
            if (offset < 0) printf("[%s%d]", regStr[regCode], offset);
            else printf("[%s+%d]", regStr[regCode], offset);
        } else {
            printf("[%d]", offset);
        }
    }
}

/* Ancho de la columna hexadecimal: la instrucción más larga ocupa 7 bytes
   (byte de control + 3 bytes de OP_B + 3 bytes de OP_A) */
#define MAX_BYTES_LINEA 7

/**
 * @brief Recorre el Const Segment e imprime cada cadena terminada en '\0'.
 * Formato:  [XXXX] XX XX XX XX         | "cadena"
 *
 * El volcado hexadecimal incluye el '\0'. Si la cadena ocupa más de 7 bytes
 * se muestran los primeros 6 seguidos de "..". Entre comillas va la cadena
 * sin el '\0', con un punto en lugar de cada carácter no imprimible.
 */
static void imprimirConstantes(TMV* mv) {
    if (mv->reg[KS] == -1) return; // El proceso no tiene Const Segment

    uint16_t segCons = (uint16_t)(((uint32_t)mv->reg[KS] >> 16) & 0xFFFF);
    if (segCons >= SEG_AMOUNT) return;

    uint32_t baseCons = mv->seg[segCons].base;
    uint32_t tamCons  = mv->seg[segCons].size;
    uint32_t offset_actual = 0;

    while (offset_actual < tamCons) {
        uint32_t dir_fisica_cad = baseCons + offset_actual;

        // 1. Largo de la cadena incluyendo el '\0' (sin pasarse del fin del segmento)
        uint32_t largo = 0;
        while (offset_actual + largo < tamCons && mv->mem[dir_fisica_cad + largo] != 0) {
            largo++;
        }
        if (offset_actual + largo < tamCons) largo++; // Cuenta el '\0'

        // 2. Dirección física (el espacio inicial alinea con la marca '>' del código)
        printf(" [%04X] ", dir_fisica_cad);

        // 3. Volcado hexadecimal: completo hasta 7 bytes, si no 6 bytes y ".."
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

        // 4. Cadena entre comillas, reemplazando los caracteres no imprimibles por '.'
        for (uint32_t i = 0; i < largo && mv->mem[dir_fisica_cad + i] != 0; i++) {
            uint8_t c = mv->mem[dir_fisica_cad + i];
            putchar((c >= 32 && c <= 126) ? c : '.');
        }
        printf("\"\n");

        offset_actual += largo;
    }
}

/**
 * @brief Muestra las constantes del Const Segment y luego recorre el segmento
 *        de código imprimiendo cada instrucción desensamblada.
 * Formato: >[XXXX] XX XX XX XX | MNEM OP_A, OP_B   ('>' marca el entry point)
 */
void ejecutarDisassembler(TMV* mv) {
    imprimirConstantes(mv);

    uint16_t segCod  = (uint16_t)(((uint32_t)mv->reg[CS] >> 16) & 0xFFFF);
    uint32_t baseCod = mv->seg[segCod].base;
    uint32_t tamCod  = mv->seg[segCod].size;
    uint32_t offset_actual = 0;

    // Antes de ejecutar, IP apunta al entry point: su offset está en los 16 bits bajos.
    // Solo se marca si IP apunta al Code Segment (no, por ejemplo, si vale -1)
    uint16_t segIP       = (uint16_t)(((uint32_t)mv->reg[IP] >> 16) & 0xFFFF);
    uint16_t entryOffset = (uint16_t)((uint32_t)mv->reg[IP] & 0xFFFF);
    int hayEntryPoint    = (segIP == segCod);

    while (offset_actual < tamCod) {
        uint32_t dir_fisica_inst = baseCod + offset_actual;
        uint8_t primer_byte = mv->mem[dir_fisica_inst];
        
        // Decodificación del byte de control
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

        // Una instrucción cortada al final del Code Segment no se decodifica:
        // se vuelcan solo sus bytes presentes para no leer fuera del segmento
        uint32_t largo_inst = 1U + tipo_opA + tipo_opB;
        int incompleta = (offset_actual + largo_inst > tamCod);
        if (incompleta) {
            tipo_opA = 0;
            tipo_opB = 0;
        }

        uint32_t offset_lectura = 1;
        int32_t valorB = 0;
        int32_t valorA = 0;
        
        // 1. Lectura Operando B (en memoria física antecede a OP_A)
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

        if (incompleta) offset_lectura = tamCod - offset_actual;

        // 3. Marca del entry point y dirección física en hexadecimal a 4 dígitos
        int esEntryPoint = hayEntryPoint && offset_actual == entryOffset;
        printf("%c[%04X] ", esEntryPoint ? '>' : ' ', dir_fisica_inst);

        // 4. Volcado en hexadecimal de los bytes leídos de la instrucción
        for (uint32_t i = 0; i < offset_lectura; i++) {
            printf("%02X ", mv->mem[dir_fisica_inst + i]);
        }

        // 5. Relleno para alinear columnas (la instrucción más larga ocupa 7 bytes)
        for (uint32_t i = offset_lectura; i < MAX_BYTES_LINEA; i++) {
            printf("   ");
        }
        printf("| ");

        // 6. Mnemónico de la instrucción ("???" si está cortada)
        const char* mnem = (!incompleta && opcode < 32 && opStr[opcode] != NULL) ? opStr[opcode] : "???";
        printf("%-5s ", mnem);
        
        // 7. Operandos en orden convencional (OP_A, OP_B)
        if (tipo_opA > 0) {
            imprimirOperando(tipo_opA, valorA);
        }
        if (tipo_opB > 0) {
            if (tipo_opA > 0) printf(", ");
            imprimirOperando(tipo_opB, valorB);
        }
        
        printf("\n");
        offset_actual += offset_lectura;
    }
}