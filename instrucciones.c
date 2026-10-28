#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
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
static int32_t leerMemoriaHardware(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes) {
    // 1. El procesador coloca la dirección lógica en LAR
    mv->reg[LAR] = (int32_t)dir_logica;

     // 2. La MMU traduce la dirección lógica a física y valida los límites del segmento
    int32_t dir_fisica = traducirDireccion(mv, dir_logica, cant_bytes);
    
    if (dir_fisica < 0) {
        printf("Error: Fallo de segmento en lectura (Dir: 0x%08X).\n", dir_logica);
        mv->errorFlag = 1;
        return 0;
    }

    // 3. Se configura MAR:
    //    - 16 bits superiores: cantidad de bytes transferidos
    //    - 16 bits inferiores: dirección física en la RAM
    mv->reg[MAR] = (int32_t)(((uint32_t)cant_bytes << 16) | ((uint32_t)dir_fisica & 0xFFFFU));
    
    int32_t dato = 0;
    // Decodifica según la cantidad de bytes solicitada
    if (cant_bytes == 1) {
        dato = (int32_t)(int8_t)mv->mem[dir_fisica]; // Extensión de signo
    } else if (cant_bytes == 2) {
        dato = (int32_t)(int16_t)((mv->mem[dir_fisica] << 8) | mv->mem[dir_fisica + 1]);
    } else { //Asume cant_bytes == 4
        // Se arma en uint32_t: desplazar un byte >= 0x80 a la posición 24 de un int es indefinido
        dato = (int32_t)(((uint32_t)mv->mem[dir_fisica] << 24) | ((uint32_t)mv->mem[dir_fisica + 1] << 16) |
                         ((uint32_t)mv->mem[dir_fisica + 2] << 8) | (uint32_t)mv->mem[dir_fisica + 3]);
    }
    
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
void escribirMemoriaHardware(TMV* mv, uint32_t dir_logica, uint16_t cant_bytes, int32_t valor) {
    mv->reg[LAR] = (int32_t)dir_logica;
    int32_t dir_fisica = traducirDireccion(mv, dir_logica, cant_bytes);
    
    if (dir_fisica < 0) {
        printf("Error: Fallo de segmento en escritura (Dir: 0x%08X).\n", dir_logica);
        mv->errorFlag = 1; 
        return;
    }
    
    mv->reg[MAR] = (int32_t)(((uint32_t)cant_bytes << 16) | ((uint32_t)dir_fisica & 0xFFFFU));
    mv->reg[MBR] = valor;

    // Descompone el valor en memoria según los bytes indicados (big endian)
    if (cant_bytes == 1) {
        mv->mem[dir_fisica] = (uint8_t)(valor & 0xFF);
    } 
    else if (cant_bytes == 2) {
        mv->mem[dir_fisica]     = (uint8_t)((valor >> 8) & 0xFF);
        mv->mem[dir_fisica + 1] = (uint8_t)(valor & 0xFF);
    } 
    else { // Asume cant_bytes == 4
        mv->mem[dir_fisica]     = (uint8_t)((valor >> 24) & 0xFF);
        mv->mem[dir_fisica + 1] = (uint8_t)((valor >> 16) & 0xFF);
        mv->mem[dir_fisica + 2] = (uint8_t)((valor >> 8)  & 0xFF);
        mv->mem[dir_fisica + 3] = (uint8_t)(valor         & 0xFF);
    }
}

/**
 * @brief Calcula la dirección lógica de un operando de memoria [reg + offset].
 *
 * El desplazamiento se suma solo a los 16 bits bajos (offset) del registro base,
 * de modo que nunca altera los 16 bits altos (índice de segmento). Si el offset
 * resultante queda fuera de [0, 0xFFFF] se produce un fallo de segmento.
 *
 * @param mv         Puntero a la Máquina Virtual.
 * @param raw_val    Valor bruto del operando de memoria (offset de 16 bits | modificador | registro).
 * @param dir_logica Salida: dirección lógica [Segmento:Offset] calculada.
 * @return int       1 si la dirección es válida, 0 ante fallo de segmento (con errorFlag en 1).
 */
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
    // Si un operando anterior ya falló, la instrucción quedó abortada: no se accede a memoria
    if (mv->errorFlag) return 0;

    if (tipo == 1) { // REGISTRO
        // Enmascaramos a 5 bits para garantizar un índice de registro válido (0..31)
        uint8_t num_reg = (uint8_t)(raw_val & 0x1F);
        uint8_t sector = (raw_val >> 5) & 0x03; // Máscara para extraer los 2 bits de sector
        
        // Retorna el valor enmascarado y extendido en signo según el sector
        if (sector == 0) return mv->reg[num_reg];                                             // EXT (4 bytes)
        else if (sector == 1) return (int32_t)(int8_t)(mv->reg[num_reg] & 0xFF);              // AL (4to byte)
        else if (sector == 2) return (int32_t)(int8_t)((mv->reg[num_reg] >> 8) & 0xFF);       // AH (3er byte)
        else return (int32_t)(int16_t)(mv->reg[num_reg] & 0xFFFF);                            // AX (2 bytes)
    } 
    else if (tipo == 2) { // INMEDIATO
        // Casteo a int16_t para forzar la extensión de signo a 32 bits
        return (int32_t)((int16_t)(raw_val & 0xFFFF));
    } 
    else if (tipo == 3) { // MEMORIA
        // Formato de memoria: 16 bits de offset con signo + 8 bits de modificador y registro base
        uint8_t mod = (raw_val >> 5) & 0x07; // Máscara de 3 bits para modificador

        // La dirección lógica resulta de sumar el offset al puntero del registro base
        uint32_t dir_logica;
        if (!calcularDireccionMemoria(mv, raw_val, &dir_logica)) return 0;

        // Decodifica: 001 (b) -> 1, 010 (w) -> 2, 100 (l) -> 4
        uint16_t cant_bytes = (mod == 1) ? 1 : (mod == 2) ? 2 : 4;
        return leerMemoriaHardware(mv, dir_logica, cant_bytes);
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
    // Una instrucción abortada (fallo al leer sus operandos) no escribe su resultado
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
/* AUXILIARES DE LAS LLAMADAS AL SISTEMA                                     */
/* ========================================================================= */

/**
 * @brief Lee una línea del teclado y la interpreta como un número en la base
 *        indicada (2, 8, 10 o 16). Acepta signo y el prefijo de la base
 *        (0b, 0o o 0x). Consume la línea completa, incluido el Enter; las
 *        líneas en blanco se saltean (se sigue esperando el valor).
 *
 * @return int32_t Valor leído (0 si la línea no contiene un número válido).
 */
static int32_t leerNumero(int base) {
    char linea[128];
    const char* p;
    do {
        if (fgets(linea, sizeof(linea), stdin) == NULL) return 0;

        // Si la línea no entró completa en el búfer se descarta el resto
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

    // Prefijo opcional de la base (0b, 0o, 0x)
    char prefijo = (base == 2) ? 'b' : (base == 8) ? 'o' : (base == 16) ? 'x' : '\0';
    if (prefijo != '\0' && p[0] == '0' && tolower((unsigned char)p[1]) == prefijo) {
        p += 2;
    }

    // Se interpreta sin signo para admitir los 32 bits completos (por ejemplo 0xFFFFFFFF)
    uint32_t valor = (uint32_t)strtoul(p, NULL, base);
    return (int32_t)(negativo ? 0U - valor : valor);
}

/**
 * @brief Imprime un valor en binario, sin ceros a la izquierda (formato del
 *        ejemplo oficial: 'H' -> 0b1001000).
 */
static void imprimirBinario(uint32_t valor) {
    printf(" 0b");
    int bit = 31;
    while (bit > 0 && !((valor >> bit) & 1U)) bit--;   // salta los ceros a la izquierda
    for (; bit >= 0; bit--) {
        putchar(((valor >> bit) & 1U) ? '1' : '0');
    }
}

/* ========================================================================= */
/* IMPLEMENTACIÓN DE LAS 26 INSTRUCCIONES                                    */
/* ========================================================================= */

// 0x00: SYS (Llamadas al Sistema: 0 = CLEAR SCREEN, 1 = READ, 2 = WRITE,
//            3 = STRING READ, 4 = STRING WRITE, F = BREAKPOINT)
void op_sys(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB; // SYS utiliza solo el primer operando

    int32_t nro_servicio = obtenerValorOperando(mv, tA, vA);
    // Si el operando falló (por ejemplo SYS [KS] sin Const Segment) no se ejecuta
    // ningún servicio: con el 0 devuelto se limpiaría la pantalla y se borraría el error
    if (mv->errorFlag) return;

    uint32_t modo        = (uint32_t)mv->reg[EAX];
    uint32_t dir_logica  = (uint32_t)mv->reg[EDX];
    uint16_t cantidad    = (uint16_t)(mv->reg[ECX] & 0xFFFF);
    uint16_t tam_celda   = (uint16_t)((mv->reg[ECX] >> 16) & 0xFFFF);

    if (tam_celda == 0) tam_celda = 1;

    // Las celdas de SYS 1 y 2 se convierten a un valor de 32 bits: no pueden superar 4 bytes
    if ((nro_servicio == 1 || nro_servicio == 2) && tam_celda > 4) {
        printf("Error: Tamano de celda invalido en SYS %d (%u bytes, maximo 4).\n", nro_servicio, tam_celda);
        mv->errorFlag = 1;
        return;
    }

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

            // Se lee un valor por línea: el resto de la línea (incluido el Enter) se
            // descarta para que una lectura posterior (por ejemplo SYS 3) no lo tome
            int32_t entrada = 0;
            if (modo & 0x02) {
                // Modo Carácter
                char c = 0;
                if (scanf(" %c", &c) == 1) entrada = (int32_t)c;
                int resto;
                while ((resto = getchar()) != '\n' && resto != EOF);
            } else if (modo & 0x10) {
                entrada = leerNumero(2);    // Modo Binario
            } else if (modo & 0x08) {
                entrada = leerNumero(16);   // Modo Hexadecimal
            } else if (modo & 0x04) {
                entrada = leerNumero(8);    // Modo Octal
            } else {
                entrada = leerNumero(10);   // Modo Decimal por defecto
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

            // Binario, hexadecimal y octal muestran solo los bytes de la celda
            // (un -1 de 1 byte es 0b11111111, 0xFF y 0o377)
            uint32_t sin_signo = dato;

            // Salida en Binario (Bit 4 = 0x10)
            if (modo & 0x10) {
                imprimirBinario(sin_signo);
            }
            // Salida en Hexadecimal (Bit 3 = 0x08)
            if (modo & 0x08) {
                if (tam_celda == 1)      printf(" 0x%02X", (unsigned int)sin_signo);
                else if (tam_celda == 2) printf(" 0x%04X", (unsigned int)sin_signo);
                else                     printf(" 0x%08X", (unsigned int)sin_signo);
            }
            // Salida en Octal (Bit 2 = 0x04)
            if (modo & 0x04) {
                printf(" 0o%o", (unsigned int)sin_signo);
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
    else if (nro_servicio == 3) {
        // SYS STRING READ: lee una línea y la guarda desde EDX terminada en '\0'.
        // ECX = máximo de caracteres (-1: sin límite); lo que exceda se descarta.
        // Sin prompt [XXXX]: (la consigna no lo muestra para strings)
        int32_t maximo = mv->reg[ECX];
        int32_t leidos = 0;
        int c;

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
        // SYS STRING WRITE: imprime desde EDX hasta encontrar '\0'
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
        // SYS CLEAR SCREEN: limpia la pantalla (no usa ni modifica registros).
        // Antes se vuelca la salida pendiente para que no aparezca después de limpiar
        fflush(stdout);
#ifdef _WIN32
        system("cls");
#else
        system("clear");
#endif
    }
    else if (nro_servicio == 0xF) {
        // SYS BREAKPOINT: solo si se indicó un .vmi; la pausa la realiza el ciclo de ejecución
        if (mv->archVmi != NULL) mv->bpPendiente = 1;
    }
    // Cualquier otro número de servicio se ignora
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

// 0x0B: PUSH (Apila un valor de 4 bytes)
void op_push(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    // 1. Decrementar SP en 4
    mv->reg[SP] -= 4;
    // 2. Si SP queda por debajo del comienzo del Stack Segment: stack overflow
    if (mv->reg[SP] < mv->reg[SS]) {
        printf("Error: Stack overflow.\n");
        mv->errorFlag = 1;
        return;
    }
    // 3 y 4. Obtener el valor del operando, ya extendido a 4 bytes
    //        (PUSH SP o PUSH [SP+n] usan el SP ya decrementado)
    int32_t valor = obtenerValorOperando(mv, tA, vA);
    if (mv->errorFlag) return;
    // 5. Guardarlo en la pila en big endian: el byte más significativo queda en el tope
    escribirMemoriaHardware(mv, (uint32_t)mv->reg[SP], 4, valor);
}

// 0x0C: POP (Desapila 4 bytes hacia un operando)
void op_pop(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    int32_t dir_fisica = traducirDireccion(mv, (uint32_t)mv->reg[SP], 4);
    if (dir_fisica < 0) {
        printf("Error: Stack underflow.\n");
        mv->errorFlag = 1;
        return;
    }
    // 1 a 3. Extraer los 4 bytes del tope (el byte del tope es el más significativo)
    int32_t valor = leerMemoriaHardware(mv, (uint32_t)mv->reg[SP], 4);
    if (mv->errorFlag) return;
    // 4. Asignar al operando (si es menor a 4 bytes se truncan los más significativos)
    //    POP [SP+n] calcula la dirección con el SP todavía sin incrementar
    guardarResultado(mv, tA, vA, valor);
    if (mv->errorFlag) return;
    // 5. Incrementar SP en 4
    mv->reg[SP] += 4;
}

// 0x0D: CALL (Llamada a subrutina)
void op_call(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    (void)tB; (void)vB;
    // Igual que en PUSH, el destino se lee antes de mover SP (CALL [SP+n] usa el SP original)
    int32_t destino_offset = obtenerValorOperando(mv, tA, vA);
    if (mv->errorFlag) return;

    mv->reg[SP] -= 4;
    if (mv->reg[SP] < mv->reg[SS]) {
        printf("Error: Stack overflow en CALL.\n");
        mv->errorFlag = 1;
        return;
    }
    escribirMemoriaHardware(mv, (uint32_t)mv->reg[SP], 4, mv->reg[IP]);
    if (mv->errorFlag) return;
    mv->reg[IP] = (mv->reg[CS] & 0xFFFF0000) | ((uint32_t)destino_offset & 0xFFFF);
}

// 0x0E: RET (Retorno de subrutina)
void op_ret(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
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
    // La suma se hace sin signo: el desborde de un int32_t es indefinido en C y,
    // compilando con optimizaciones, el compilador puede eliminar la detección de V
    int32_t res = (int32_t)((uint32_t)a + (uint32_t)b);
    guardarResultado(mv, tA, vA, res);

    // Carry: suma sin signo excede 32 bits
    uint64_t u_sum = (uint64_t)(uint32_t)a + (uint64_t)(uint32_t)b;
    int c = (u_sum >> 32) & 1;

    // Overflow: suma de operandos del mismo signo produce resultado de signo opuesto
    int v = ((a ^ res) & (b ^ res)) < 0;

    actualizarCC(mv, res < 0, res == 0, c, v);
}

// 0x12: SUB (Resta: A = A - B)
void op_sub(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (int32_t)((uint32_t)a - (uint32_t)b);   // sin signo: ver op_add
    guardarResultado(mv, tA, vA, res);

    // Carry en resta: carry-out de la suma a + (~b + 1)
    uint64_t comp_b = (uint32_t)(~b) + 1ULL;
    uint64_t sum_sub = (uint64_t)(uint32_t)a + comp_b;
    int c = (sum_sub >> 32) & 1;

    // Overflow con signo en resta: operandos de distinto signo y el resultado
    // con el signo de b (incluye 0 - (-2147483648))
    int v = ((a ^ b) & (a ^ res)) < 0;

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
    if (mv->errorFlag) return; // Evita un falso "dividir por 0" tras un fallo de segmento

    if (b == 0) {
        printf("Error: No se puede dividir por 0.\n");
        mv->errorFlag = 1;
        return;
    }

    // -2147483648 / -1 no entra en 32 bits: en C es indefinido y en x86 aborta el
    // proceso anfitrión. Se resuelve como el hardware de 32 bits: cociente
    // -2147483648 (desborde, V = 1) y resto 0
    int desborde = (a == INT32_MIN && b == -1);
    int32_t cociente = desborde ? INT32_MIN : a / b;
    int32_t resto    = desborde ? 0 : a % b;

    guardarResultado(mv, tA, vA, cociente);
    if (mv->errorFlag) return;
    mv->reg[AC] = resto;

    actualizarCC(mv, cociente < 0, cociente == 0, 0, desborde);
}

// 0x15: CMP (Compara A con B calculando A - B sin guardar el resultado)
void op_cmp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = (int32_t)((uint32_t)a - (uint32_t)b);   // sin signo: ver op_add

    uint64_t comp_b = (uint32_t)(~b) + 1ULL;
    uint64_t sum_sub = (uint64_t)(uint32_t)a + comp_b;
    int c = (sum_sub >> 32) & 1;
    int v = ((a ^ b) & (a ^ res)) < 0;   // mismo criterio que op_sub

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

    // SWAP equivale a XOR A,B / XOR B,A / XOR A,B y afecta CC como el último XOR,
    // cuyo resultado es el nuevo valor de A (el B original)
    actualizarCC(mv, valB < 0, valB == 0, 0, 0);
}

// 0x1A: SHL (Desplazamiento lógico a la izquierda)
void op_shl(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t a = obtenerValorOperando(mv, tA, vA);
    int32_t n = obtenerValorOperando(mv, tB, vB) & 0x1F; // Acotado a 0..31 bits

    // Corrimiento sin signo: desplazar un int32_t negativo a la izquierda es indefinido
    int32_t res = (int32_t)((uint32_t)a << n);
    guardarResultado(mv, tA, vA, res);

    // Carry: último bit desplazado fuera del límite de 32 bits
    int c = (n > 0) ? (int)((((uint32_t)a) >> (32 - n)) & 1U) : 0;
    // Overflow: SHL equivale a multiplicar por 2^n; V se enciende si ese producto no
    // entra en 32 bits con signo (tabla oficial: SHL 1073741824, 2 -> 0 con C = 1 y V = 1)
    int64_t producto = (int64_t)a * ((int64_t)1 << n);
    int v = (producto > INT32_MAX || producto < INT32_MIN);

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
    int32_t res = (int32_t)(((uint32_t)a & 0x0000FFFFU) | (((uint32_t)b & 0xFFFFU) << 16));
    guardarResultado(mv, tA, vA, res);
}

// 0x1F: RND (Genera un número pseudoaleatorio entre 0 y B). No afecta CC: no está
// en la lista de instrucciones que lo modifican
void op_rnd(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t b = obtenerValorOperando(mv, tB, vB);
    int32_t res = 0;
    if (b > 0) {
        // rand() puede llegar solo a 32767 (Windows): se combinan tres llamadas para cubrir 32 bits
        uint32_t aleatorio = ((uint32_t)rand() << 30) ^ ((uint32_t)rand() << 15) ^ (uint32_t)rand();
        res = (int32_t)(aleatorio % ((uint32_t)b + 1U));
    }
    guardarResultado(mv, tA, vA, res);
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
