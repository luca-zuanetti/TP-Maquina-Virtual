#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include "instrucciones.h"


/* ========================================================================= */
/* FUNCIONES AUXILIARES DE OPERANDOS Y MEMORIA                               */
/* ========================================================================= */

// Obtiene el valor real de un operando (sea Registro, Inmediato o Memoria)
int32_t obtenerValorOperando(TMV* mv, uint8_t tipo, int32_t raw_val) {
    if (tipo == 1) { // Registro
        int regCode = raw_val & 0x1F;
        return mv->reg[regCode];
    } 
    else if (tipo == 2) { // Inmediato
        return (int16_t)(raw_val & 0xFFFF);
    } 
    else if (tipo == 3) { // Memoria (Dirección lógica)
        int regCode = raw_val & 0x1F;
        int16_t offset = (int16_t)(raw_val >> 8);
        uint32_t base_seg_val = mv->reg[DS]; // Por defecto usa DS en esta parte
        uint16_t indice_seg = (base_seg_val >> 16) & 0xFFFF;
        
        uint32_t dir_logica = ((uint32_t)indice_seg << 16) | (uint16_t)(mv->reg[regCode] + offset);
        
        // Control de registros LAR y MAR según el TP
        mv->reg[LAR] = dir_logica;
        mv->reg[MAR] = (4 << 16);

        int32_t dir_fisica = traducirDireccion(mv, dir_logica, 4);
        if (dir_fisica == -1) {
            mv->errorFlag = 1;
            return 0;
        }
        mv->reg[MAR] = (mv->reg[MAR] & 0xFFFF0000) | (dir_fisica & 0xFFFF);

        // Leer 4 bytes de memoria
        int32_t val = 0;
        for (int i = 0; i < 4; i++) {
            val = (val << 8) | mv->mem[dir_fisica + i];
        }
        mv->reg[MBR] = val;
        return val;
    }
    return 0;
}

// Guarda un resultado en el destino (OpA)
void guardarResultado(TMV* mv, uint8_t tipo, int32_t raw_val, int32_t valor) {
    if (tipo == 1) { // Registro destino
        int regCode = raw_val & 0x1F;
        // No permitimos modificar IP o registros de sistema directamente si no corresponde
        if (regCode != IP) {
            mv->reg[regCode] = valor;
        }
    } 
    else if (tipo == 3) { // Memoria destino
        int regCode = raw_val & 0x1F;
        int16_t offset = (int16_t)(raw_val >> 8);
        uint32_t base_seg_val = mv->reg[DS];
        uint16_t indice_seg = (base_seg_val >> 16) & 0xFFFF;
        
        uint32_t dir_logica = ((uint32_t)indice_seg << 16) | (uint16_t)(mv->reg[regCode] + offset);
        
        mv->reg[LAR] = dir_logica;
        mv->reg[MAR] = (4 << 16);

        int32_t dir_fisica = traducirDireccion(mv, dir_logica, 4);
        if (dir_fisica == -1) {
            mv->errorFlag = 1;
            return;
        }
        mv->reg[MAR] = (mv->reg[MAR] & 0xFFFF0000) | (dir_fisica & 0xFFFF);
        mv->reg[MBR] = valor;

        // Escribir 4 bytes en memoria
        for (int i = 3; i >= 0; i--) {
            mv->mem[dir_fisica + i] = (valor & 0xFF);
            valor >>= 8;
        }
    }
}

// Función auxiliar para realizar el salto preservando el segmento de código actual en IP
static void realizarSalto(TMV* mv, int32_t destino) {
    uint32_t segmento_actual = mv->reg[IP] & 0xFFFF0000; // Mantiene los 16 bits altos (segmento)
    mv->reg[IP] = segmento_actual | ((uint32_t)destino & 0xFFFF); // Actualiza los 16 bits bajos (offset)
}

// --- ACTUALIZACIÓN DE BANDERAS CC ---
static void actualizarCC_Aritmetico(TMV* mv, int32_t valA, int32_t valB, int64_t res64) {
    int32_t res = (int32_t)res64;
    int n = (res < 0) ? 1 : 0;
    int z = (res == 0) ? 1 : 0;
    int c = (res64 > INT32_MAX || res64 < INT32_MIN) ? 1 : 0;
    int v = ((valA > 0 && valB > 0 && res < 0) || (valA < 0 && valB < 0 && res >= 0)) ? 1 : 0;
    mv->reg[CC] = (n << 3) | (z << 2) | (c << 1) | v;
}

static void actualizarCC_Logico(TMV* mv, int32_t res) {
    int n = (res < 0) ? 1 : 0;
    int z = (res == 0) ? 1 : 0;
    mv->reg[CC] = (n << 3) | (z << 2) | 0;
}

/* ========================================================================= */
/* IMPLEMENTACIÓN DE LAS 26 INSTRUCCIONES                                    */
/* ========================================================================= */

// 0x00: SYS
void op_sys(TMV* mv, uint8_t tipo_opA, int32_t valA, uint8_t tipo_opB, int32_t valB) {
    uint32_t modo = mv->reg[EAX];
    uint32_t dir_logica = mv->reg[EDX];
    uint16_t cantidad = mv->reg[ECX] & 0xFFFF;
    uint16_t tam_celda = (mv->reg[ECX] >> 16) & 0xFFFF;

    mv->reg[LAR] = dir_logica;
    mv->reg[MAR] = ((uint32_t)tam_celda << 16);

    int32_t dir_fisica = traducirDireccion(mv, dir_logica, tam_celda * cantidad);
    if (dir_fisica == -1) {
        printf("Error: Fallo de segmento en SYS\n");
        mv->errorFlag = 1;
        return;
    }
    mv->reg[MAR] = (mv->reg[MAR] & 0xFFFF0000) | (dir_fisica & 0xFFFF);

    if (modo == 1) { // MODO READ (Lectura por teclado)
        for (int i = 0; i < cantidad; i++) {
            uint32_t dir_fis_actual = dir_fisica + (i * tam_celda);
            printf("Ingrese valor para [%04X]: ", dir_fis_actual);
            
            uint32_t val = 0;
            if (scanf("%u", &val) != 1) {
                printf("Error al leer la entrada.\n");
                mv->errorFlag = 1;
                break;
            }
            
            mv->reg[MBR] = val;

            // Escribir el valor en la memoria física (Little Endian)
            uint32_t temp_val = val;
            for (int b = 0; b < tam_celda; b++) {
                mv->mem[dir_fis_actual + b] = (temp_val & 0xFF);
                temp_val >>= 8;
            }
        }
    } 
    else if (modo == 2) { // MODO WRITE (Escritura por pantalla)
        for (int i = 0; i < cantidad; i++) {
            uint32_t dir_fis_actual = dir_fisica + (i * tam_celda);
            printf("[%04X]: ", dir_fis_actual);
            uint32_t val = 0;
            for (int b = 0; b < tam_celda; b++) {
                val = (val << 8) | mv->mem[dir_fis_actual + b];
            }
            mv->reg[MBR] = val;
            printf("%u\n", val);
        }
    }
    else {
        printf("Error: Modo SYS desconocido (%u)\n", modo);
        mv->errorFlag = 1;
    }
}

// 0x01 - 0x09: Saltos (JMP, JP, JN, JZ, JC, JV, JNP, JNN, JNZ)

// 0x01: JMP (Salto incondicional)
void op_jmp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t destino = obtenerValorOperando(mv, tA, vA);
    realizarSalto(mv, destino);
}

// 0x02: JP / JGE (Salto si no negativo) -> N == 0
void op_jp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_N) == 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x03: JN (Salto si negativo) -> N == 1
void op_jn(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_N) != 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x04: JZ (Salto si cero) -> Z == 1
void op_jz(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_Z) != 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x05: JC (Salto si hay acarreo) -> C == 1
void op_jc(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_C) != 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x06: JV (Salto si hay desbordamiento) -> V == 1
void op_jv(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_V) != 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x07: JNP (Salto si no positivo)
void op_jnp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_N) != 0 || (mv->reg[CC] & FLAG_Z) != 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x08: JNN (Salto si no negativo) -> N == 0
void op_jnn(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_N) == 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x09: JNZ (Salto si no cero) -> Z == 0
void op_jnz(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    if ((mv->reg[CC] & FLAG_Z) == 0) {
        realizarSalto(mv, obtenerValorOperando(mv, tA, vA));
    }
}

// 0x0A: NOT
void op_not(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t res = ~valA;
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x0F: STOP
void op_stop(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    mv->reg[IP] = 0xFFFFFFFF;
}

// 0x10: MOV
void op_mov(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, valB);
    actualizarCC_Logico(mv, valB);
}

// 0x11: ADD
void op_add(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int64_t res64 = (int64_t)valA + (int64_t)valB;
    guardarResultado(mv, tA, vA, (int32_t)res64);
    actualizarCC_Aritmetico(mv, valA, valB, res64);
}

// 0x12: SUB
void op_sub(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int64_t res64 = (int64_t)valA - (int64_t)valB;
    guardarResultado(mv, tA, vA, (int32_t)res64);
    actualizarCC_Aritmetico(mv, valA, valB, res64);
}

// 0x13: MUL
void op_mul(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int64_t res64 = (int64_t)valA * (int64_t)valB;
    int32_t res = (int32_t)res64;
    guardarResultado(mv, tA, vA, res);
    
    int n = (res < 0) ? 1 : 0;
    int z = (res == 0) ? 1 : 0;
    int c = (res64 != (int64_t)res) ? 1 : 0;
    mv->reg[CC] = (n << 3) | (z << 2) | (c << 1) | c;
}

// 0x14: DIV
void op_div(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    if (valB == 0) {
        printf("Error: Division por cero.\n");
        mv->errorFlag = 1;
        return;
    }
    int32_t res = valA / valB;
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x15: CMP
void op_cmp(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int64_t res64 = (int64_t)valA - (int64_t)valB;
    actualizarCC_Aritmetico(mv, valA, valB, res64);
}

// 0x16: AND
void op_and(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = valA & valB;
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x17: OR
void op_or(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = valA | valB;
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x18: XOR
void op_xor(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = valA ^ valB;
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x19: SWAP
void op_swap(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    guardarResultado(mv, tA, vA, valB);
    guardarResultado(mv, tB, vB, valA);
    actualizarCC_Logico(mv, valB);
}

// 0x1A: SHL
void op_shl(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = valA << (valB & 0x1F);
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x1B: SHR
void op_shr(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    uint32_t valA = (uint32_t)obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = (int32_t)(valA >> (valB & 0x1F));
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x1C: SAR
void op_sar(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = valA >> (valB & 0x1F);
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

// 0x1D: LDL
void op_ldl(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = (valA & 0xFFFF0000) | (valB & 0xFFFF);
    guardarResultado(mv, tA, vA, res);
}

// 0x1E: LDH
void op_ldh(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t valA = obtenerValorOperando(mv, tA, vA);
    int32_t valB = obtenerValorOperando(mv, tB, vB);
    int32_t res = (valA & 0x0000FFFF) | ((valB & 0xFFFF) << 16);
    guardarResultado(mv, tA, vA, res);
}

// 0x1F: RND
void op_rnd(TMV* mv, uint8_t tA, int32_t vA, uint8_t tB, int32_t vB) {
    int32_t res = (rand() % 65536) - 32768;
    guardarResultado(mv, tA, vA, res);
    actualizarCC_Logico(mv, res);
}

/* ========================================================================= */
/* TABLA DE DESPACHO GLOBAL                                                  */
/* ========================================================================= */
InstruccionFunc tablaInstrucciones[32];

void inicializarTablaInstrucciones(void) {
    for(int i = 0; i < 32; i++) tablaInstrucciones[i] = NULL;
    
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