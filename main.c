#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ram 16384

void inicializarRegistros(int registros[32]){
    for (int i = 0; i < 32; i++)
        registros[i] = 0;
    registros[26] = 0;
    registros[0] = registros[26];
    registros[27] = 1 << 16;
}

void inicializaTabla(short int tablaSegmentos[8][2]){
    int i, j;
    for (i=0; i < 8; i++){
        for (j=0; j < 2; j++)
            tablaSegmentos[i][j] = -1;
    }
}

int getTipoOperacion(char instruccion){
    if (instruccion & 0x10)
        return 1;
    else{
        if ((instruccion & 0xC0) != 0)
            return 2;
        else
            return 3;
    }
}


void procesar(char memoria[ram], int registros[32], short int tablaSegmentos[8][2]){
    int offset;
    int indTabla;
    int dir;
    int codOp;
    unsigned char instruccion;
    unsigned int tamañoInst = 0;
    while (registros[0] != -1){
        indTabla = (registros[0] >> 16) & 0xFFFF;
        offset = registros[0] & 0xFFFF;
        dir = tablaSegmentos[indTabla][0] + offset;
        if(offset > tablaSegmentos[indTabla][1]){
            registros[0] = -1;
            printf("Error: Fallo de segmento");
            exit(1);
        }
        else{
            instruccion = memoria[dir];
            codOp = instruccion & 0x1F;
            registros[1] = codOp;  
            if (codOp == 0x0F)
                registros[0] = -1;
            else{
                switch(getTipoOperacion(instruccion)){
                case 1:
                    
                }
            }
        }
    }
}

int getTipoOperando(char instruccion){

}


void leerArchivo(char nombreArch[], char memoria[ram], int registros[32], short int tablaSegmentos[8][2]){
    FILE *arch = fopen(nombreArch, "rb");
    if (arch == NULL){
        printf("Error al abrir el archivo.\n");
    }
    else{
        char id[6];
        char ver;
        char cod;
        short int tamCod;
        unsigned int i;
        
        fread(id, 1, 5, arch);
        id[5] = '\0';

        fread(&ver, 1, 1, arch);
        inicializaTabla(tablaSegmentos);
        fread(&tamCod, sizeof(short int), 1, arch);
        tamCod = (tamCod << 8) | ((tamCod >> 8) & 0x00FF);
        tablaSegmentos[0][0] = 0;
        tablaSegmentos[0][1] = tablaSegmentos[1][0] = tamCod;
        tablaSegmentos[1][1] = ram - tamCod;
        inicializarRegistros(registros);

        for (i = 0; i < tamCod; i++){
            fread(&cod, sizeof(char), 1, arch);
            memoria[i] = cod;
            printf("Byte %d: %02X\n", i, (unsigned char)cod); 
        }
    }
    fclose(arch);
}


void main(){
    char nombre[50];
    strcpy(nombre, "TP3EJ7.vmx");
    int tablaSegmentos[8][2];
    int registros[32];
    char memoria[ram];
    leerArchivo(nombre, memoria, registros, tablaSegmentos);

}