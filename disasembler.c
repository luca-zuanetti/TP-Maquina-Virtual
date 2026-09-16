void imprimirOperando(uint8_t tipo, int32_t valor) {
    if (tipo == 1) { // Operando Registro
        int regCode = valor & 0x1F;
        if (regCode >= 0 && regCode < 32) {
            printf("%s", regStr[regCode]);
        }
    } else if (tipo == 2) { //Operando Inmediato
        printf("%d", (int16_t)valor);
    } else if (tipo == 3) { // Operando Memoria
        int regCode = valor & 0x1F;
        int16_t offset = (int16_t)(valor >> 8);
        if (regCode >= 0 && regCode < 32) {
            if (offset > 0)
            printf("[%s%+d]", regStr[regCode], offset);
            else
                if (offset < 0)
                    printf("[%s%-d]", regStr[regCode], offset);
                else
                    printf("[%s]", regStr[regCode]);
        } else {
            printf("[%d]", offset);
        }
    }
}

void ejecutarDisassembler(TMV* mv) {
    uint16_t csIndex = 0; // Segmento de código
    uint32_t baseCod = mv->seg[csIndex].base;
    uint32_t tamCod  = mv->seg[csIndex].size;
    
    uint32_t offset_actual = 0;


    while (offset_actual < tamCod) {
        // Dirección física donde comienza la instrucción actual
        uint32_t dir_fisica_inst = baseCod + offset_actual;

        uint8_t primer_byte = mv->mem[dir_fisica_inst];
        uint8_t opcode = primer_byte & 0x1F;
        uint8_t tipo_opA = (primer_byte >> 4) & 0x03;
        uint8_t tipo_opB = (primer_byte >> 6) & 0x03;

        uint32_t offset_lectura = 1; // Cuenta el primer byte
        int32_t valorB = 0;
        int32_t valorA = 0;

        if (tipo_opB > 0) {
            for (int i = 0; i < tipo_opB; i++) {
                valorB = (valorB << 8) | mv->mem[dir_fisica_inst + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opB == 2) valorB = (int16_t)valorB;
        }

        if (tipo_opA > 0) {
            for (int i = 0; i < tipo_opA; i++) {
                valorA = (valorA << 8) | mv->mem[dir_fisica_inst + offset_lectura];
                offset_lectura++;
            }
            if (tipo_opA == 2) valorA = (int16_t)valorA;
        }

        printf("[%04X] ", dir_fisica_inst);

        for (uint32_t i = 0; i < offset_lectura; i++) {
            printf("%02X ", mv->mem[dir_fisica_inst + i]);
        }

        // Espaciado estético para alinear
        for (uint32_t i = offset_lectura; i < 6; i++) {
            printf("   ");
        }
        printf(" | ");

        const char* mnem = (opcode <= 0x1F && mnemStr[opcode] != NULL) ? mnemStr[opcode] : "UNKNOWN";

        printf("%-5s", mnem);

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
    printf("-----------------------------------------\n");
}