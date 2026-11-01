#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include "mv.h"

/* ========================================================================= */
/* ACCESO A MEMORIA (LAR, MAR, MBR)                                          */
/* ========================================================================= */

/* Lectura de 1, 2 o 4 bytes: LAR = lógica, MAR = [bytes | física], MBR = dato
   (Big Endian, con extensión de signo). Fallo de segmento -> errorFlag. */
static int32_t leerMemoriaHardware(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes) {
    // 1. LAR: dirección lógica
    mv->reg[LAR] = (int32_t)dir_logica;

    // 2. MMU: lógica -> física
    int32_t dir_fisica = traducirDireccion(mv, dir_logica, cant_bytes);

    if (dir_fisica < 0) {
        printf("Error: Fallo de segmento en lectura (Dir: 0x%08X).\n", dir_logica);
        mv->errorFlag = 1;
        return 0;
    }

    // 3. MAR: [cantidad de bytes | dirección física]
    mv->reg[MAR] = (int32_t)(((uint32_t)cant_bytes << 16) | ((uint32_t)dir_fisica & 0xFFFFU));

    // 4. Lectura Big Endian con extensión de signo
    int32_t dato = 0;
    if (cant_bytes == 1) {
        dato = (int32_t)(int8_t)mv->mem[dir_fisica];
    } else if (cant_bytes == 2) {
        dato = (int32_t)(int16_t)((mv->mem[dir_fisica] << 8) | mv->mem[dir_fisica + 1]);
    } else { // 4 bytes
        dato = (int32_t)(((uint32_t)mv->mem[dir_fisica] << 24) | ((uint32_t)mv->mem[dir_fisica + 1] << 16) |
                         ((uint32_t)mv->mem[dir_fisica + 2] << 8) | (uint32_t)mv->mem[dir_fisica + 3]);
    }

    // 5. MBR: dato leído
    mv->reg[MBR] = dato;
    return dato;
}

/* Escritura de 1, 2 o 4 bytes en Big Endian, actualizando LAR, MAR y MBR. */
static void escribirMemoriaHardware(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes, int32_t valor) {
    mv->reg[LAR] = (int32_t)dir_logica;
    int32_t dir_fisica = traducirDireccion(mv, dir_logica, cant_bytes);

    if (dir_fisica < 0) {
        printf("Error: Fallo de segmento en escritura (Dir: 0x%08X).\n", dir_logica);
        mv->errorFlag = 1;
        return;
    }

    mv->reg[MAR] = (int32_t)(((uint32_t)cant_bytes << 16) | ((uint32_t)dir_fisica & 0xFFFFU));
    mv->reg[MBR] = valor;

    // Byte más significativo en la dirección más baja
    if (cant_bytes == 1) {
        mv->mem[dir_fisica] = (uint8_t)(valor & 0xFF);
    }
    else if (cant_bytes == 2) {
        mv->mem[dir_fisica]     = (uint8_t)((valor >> 8) & 0xFF);
        mv->mem[dir_fisica + 1] = (uint8_t)(valor & 0xFF);
    }
    else { // 4 bytes
        mv->mem[dir_fisica]     = (uint8_t)((valor >> 24) & 0xFF);
        mv->mem[dir_fisica + 1] = (uint8_t)((valor >> 16) & 0xFF);
        mv->mem[dir_fisica + 2] = (uint8_t)((valor >> 8)  & 0xFF);
        mv->mem[dir_fisica + 3] = (uint8_t)(valor         & 0xFF);
    }
}

/* Operando [reg + offset]: suma el offset solo a los 16 bits bajos del registro,
   sin tocar el segmento. Offset fuera de 0..0xFFFF -> fallo de segmento. */
static int calcularDireccionMemoria(TMV* mv, int32_t raw_val, uint32_t* dir_logica) {
    uint8_t regBase = (uint8_t)(raw_val & 0x1F);
    int16_t offset  = (int16_t)((raw_val >> 8) & 0xFFFF);

    uint32_t valorBase = (uint32_t)mv->reg[regBase];
    uint16_t segIdx    = (uint16_t)((valorBase >> 16) & 0xFFFF);
    uint16_t regOffset = (uint16_t)(valorBase & 0xFFFF);

    int32_t offsetFinal = (int32_t)regOffset + (int32_t)offset;
    if (offsetFinal < 0 || offsetFinal > 0xFFFF) {
        printf("Error: Fallo de segmento (desplazamiento fuera de rango: 0x%04X %+d).\n", regOffset, offset);
        mv->errorFlag = 1;
        return 0;
    }

    *dir_logica = ((uint32_t)segIdx << 16) | (uint32_t)offsetFinal;
    return 1;
}

/* Valor de un operando según su tipo: 1 registro (EAX/AL/AH/AX), 2 inmediato,
   3 memoria (b/w/l). Siempre devuelve 32 bits con signo extendido. */
static int32_t obtenerValorOperando(TMV* mv, uint8_t tipo, int32_t raw_val) {
    // Instrucción ya abortada: no se accede a memoria
    if (mv->errorFlag) return 0;

    if (tipo == 1) { // REGISTRO: 5 bits de código + 2 bits de sector
        uint8_t num_reg = (uint8_t)(raw_val & 0x1F);
        uint8_t sector = (raw_val >> 5) & 0x03;

        if (sector == 0) return mv->reg[num_reg];                                             // EAX (4 bytes)
        else if (sector == 1) return (int32_t)(int8_t)(mv->reg[num_reg] & 0xFF);              // AL
        else if (sector == 2) return (int32_t)(int8_t)((mv->reg[num_reg] >> 8) & 0xFF);       // AH
        else return (int32_t)(int16_t)(mv->reg[num_reg] & 0xFFFF);                            // AX
    }
    else if (tipo == 2) { // INMEDIATO: 16 bits con signo
        return (int32_t)((int16_t)(raw_val & 0xFFFF));
    }
    else if (tipo == 3) { // MEMORIA: offset 16 bits | modificador 3 bits | registro 5 bits
        uint8_t mod = (raw_val >> 5) & 0x07;

        uint32_t dir_logica;
        if (!calcularDireccionMemoria(mv, raw_val, &dir_logica)) return 0;

        // Modificador: b -> 1, w -> 2, l -> 4 bytes
        uint16_t cant_bytes = (mod == 1) ? 1 : (mod == 2) ? 2 : 4;
        return leerMemoriaHardware(mv, dir_logica, cant_bytes);
    }
    return 0;
}

/* Tamaño en bytes de un operando (registro por sector, memoria por modificador). */
static int cantBytesOperando(uint8_t tipo, int32_t raw_val) {
    if (tipo == 1) {
        uint8_t sector = (raw_val >> 5) & 0x03;
        return (sector == 0) ? 4 : (sector == 3) ? 2 : 1;
    }
    if (tipo == 3) {
        uint8_t mod = (raw_val >> 5) & 0x07;
        return (mod == 1) ? 1 : (mod == 2) ? 2 : 4;
    }
    return 4;
}

/* Escribe el resultado en el operando destino. En un sector de registro solo pisa
   esos bytes; un inmediato como destino es error. */
static void guardarResultado(TMV* mv, uint8_t tipo, int32_t raw_val, int32_t valor) {
    // Instrucción abortada: no escribe
    if (mv->errorFlag) return;

    if (tipo == 1) { // REGISTRO
        uint8_t num_reg = (uint8_t)(raw_val & 0x1F);
        uint8_t sector = (raw_val >> 5) & 0x03;

        if (sector == 0) mv->reg[num_reg] = valor;
        else if (sector == 1) mv->reg[num_reg] = (mv->reg[num_reg] & 0xFFFFFF00) | (valor & 0xFF);
        else if (sector == 2) mv->reg[num_reg] = (mv->reg[num_reg] & 0xFFFF00FF) | ((valor & 0xFF) << 8);
        else if (sector == 3) mv->reg[num_reg] = (mv->reg[num_reg] & 0xFFFF0000) | (valor & 0xFFFF);
    }
    else if (tipo == 3) { // MEMORIA
        uint8_t mod = (raw_val >> 5) & 0x07;
        uint32_t dir_logica;
        if (!calcularDireccionMemoria(mv, raw_val, &dir_logica)) return;

        uint16_t cant_bytes = (mod == 1) ? 1 : (mod == 2) ? 2 : 4;
        escribirMemoriaHardware(mv, dir_logica, cant_bytes, valor);
    }
    else{
        printf("Error: Intento de escritura en un operando no modificable.\n");
        mv->errorFlag = 1;
    }
}

/* Salto: IP = [segmento de CS | offset destino]. */
static void realizarSalto(TMV* mv, int32_t destino_offset) {
    mv->reg[IP] = (mv->reg[CS] & 0xFFFF0000) | ((uint32_t)destino_offset & 0xFFFF);
}

/* ========================================================================= */
/* REGISTRO CC (NZCV)                                                        */
/* ========================================================================= */

/* Recalcula CC con las banderas N, Z, C y V. */
static void actualizarCC(TMV* mv, int n, int z, int c, int v) {
    uint32_t nuevo_cc = 0;

    if (n) nuevo_cc |= CC_N;
    if (z) nuevo_cc |= CC_Z;
    if (c) nuevo_cc |= CC_C;
    if (v) nuevo_cc |= CC_V;

    mv->reg[CC] = (int32_t)nuevo_cc;
}

/* ========================================================================= */
/* AUXILIARES DE SYS                                                         */
/* ========================================================================= */

/* Lee una línea del teclado como número en base 2, 8, 10 o 16
   (acepta signo y prefijo 0b/0o/0x; saltea líneas vacías). */
static int32_t leerNumero(int base) {
    char linea[128];
    const char* p;
    do {
        if (fgets(linea, sizeof(linea), stdin) == NULL) return 0;

        // Línea más larga que el búfer: descarta el resto
        if (strchr(linea, '\n') == NULL) {
            int resto;
            while ((resto = getchar()) != '\n' && resto != EOF);
        }

        p = linea;
        while (isspace((unsigned char)*p)) p++;
    } while (*p == '\0');

    int negativo = 0;
    if (*p == '-' || *p == '+') {
        negativo = (*p == '-');
        p++;
    }

    // Prefijo opcional de la base
    char prefijo = (base == 2) ? 'b' : (base == 8) ? 'o' : (base == 16) ? 'x' : '\0';
    if (prefijo != '\0' && p[0] == '0' && tolower((unsigned char)p[1]) == prefijo) {
        p += 2;
    }

    // Sin signo para admitir los 32 bits completos (ej. 0xFFFFFFFF)
    uint32_t valor = (uint32_t)strtoul(p, NULL, base);
    return (int32_t)(negativo ? 0U - valor : valor);
}

/* Imprime en binario sin ceros a la izquierda (ej. 0b1001000). */
static void imprimirBinario(uint32_t valor) {
    printf(" 0b");
    int bit = 31;
    while (bit > 0 && !((valor >> bit) & 1U)) bit--;
    for (; bit >= 0; bit--) {
        putchar(((valor >> bit) & 1U) ? '1' : '0');
    }
}

/* ========================================================================= */
/* INSTRUCCIONES                                                             */
/* ========================================================================= */

// 0x00: SYS -> 0 limpiar pantalla, 1 READ, 2 WRITE, 3 leer string, 4 escribir string, F breakpoint.
// SYS 1/2: EDX = dirección, ECX = [tamaño de celda | cantidad], EAX = formato
static void op_sys(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;

    int32_t nro_servicio = obtenerValorOperando(mv, tA, vA);
    // Operando inválido: no ejecuta ningún servicio
    if (mv->errorFlag) return;

    uint32_t modo        = (uint32_t)mv->reg[EAX];
    uint32_t dir_logica  = (uint32_t)mv->reg[EDX];
    uint16_t cantidad    = (uint16_t)(mv->reg[ECX] & 0xFFFF);
    uint16_t tam_celda   = (uint16_t)((mv->reg[ECX] >> 16) & 0xFFFF);

    if (tam_celda == 0) tam_celda = 1;

    // SYS 1 y 2: celdas de 1 a 4 bytes
    if ((nro_servicio == 1 || nro_servicio == 2) && tam_celda > 4) {
        printf("Error: Tamano de celda invalido en SYS %d (%u bytes, maximo 4).\n", nro_servicio, tam_celda);
        mv->errorFlag = 1;
        return;
    }

    if (nro_servicio == 1) {
        // SYS 1 (READ): lee 'cantidad' valores del teclado, uno por celda
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
            printf("[%04X]: ", (unsigned int)dir_fisica);
            fflush(stdout);

            // Un valor por línea, en el formato que indique EAX
            int32_t entrada = 0;
            if (modo & 0x02) {
                // Carácter: primer carácter de la línea
                int c;
                do {
                    c = getchar();
                } while (c == '\n' || c == '\r');
                if (c != EOF) {
                    entrada = (int32_t)(char)c;
                    int resto;
                    while ((resto = getchar()) != '\n' && resto != EOF);
                }
            } else if (modo & 0x10) {
                entrada = leerNumero(2);    // Binario
            } else if (modo & 0x08) {
                entrada = leerNumero(16);   // Hexadecimal
            } else if (modo & 0x04) {
                entrada = leerNumero(8);    // Octal
            } else {
                entrada = leerNumero(10);   // Decimal
            }

            mv->reg[MBR] = entrada;

            // Guarda en la celda (Big Endian)
            for (int b = 0; b < tam_celda; b++) {
                int shift = (tam_celda - 1 - b) * 8;
                mv->mem[dir_fisica + b] = (uint8_t)((entrada >> shift) & 0xFF);
            }
        }
    }
    else if (nro_servicio == 2) {
        // SYS 2 (WRITE): muestra 'cantidad' celdas en todos los formatos pedidos en EAX
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

            // Lee la celda (Big Endian)
            uint32_t dato = 0;
            for (int b = 0; b < tam_celda; b++) {
                dato = (dato << 8) | mv->mem[dir_fisica + b];
            }

            // Decimal: con extensión de signo
            int32_t val_mostrar = (int32_t)dato;
            if (tam_celda == 1 && (val_mostrar & 0x80)) {
                val_mostrar |= 0xFFFFFF00;
            } else if (tam_celda == 2 && (val_mostrar & 0x8000)) {
                val_mostrar |= 0xFFFF0000;
            }

            mv->reg[MBR] = val_mostrar;
            printf("[%04X]:", (unsigned int)dir_fisica);

            // Binario, hexa y octal: solo los bytes de la celda (-1 en 1 byte = 0xFF)
            uint32_t sin_signo = dato;

            // Binario (0x10)
            if (modo & 0x10) {
                imprimirBinario(sin_signo);
            }
            // Hexadecimal (0x08)
            if (modo & 0x08) {
                if (tam_celda == 1)      printf(" 0x%02X", (unsigned int)sin_signo);
                else if (tam_celda == 2) printf(" 0x%04X", (unsigned int)sin_signo);
                else                     printf(" 0x%08X", (unsigned int)sin_signo);
            }
            // Octal (0x04)
            if (modo & 0x04) {
                printf(" 0o%o", (unsigned int)sin_signo);
            }
            // Caracteres (0x02): '.' si no es imprimible
            if (modo & 0x02) {
                printf(" ");
                for (int b = tam_celda - 1; b >= 0; b--) {
                    char c = (char)((val_mostrar >> (b * 8)) & 0xFF);
                    putchar((c >= 32 && c <= 126) ? c : '.');
                }
            }
            // Decimal (0x01)
            if (modo & 0x01) {
                printf(" %d", val_mostrar);
            }
            printf("\n");
        }
    }
    else if (nro_servicio == 3) {
        // SYS 3 (STRING READ): lee una línea y la guarda en EDX con '\0'.
        // ECX = máximo de caracteres (-1 = sin límite)
        int32_t maximo = mv->reg[ECX];
        int32_t leidos = 0;
        int c;

        fflush(stdout);
        while ((c = getchar()) != EOF && c != '\n') {
            if (c == '\r') continue;                          // fin de línea de Windows
            if (maximo != -1 && leidos >= maximo) continue;   // excede el máximo: se descarta
            escribirMemoriaHardware(mv, dir_logica + (uint32_t)leidos, 1, c);
            if (mv->errorFlag) return;
            leidos++;
        }
        escribirMemoriaHardware(mv, dir_logica + (uint32_t)leidos, 1, 0);
    }
    else if (nro_servicio == 4) {
        // SYS 4 (STRING WRITE): imprime desde EDX hasta el '\0'
        uint32_t dir = dir_logica;
        for (;;) {
            uint8_t c = (uint8_t)leerMemoriaHardware(mv, dir, 1);
            if (mv->errorFlag || c == 0) break;
            putchar(c);
            dir++;
        }
        fflush(stdout);
    }
    else if (nro_servicio == 0) {
        // SYS 0 (CLEAR SCREEN)
        fflush(stdout);
#ifdef _WIN32
        system("cls");
#else
        system("clear");
#endif
    }
    else if (nro_servicio == 0xF) {
        // SYS F (BREAKPOINT): solo marca la pausa; la hace ejecutarMV (requiere .vmi)
        if (mv->archVmi != NULL) mv->bpPendiente = 1;
    }
    // Otro servicio: se ignora
}

// 0x01: JMP (salto incondicional)
static void op_jmp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
}

// 0x02: JP (salta si > 0: N = 0 y Z = 0)
static void op_jp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (!(mv->reg[CC] & CC_N) && !(mv->reg[CC] & CC_Z)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x03: JN (salta si < 0: N = 1)
static void op_jn(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_N) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x04: JZ (salta si = 0: Z = 1)
static void op_jz(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_Z) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x05: JC (salta si C = 1)
static void op_jc(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_C) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x06: JV (salta si V = 1)
static void op_jv(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (mv->reg[CC] & CC_V) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x07: JNP (salta si <= 0: N = 1 o Z = 1)
static void op_jnp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if ((mv->reg[CC] & CC_N) || (mv->reg[CC] & CC_Z)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x08: JNN (salta si >= 0: N = 0)
static void op_jnn(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (!(mv->reg[CC] & CC_N)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x09: JNZ (salta si != 0: Z = 0)
static void op_jnz(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    if (!(mv->reg[CC] & CC_Z)) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x0A: NOT (A = ~A)
static void op_not(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t res  = ~valA;
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x0F: STOP (IP = -1: termina el ciclo)
static void op_stop(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tA; (void)vA; (void)tB; (void)vB;
    mv->reg[IP] = (int32_t)0xFFFFFFFF;
}

// 0x0B: PUSH (SP -= 4 y guarda A en el tope). Debajo de SS: stack overflow
static void op_push(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    // 1. SP -= 4 (en aritmética sin signo para evitar desborde indefinido)
    mv->reg[SP] = (int32_t)((uint32_t)mv->reg[SP] - 4U);
    // 2. Control de stack overflow
    if (mv->reg[SP] < mv->reg[SS]) {
        printf("Error: Stack overflow.\n");
        mv->errorFlag = 1;
        return;
    }
    // 3. Valor del operando (PUSH [SP+n] ya ve el SP decrementado)
    int32_t valor = obtenerValorOperando(mv, tA, vA);
    if (mv->errorFlag) return;
    // 4. Se escribe en el tope (4 bytes, Big Endian)
    escribirMemoriaHardware(mv, (uint32_t)mv->reg[SP], 4, valor);
}

// 0x0C: POP (A = tope y SP += 4). Pila vacía: stack underflow
static void op_pop(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    int32_t dir_fisica = traducirDireccion(mv, (uint32_t)mv->reg[SP], 4);
    if (dir_fisica < 0) {
        printf("Error: Stack underflow.\n");
        mv->errorFlag = 1;
        return;
    }
    // 1. Lee los 4 bytes del tope
    int32_t valor = leerMemoriaHardware(mv, (uint32_t)mv->reg[SP], 4);
    if (mv->errorFlag) return;
    // 2. Guarda en A (POP [SP+n] usa el SP sin incrementar)
    guardarResultado(mv, tA, vA, valor);
    if (mv->errorFlag) return;
    // 3. SP += 4
    mv->reg[SP] += 4;
}

// 0x0D: CALL (PUSH IP + JMP al operando)
static void op_call(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    // PUSH IP con control de stack overflow
    mv->reg[SP] = (int32_t)((uint32_t)mv->reg[SP] - 4U);
    if (mv->reg[SP] < mv->reg[SS]) {
        printf("Error: Stack overflow en CALL.\n");
        mv->errorFlag = 1;
        return;
    }
    // IP ya apunta a la siguiente instrucción: es la dirección de retorno
    escribirMemoriaHardware(mv, (uint32_t)mv->reg[SP], 4, mv->reg[IP]);
    if (mv->errorFlag) return;

    // JMP al destino
    int32_t destino_offset = obtenerValorOperando(mv, tA, vA);
    if (mv->errorFlag) return;
    mv->reg[IP] = (mv->reg[CS] & 0xFFFF0000) | ((uint32_t)destino_offset & 0xFFFF);
}

// 0x0E: RET (POP IP). Retorno -1 del main: fin del programa
static void op_ret(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tA; (void)vA; (void)tB; (void)vB;
    int32_t dir_fisica = traducirDireccion(mv, (uint32_t)mv->reg[SP], 4);
    if (dir_fisica < 0) {
        printf("Error: Stack underflow en RET.\n");
        mv->errorFlag = 1;
        return;
    }
    int32_t valor_retorno = leerMemoriaHardware(mv, (uint32_t)mv->reg[SP], 4);
    if (mv->errorFlag) return;
    mv->reg[IP] = valor_retorno;
    mv->reg[SP] += 4;
}

// 0x10: MOV (A = B)
static void op_mov(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, valB);
    actualizarCC(mv, valB < 0, valB == 0, 0, 0);
}

// 0x11: ADD (A = A + B)
static void op_add(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    // Suma sin signo: en C el desborde con signo es indefinido
    int32_t res = (int32_t)((uint32_t)a + (uint32_t)b);
    guardarResultado(mv, tA, vA, res);

    // C: la suma sin signo no entra en 32 bits
    uint64_t u_sum = (uint64_t)(uint32_t)a + (uint64_t)(uint32_t)b;
    int c = (u_sum >> 32) & 1;

    // V: operandos de igual signo y resultado de signo opuesto
    int v = ((a ^ res) & (b ^ res)) < 0;

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x12: SUB (A = A - B)
static void op_sub(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (int32_t)((uint32_t)a - (uint32_t)b);
    guardarResultado(mv, tA, vA, res);

    // C: acarreo de a + (complemento a 2 de b)
    uint64_t comp_b = (uint32_t)(~b) + 1ULL;
    uint64_t sum_sub = (uint64_t)(uint32_t)a + comp_b;
    int c = (sum_sub >> 32) & 1;

    // V: operandos de distinto signo y resultado con el signo de b
    int v = ((a ^ b) & (a ^ res)) < 0;

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x13: MUL (A = A * B). C y V si el producto no entra en 32 bits
static void op_mul(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int64_t prod64 = (int64_t)a * (int64_t)b;
    int32_t res = (int32_t)prod64;
    guardarResultado(mv, tA, vA, res);

    int desborde = (prod64 > INT32_MAX || prod64 < INT32_MIN);
    actualizarCC(mv, res < 0, res == 0, desborde, desborde);
}

// 0x14: DIV (A = A / B, resto en AC). B = 0: error
static void op_div(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    if (mv->errorFlag) return;

    if (b == 0) {
        printf("Error: No se puede dividir por 0.\n");
        mv->errorFlag = 1;
        return;
    }

    // Caso límite -2147483648 / -1: no entra en 32 bits -> V = 1 (evita que C aborte)
    int desborde = (a == INT32_MIN && b == -1);
    int32_t cociente = desborde ? INT32_MIN : a / b;
    int32_t resto    = desborde ? 0 : a % b;

    guardarResultado(mv, tA, vA, cociente);
    if (mv->errorFlag) return;
    mv->reg[AC] = resto;

    actualizarCC(mv, cociente < 0, cociente == 0, 0, desborde);
}

// 0x15: CMP (A - B solo para actualizar CC, no guarda)
static void op_cmp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (int32_t)((uint32_t)a - (uint32_t)b);

    uint64_t comp_b = (uint32_t)(~b) + 1ULL;
    uint64_t sum_sub = (uint64_t)(uint32_t)a + comp_b;
    int c = (sum_sub >> 32) & 1;
    int v = ((a ^ b) & (a ^ res)) < 0;   // igual que SUB

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x16: AND (A = A & B)
// AND/OR/XOR leen A y B en sentencias separadas para fijar el orden de acceso a memoria
static void op_and(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = a & b;
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x17: OR (A = A | B)
static void op_or(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = a | b;
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x18: XOR (A = A ^ B)
static void op_xor(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = a ^ b;
    guardarResultado(mv, tA, vA, res);
    actualizarCC(mv, res < 0, res == 0, 0, 0);
}

// 0x19: SWAP (intercambia A y B)
static void op_swap(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    // SWAP EAX, [EAX]: se escribe primero la memoria para no cambiar su dirección
    if (tA == 1 && tB == 3) {
        guardarResultado(mv, tB, vB, valA);
        guardarResultado(mv, tA, vA, valB);
    } else {
        guardarResultado(mv, tA, vA, valB);
        guardarResultado(mv, tB, vB, valA);
    }

    // CC según el nuevo valor de A (como el último XOR del intercambio)
    actualizarCC(mv, valB < 0, valB == 0, 0, 0);
}

// 0x1A: SHL (A = A << B)
static void op_shl(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t n = obtenerValorOperando(mv, tB, vB) & 0x1F; // 0..31 bits

    int32_t res = (int32_t)((uint32_t)a << n);
    guardarResultado(mv, tA, vA, res);

    // C: último bit que sale por la izquierda
    int c = (n > 0) ? (int)((((uint32_t)a) >> (32 - n)) & 1U) : 0;
    // V: A * 2^n no entra en 32 bits con signo
    int64_t producto = (int64_t)a * ((int64_t)1 << n);
    int v = (producto > INT32_MAX || producto < INT32_MIN);

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x1B: SHR (A = A >> B lógico: entra con ceros)
static void op_shr(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    uint32_t a = (uint32_t)obtenerValorOperando(mv, tA, vA);
    int32_t n  = obtenerValorOperando(mv, tB, vB) & 0x1F;

    // Operando de 1 o 2 bytes: se quita la extensión de signo de la lectura
    int bytes = cantBytesOperando(tA, vA);
    if (bytes < 4) a &= (1U << (8 * bytes)) - 1U;

    uint32_t res = (n == 0) ? a : (a >> n);
    guardarResultado(mv, tA, vA, (int32_t)res);

    // C: último bit que sale por la derecha
    int c = (n > 0) ? (int)((a >> (n - 1)) & 1U) : 0;
    actualizarCC(mv, (int32_t)res < 0, res == 0, c, 0);
}

// 0x1C: SAR (A = A >> B aritmético: conserva el signo)
static void op_sar(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t n = obtenerValorOperando(mv, tB, vB) & 0x1F;

    int32_t res = (n == 0) ? a : (a >> n);
    guardarResultado(mv, tA, vA, res);

    int c = (n > 0) ? (int)((((uint32_t)a) >> (n - 1)) & 1U) : 0;
    actualizarCC(mv, res < 0, res == 0, c, 0);
}

// 0x1D: LDL (carga B en los 2 bytes bajos de A)
static void op_ldl(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (a & (int32_t)0xFFFF0000) | (b & 0xFFFF);
    guardarResultado(mv, tA, vA, res);
}

// 0x1E: LDH (carga B en los 2 bytes altos de A)
static void op_ldh(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (int32_t)(((uint32_t)a & 0x0000FFFFU) | (((uint32_t)b & 0xFFFFU) << 16));
    guardarResultado(mv, tA, vA, res);
}

// 0x1F: RND (A = aleatorio entre 0 y B). No modifica CC
static void op_rnd(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = 0;
    if (b > 0) {
        // Tres rand() combinados para cubrir 32 bits (en Windows rand() llega a 32767)
        uint32_t aleatorio = ((uint32_t)rand() << 30) ^ ((uint32_t)rand() << 15) ^ (uint32_t)rand();
        res = (int32_t)(aleatorio % ((uint32_t)b + 1U));
    }
    guardarResultado(mv, tA, vA, res);
}

/* ========================================================================= */
/* TABLA DE DESPACHO                                                         */
/* ========================================================================= */
InstruccionFunc tablaInstrucciones[32];

/* Asocia cada opcode con su función: el ciclo de ejecución llama tablaInstrucciones[opcode]. */
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
    tablaInstrucciones[PUSH] = op_push;
    tablaInstrucciones[POP]  = op_pop;
    tablaInstrucciones[CALL] = op_call;
    tablaInstrucciones[RET]  = op_ret;
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
