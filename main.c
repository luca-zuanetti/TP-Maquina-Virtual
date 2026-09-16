#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define RAM_SIZE   16384
#define SEG_AMOUNT 8

typedef struct {
    uint16_t base;
    uint16_t size;
} TableSeg;

typedef struct {
    uint8_t  mem[RAM_SIZE];       // unsigned char para evitar bugs de signo
    int32_t  reg[32];
    TableSeg seg[SEG_AMOUNT];
    int      errorFlag;
} TMV;

void inicializarSegmentosYRegistros(TMV* mv, uint16_t tamCod) {
    // 1. Inicializar tabla con -1 (0xFFFF)
    for (int i = 0; i < SEG_AMOUNT; i++) {
        mv->seg[i].base = 0xFFFF;
        mv->seg[i].size = 0xFFFF;
    }

    // 2. Configurar CS (0) y DS (1)
    mv->seg[0].base = 0;
    mv->seg[0].size = tamCod;
    mv->seg[1].base = tamCod;
    mv->seg[1].size = RAM_SIZE - tamCod;

    // 3. Inicializar registros
    memset(mv->reg, 0, sizeof(mv->reg));
    mv->reg[26] = 0x00000000;          // CS: segmento 0, offset 0
    mv->reg[27] = 0x00010000;          // DS: segmento 1, offset 0
    mv->reg[0]  = mv->reg[26];         // IP arranca en CS
}

int cargarArchivo(TMV* mv, const char* nombreArch) {
    FILE* arch = fopen(nombreArch, "rb");
    if (!arch) {
        fprintf(stderr, "Error: No se pudo abrir el archivo %s\n", nombreArch);
        return 0;
    }
    
    uint8_t header[8];
    if (fread(header, 1, 8, arch) != 8) {
        fclose(arch);
        return 0;
    }

    // Validar identificador "VMX26" y versión 1
    if (memcmp(header, "VMX26", 5) != 0 || header[5] != 1) {
        fprintf(stderr, "Error: Archivo incompatible o cabecera inválida.\n");
        fclose(arch);
        return 0;
    }

    // Tamaño de código en Big Endian
    uint16_t tamCod = ((uint16_t)header[6] << 8) | header[7];

    inicializarSegmentosYRegistros(mv, tamCod);

    // Lectura directa en bloque a la memoria RAM
    fread(mv->mem, 1, tamCod, arch);
    fclose(arch);
    return 1;
}