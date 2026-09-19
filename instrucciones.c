#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include "mv.h"

/* ========================================================================= */
/* SUBSISTEMA DE MEMORIA Y HARDWARE (LAR, MAR, MBR)                          */
/* ========================================================================= */

/**
 * @brief Lee 4 bytes de memoria simulando el bus de datos del hardware.
 * 
 * Cumple con el requisito de la cátedra:
 * - Carga en LAR la dirección lógica solicitada.
 * - Carga en MAR la cantidad de bytes (parte alta) y la dirección física (parte baja).
 * - Carga en MBR el dato de 32 bits leído en formato Big Endian.
 * 
 * @param mv         Puntero a la estructura de la Máquina Virtual.
 * @param dir_logica Dirección lógica compuesta por [Segmento:Offset].
 * @return int32_t   Valor de 32 bits leído con signo.
 */
static int32_t leerMemoriaHardware(TMV* mv, uint32_t dir_logica) {
    // 1. El procesador coloca la dirección lógica en LAR
    mv->reg[LAR] = (int32_t)dir_logica;

    // 2. La MMU traduce la dirección lógica a física y valida los límites del segmento
    int32_t dir_fisica = traducirDireccion(mv, dir_logica, 4);
    if (dir_fisica < 0) {
        printf("Error: Fallo de segmento en lectura (Dir Logica: 0x%08X).\n", dir_logica);
        mv->errorFlag = 1;
        return 0;
    }

    // 3. Se configura MAR:
    //    - 16 bits superiores: cantidad de bytes transferidos (4)
    //    - 16 bits inferiores: dirección física en la RAM
    mv->reg[MAR] = (int32_t)((4U << 16) | ((uint32_t)dir_fisica & 0xFFFFU));

    // 4. Lectura de los 4 bytes consecutivos en memoria física bajo orden Big Endian
    //    (el byte más significativo reside en la dirección física menor)
    uint32_t b0 = mv->mem[dir_fisica];
    uint32_t b1 = mv->mem[dir_fisica + 1];
    uint32_t b2 = mv->mem[dir_fisica + 2];
    uint32_t b3 = mv->mem[dir_fisica + 3];

    int32_t dato = (int32_t)((b0 << 24) | (b1 << 16) | (b2 << 8) | b3);

    // 5. El dato resultante queda disponible en el registro MBR
    mv->reg[MBR] = dato;

    return dato;
}

/**
 * @brief Escribe 4 bytes en memoria simulando el bus de datos del hardware.
 * 
 * Actualiza LAR, MAR y MBR, y descompone el valor de 32 bits en 4 celdas
 * de 1 byte en orden Big Endian.
 * 
 * @param mv         Puntero a la Máquina Virtual.
 * @param dir_logica Dirección lógica de destino [Segmento:Offset].
 * @param valor      Entero de 32 bits a escribir.
 */
static void escribirMemoriaHardware(TMV* mv, uint32_t dir_logica, int32_t valor) {
    mv->reg[LAR] = (int32_t)dir_logica;

    int32_t dir_fisica = traducirDireccion(mv, dir_logica, 4);
    if (dir_fisica < 0) {
        printf("Error: Fallo de segmento en escritura (Dir Logica: 0x%08X).\n", dir_logica);
        mv->errorFlag = 1;
        return;
    }

    mv->reg[MAR] = (int32_t)((4U << 16) | ((uint32_t)dir_fisica & 0xFFFFU));
    mv->reg[MBR] = valor;

    // Descomposición del dato en 4 bytes (Big Endian)
    mv->mem[dir_fisica]     = (uint8_t)((valor >> 24) & 0xFF);
    mv->mem[dir_fisica + 1] = (uint8_t)((valor >> 16) & 0xFF);
    mv->mem[dir_fisica + 2] = (uint8_t)((valor >> 8)  & 0xFF);
    mv->mem[dir_fisica + 3] = (uint8_t)(valor         & 0xFF);
}

/**
 * @brief Interpreta y obtiene el valor final de un operando de la instrucción.
 * 
 * Tipos de operando admitidos por la arquitectura:
 * - Tipo 1 (Registro): El operando contiene el código del registro (0..31).
 * - Tipo 2 (Inmediato): El operando es un entero signado de 16 bits.
 * - Tipo 3 (Memoria): Operando de 3 bytes compuesto por registro base y desplazamiento.
 * 
 * @param mv      Puntero a la Máquina Virtual.
 * @param tipo    Código binario del tipo de operando (1, 2 o 3).
 * @param raw_val Valor bruto leído durante la etapa de Decode.
 * @return int32_t Valor resuelto y extendido a 32 bits.
 */
int32_t obtenerValorOperando(TMV* mv, uint8_t tipo, int32_t raw_val) {
    if (tipo == 1) {
        // Enmascaramos a 5 bits para garantizar un índice de registro válido (0..31)
        uint8_t num_reg = (uint8_t)(raw_val & 0x1F);
        return mv->reg[num_reg];
    } 
    else if (tipo == 2) {
        // Casteo a int16_t para forzar la extensión de signo a 32 bits
        return (int32_t)((int16_t)(raw_val & 0xFFFF));
    } 
    else if (tipo == 3) {
        // Formato de memoria: 8 bits de registro base + 16 bits de offset con signo
        uint8_t regBase = (uint8_t)(raw_val & 0x1F);
        int16_t offset  = (int16_t)((raw_val >> 8) & 0xFFFF);

        // La dirección lógica resulta de sumar el offset al puntero del registro base
        uint32_t dir_logica = (uint32_t)(mv->reg[regBase] + offset);

        return leerMemoriaHardware(mv, dir_logica);
    }
    return 0;
}

/**
 * @brief Guarda un resultado de 32 bits en el operando de destino (A).
 * 
 * @param mv      Puntero a la Máquina Virtual.
 * @param tipo    Tipo de operando destino (1: Registro, 3: Memoria).
 * @param raw_val Valor bruto decodificado del operando destino.
 * @param valor   Valor que se desea escribir.
 */
void guardarResultado(TMV* mv, uint8_t tipo, int32_t raw_val, int32_t valor) {
    if (tipo == 1) {
        uint8_t num_reg = (uint8_t)(raw_val & 0x1F);
        mv->reg[num_reg] = valor;
    } 
    else if (tipo == 3) {
        uint8_t regBase = (uint8_t)(raw_val & 0x1F);
        int16_t offset  = (int16_t)((raw_val >> 8) & 0xFFFF);
        uint32_t dir_logica = (uint32_t)(mv->reg[regBase] + offset);

        escribirMemoriaHardware(mv, dir_logica, valor);
    } 
    else {
        // Un inmediato no puede ser operando de destino
        printf("Error: Intento de escritura en un operando no modificable.\n");
        mv->errorFlag = 1;
    }
}

/**
 * @brief Realiza un salto dentro del segmento de código actual (CS).
 * 
 * Conserva el índice de segmento en los 16 bits superiores de IP y reemplaza
 * el desplazamiento en los 16 bits inferiores.
 * 
 * @param mv             Puntero a la Máquina Virtual.
 * @param destino_offset Desplazamiento de destino dentro de CS.
 */
static void realizarSalto(TMV* mv, int32_t destino_offset) {
    mv->reg[IP] = (mv->reg[CS] & 0xFFFF0000) | ((uint32_t)destino_offset & 0xFFFF);
}

/* ========================================================================= */
/* GESTIÓN DE BANDERAS DEL REGISTRO CC (NZCV)                                */
/* ========================================================================= */

/**
 * @brief Actualiza las banderas del registro de códigos de condición (CC).
 * 
 * Asignación de bits en la arquitectura 2026:
 * - Bit 31: N (Negativo / Signo)
 * - Bit 30: Z (Cero)
 * - Bit 29: C (Acarreo / Carry)
 * - Bit 28: V (Desbordamiento / Overflow)
 */
static void actualizarCC(TMV* mv, int n, int z, int c, int v) {
    uint32_t nuevo_cc = 0;

    if (n) nuevo_cc |= CC_N;
    if (z) nuevo_cc |= CC_Z;
    if (c) nuevo_cc |= CC_C;
    if (v) nuevo_cc |= CC_V;

    mv->reg[CC] = (int32_t)nuevo_cc;
}

/* ========================================================================= */
/* IMPLEMENTACIÓN DE LAS 26 INSTRUCCIONES                                    */
/* ========================================================================= */

// 0x00: SYS (Llamadas al Sistema: 1 = READ, 2 = WRITE)
void op_sys(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB; // SYS utiliza solo el primer operando

    int32_t nro_servicio = obtenerValorOperando(mv, tA, vA);
    uint32_t modo        = (uint32_t)mv->reg[EAX];
    uint32_t dir_logica  = (uint32_t)mv->reg[EDX];
    uint16_t cantidad    = (uint16_t)(mv->reg[ECX] & 0xFFFF);
    uint16_t tam_celda   = (uint16_t)((mv->reg[ECX] >> 16) & 0xFFFF);

    if (tam_celda == 0) tam_celda = 1;

    if (nro_servicio == 1) {
        // SYS READ: Lee valores desde el teclado y los guarda en celdas de memoria
        for (int i = 0; i < cantidad; i++) {
            uint32_t dir_celda_logica = dir_logica + (i * tam_celda);
            mv->reg[LAR] = (int32_t)dir_celda_logica;

            int32_t dir_fisica = traducirDireccion(mv, dir_celda_logica, tam_celda);
            if (dir_fisica < 0) {
                printf("Error: Fallo de segmento en SYS READ.\n");
                mv->errorFlag = 1;
                return;
            }

            mv->reg[MAR] = (int32_t)(((uint32_t)tam_celda << 16) | ((uint32_t)dir_fisica & 0xFFFFU));
            printf("[%04X]: ", dir_fisica);

            int32_t entrada = 0;
            if (modo & 0x02) {
                // Modo Carácter
                char c = 0;
                if (scanf(" %c", &c) == 1) entrada = (int32_t)c;
            } else if (modo & 0x08) {
                // Modo Hexadecimal
                if (scanf("%x", (unsigned int*)&entrada) != 1) entrada = 0;
            } else if (modo & 0x04) {
                // Modo Octal
                if (scanf("%o", (unsigned int*)&entrada) != 1) entrada = 0;
            } else {
                // Modo Decimal por defecto
                if (scanf("%d", &entrada) != 1) entrada = 0;
            }

            mv->reg[MBR] = entrada;

            // Almacenamos el dato en la celda en formato Big Endian
            for (int b = 0; b < tam_celda; b++) {
                int shift = (tam_celda - 1 - b) * 8;
                mv->mem[dir_fisica + b] = (uint8_t)((entrada >> shift) & 0xFF);
            }
        }
    } 
    else if (nro_servicio == 2) {
        // SYS WRITE: Muestra en pantalla el contenido de las celdas de memoria
        for (int i = 0; i < cantidad; i++) {
            uint32_t dir_celda_logica = dir_logica + (i * tam_celda);
            mv->reg[LAR] = (int32_t)dir_celda_logica;

            int32_t dir_fisica = traducirDireccion(mv, dir_celda_logica, tam_celda);
            if (dir_fisica < 0) {
                printf("Error: Fallo de segmento en SYS WRITE.\n");
                mv->errorFlag = 1;
                return;
            }

            mv->reg[MAR] = (int32_t)(((uint32_t)tam_celda << 16) | ((uint32_t)dir_fisica & 0xFFFFU));

            // Reconstrucción del dato en formato Big Endian
            uint32_t dato = 0;
            for (int b = 0; b < tam_celda; b++) {
                dato = (dato << 8) | mv->mem[dir_fisica + b];
            }

            int32_t val_mostrar = (int32_t)dato;
            // Extensión de signo si la celda es de 1 o 2 bytes
            if (tam_celda == 1 && (val_mostrar & 0x80)) {
                val_mostrar |= 0xFFFFFF00;
            } else if (tam_celda == 2 && (val_mostrar & 0x8000)) {
                val_mostrar |= 0xFFFF0000;
            }

            mv->reg[MBR] = val_mostrar;
            printf("[%04X]:", dir_fisica);

            // Salida en Binario (Bit 4 = 0x10)
            if (modo & 0x10) {
                printf(" 0b");
                int bits = tam_celda * 8;
                for (int b = bits - 1; b >= 0; b--) {
                    putchar((val_mostrar & (1 << b)) ? '1' : '0');
                }
            }
            // Salida en Hexadecimal (Bit 3 = 0x08)
            if (modo & 0x08) {
                if (tam_celda == 1)      printf(" 0x%02X", val_mostrar & 0xFF);
                else if (tam_celda == 2) printf(" 0x%04X", val_mostrar & 0xFFFF);
                else                     printf(" 0x%08X", val_mostrar);
            }
            // Salida en Octal (Bit 2 = 0x04)
            if (modo & 0x04) {
                printf(" 0o%o", val_mostrar);
            }
            // Salida en Caracteres (Bit 1 = 0x02)
            if (modo & 0x02) {
                printf(" ");
                for (int b = tam_celda - 1; b >= 0; b--) {
                    char c = (char)((val_mostrar >> (b * 8)) & 0xFF);
                    putchar((c >= 32 && c <= 126) ? c : '.');
                }
            }
            // Salida en Decimal (Bit 0 = 0x01)
            if (modo & 0x01) {
                printf(" %d", val_mostrar);
            }
            printf("\n");
        }
    } 
    else {
        printf("Error: Servicio SYS desconocido (%d).\n", nro_servicio);
        mv->errorFlag = 1;
    }
}

// 0x01: JMP (Salto Incondicional)
void op_jmp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
}

// 0x02: JP (Salto si es estrictamente positivo: N == 0 y Z == 0)
void op_jp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (!(mv->reg[CC] & CC_N) && !(mv->reg[CC] & CC_Z)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x03: JN (Salto si es negativo: N == 1)
void op_jn(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_N) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x04: JZ (Salto si es cero: Z == 1)
void op_jz(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_Z) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x05: JC (Salto si hay acarreo: C == 1)
void op_jc(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_C) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x06: JV (Salto si hay desbordamiento: V == 1)
void op_jv(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_V) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x07: JNP (Salto si no es positivo: N == 1 o Z == 1)
void op_jnp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if ((mv->reg[CC] & CC_N) || (mv->reg[CC] & CC_Z)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x08: JNN (Salto si no es negativo: N == 0)
void op_jnn(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (!(mv->reg[CC] & CC_N)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x09: JNZ (Salto si no es cero: Z == 0)
void op_jnz(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (!(mv->reg[CC] & CC_Z)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x0A: NOT (Negación bit a bit: A = ~A)
void op_not(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t res  = ~valA;
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x0F: STOP (Detiene el procesador fijando IP = 0xFFFFFFFF)
void op_stop(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tA; (void)vA; (void)tB; (void)vB;
    mv->reg[IP] = (int32_t)0xFFFFFFFF;
}

// 0x10: MOV (Asignación: A = B)
void op_mov(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, valB);
    actualizarCC(mv, valB < 0, valB == 0, 0, 0);
}

// 0x11: ADD (Suma: A = A + B)
void op_add(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = a + b;
    guardarResultado(mv, tA, vA, res);

    // Carry: suma sin signo excede 32 bits
    uint64_t u_sum = (uint64_t)(uint32_t)a + (uint64_t)(uint32_t)b;
    int c = (u_sum >> 32) & 1;

    // Overflow: suma de operandos del mismo signo produce resultado de signo opuesto
    int v = ((a > 0 && b > 0 && res < 0) || (a < 0 && b < 0 && res >= 0));

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x12: SUB (Resta: A = A - B)
void op_sub(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = a - b;
    guardarResultado(mv, tA, vA, res);

    // Carry en resta: carry-out de la suma a + (~b + 1)
    uint64_t comp_b = (uint32_t)(~b) + 1ULL;
    uint64_t sum_sub = (uint64_t)(uint32_t)a + comp_b;
    int c = (sum_sub >> 32) & 1;

    // Overflow con signo en resta
    int v = ((a > 0 && b < 0 && res < 0) || (a < 0 && b > 0 && res >= 0));

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x13: MUL (Multiplicación: A = A * B)
void op_mul(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int64_t prod64 = (int64_t)a * (int64_t)b;
    int32_t res = (int32_t)prod64;
    guardarResultado(mv, tA, vA, res);

    // Desborde si el producto de 64 bits no cabe en un entero signado de 32 bits
    int desborde = (prod64 > INT32_MAX || prod64 < INT32_MIN);
    actualizarCC(mv, res < 0, res == 0, desborde, desborde);
}

// 0x14: DIV (División entera: A = A / B, Resto en AC)
void op_div(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);

    if (b == 0) {
        printf("Error: No se puede dividir por 0.\n");
        mv->errorFlag = 1;
        return;
    }

    int32_t cociente = a / b;
    int32_t resto    = a % b;

    guardarResultado(mv, tA, vA, cociente);
    mv->reg[AC] = resto;

    actualizarCC(mv, cociente < 0, cociente == 0, 0, 0);
}

// 0x15: CMP (Compara A con B calculando A - B sin guardar el resultado)
void op_cmp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = a - b;

    uint64_t comp_b = (uint32_t)(~b) + 1ULL;
    uint64_t sum_sub = (uint64_t)(uint32_t)a + comp_b;
    int c = (sum_sub >> 32) & 1;
    int v = ((a > 0 && b < 0 && res < 0) || (a < 0 && b > 0 && res >= 0));

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x16: AND (Lógica AND bit a bit)
void op_and(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t res = obtenerValorOperando(mv, tA, vA) & obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x17: OR (Lógica OR bit a bit)
void op_or(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t res = obtenerValorOperando(mv, tA, vA) | obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x18: XOR (Lógica XOR bit a bit)
void op_xor(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t res = obtenerValorOperando(mv, tA, vA) ^ obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x19: SWAP (Intercambio de valores entre operandos)
void op_swap(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, valB);
    guardarResultado(mv, tB, vB, valA);

    // Afecta CC igual que un XOR entre ambos valores
    int32_t res = valA ^ valB;
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x1A: SHL (Desplazamiento lógico a la izquierda)
void op_shl(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t n = obtenerValorOperando(mv, tB, vB) & 0x1F; // Acotado a 0..31 bits

    int32_t res = (n == 0) ? a : (a << n);
    guardarResultado(mv, tA, vA, res);

    // Carry: último bit desplazado fuera del límite de 32 bits
    int c = (n > 0) ? (int)((((uint32_t)a) >> (32 - n)) & 1U) : 0;
    // Overflow: el signo cambió durante el corrimiento
    int v = (n > 0) ? (((a ^ res) >> 31) & 1) : 0;

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x1B: SHR (Desplazamiento lógico a la derecha)
void op_shr(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    uint32_t a = (uint32_t)obtenerValorOperando(mv, tA, vA);
    int32_t n  = obtenerValorOperando(mv, tB, vB) & 0x1F;

    uint32_t res = (n == 0) ? a : (a >> n);
    guardarResultado(mv, tA, vA, (int32_t)res);

    // Carry: último bit expulsado por la derecha
    int c = (n > 0) ? (int)((a >> (n - 1)) & 1U) : 0;
    actualizarCC(mv, (int32_t)res < 0, res == 0, c, 0);
}

// 0x1C: SAR (Desplazamiento aritmético a la derecha conservando signo)
void op_sar(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t n = obtenerValorOperando(mv, tB, vB) & 0x1F;

    int32_t res = (n == 0) ? a : (a >> n);
    guardarResultado(mv, tA, vA, res);

    int c = (n > 0) ? (int)((((uint32_t)a) >> (n - 1)) & 1U) : 0;
    actualizarCC(mv, res < 0, res == 0, c, 0);
}

// 0x1D: LDL (Carga en los 2 bytes menos significativos)
void op_ldl(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (a & (int32_t)0xFFFF0000) | (b & 0xFFFF);
    guardarResultado(mv, tA, vA, res);
}

// 0x1E: LDH (Carga en los 2 bytes más significativos)
void op_ldh(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (a & 0x0000FFFF) | ((b & 0xFFFF) << 16);
    guardarResultado(mv, tA, vA, res);
}

// 0x1F: RND (Genera un número pseudoaleatorio entre 0 y B)
void op_rnd(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (b > 0) ? (rand() % (b + 1)) : 0;
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

/* ========================================================================= */
/* TABLA DE DESPACHO DE INSTRUCCIONES                                        */
/* ========================================================================= */
InstruccionFunc tablaInstrucciones[32];

void inicializarTablaInstrucciones(void) {
    for (int i = 0; i < 32; i++) {
        tablaInstrucciones[i] = NULL;
    }

    tablaInstrucciones[SYS]  = op_sys;
    tablaInstrucciones[JMP]  = op_jmp;
    tablaInstrucciones[JP]   = op_jp;
    tablaInstrucciones[JN]   = op_jn;
    tablaInstrucciones[JZ]   = op_jz;
    tablaInstrucciones[JC]   = op_jc;
    tablaInstrucciones[JV]   = op_jv;
    tablaInstrucciones[JNP]  = op_jnp;
    tablaInstrucciones[JNN]  = op_jnn;
    tablaInstrucciones[JNZ]  = op_jnz;
    tablaInstrucciones[NOT]  = op_not;
    tablaInstrucciones[STOP] = op_stop;
    tablaInstrucciones[MOV]  = op_mov;
    tablaInstrucciones[ADD]  = op_add;
    tablaInstrucciones[SUB]  = op_sub;
    tablaInstrucciones[MUL]  = op_mul;
    tablaInstrucciones[DIV]  = op_div;
    tablaInstrucciones[CMP]  = op_cmp;
    tablaInstrucciones[AND]  = op_and;
    tablaInstrucciones[OR]   = op_or;
    tablaInstrucciones[XOR]  = op_xor;
    tablaInstrucciones[SWAP] = op_swap;
    tablaInstrucciones[SHL]  = op_shl;
    tablaInstrucciones[SHR]  = op_shr;
    tablaInstrucciones[SAR]  = op_sar;
    tablaInstrucciones[LDL]  = op_ldl;
    tablaInstrucciones[LDH]  = op_ldh;
    tablaInstrucciones[RND]  = op_rnd;
}