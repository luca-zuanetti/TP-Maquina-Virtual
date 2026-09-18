#ifndef INSTRUCCIONES_H
#define INSTRUCCIONES_H
#define FLAG_N (1 << 0) // Signo / Negativo
#define FLAG_Z (1 << 1) // Cero
#define FLAG_C (1 << 2) // Acarreo
#define FLAG_V (1 << 3) // Desbordamiento


#include "mv.h"

// Firma estándar para las funciones de instrucción de la máquina virtual
typedef void (*InstruccionFunc)(TMV* mv, uint8_t tipo_opA, int32_t valA, uint8_t tipo_opB, int32_t valB);

// Arreglo global de la tabla de instrucciones
extern InstruccionFunc tablaInstrucciones[32];

// Función para inicializar la tabla con los opcodes
void inicializarTablaInstrucciones(void);

#endif