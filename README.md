# 🖥️ Máquina Virtual - Parte I (Arquitectura 2026)
Trabajo Práctico para la asignatura **Fundamentos de la Arquitectura de Computadoras**  
Facultad de Ingeniería - Universidad Nacional de Mar del Plata (UNMDP)

---

## 1. Descripción General
Este proyecto implementa un emulador o Máquina Virtual capaz de ejecutar programas compilados a lenguaje máquina con extensión `.vmx`, traducidos por la herramienta `vmt` provista por la cátedra a partir de archivos fuente Assembler (`.asm`).

El emulador simula a bajo nivel los subsistemas esenciales de un computador:
- Memoria RAM física de direccionamiento lineal.
- Segmentación de memoria mediante tabla de descriptores para Código y Datos.
- Banco de registros de propósito general, punteros y registros de control de bus.
- Ciclo de instrucción estructurado en **Fetch**, **Decode**, **Update IP** y **Execute**.
- Registro de Códigos de Condición (`CC`) con banderas aritmético-lógicas `NZCV`.

---

## 2. Componentes de la Arquitectura

### 2.1 Memoria Principal (RAM)
- Tamaño físico fijo de **16 KiB** (16.384 celdas de 1 byte).
- Rango de direcciones físicas válidas: `0x0000` hasta `0x3FFF`.

### 2.2 Tabla de Descriptores de Segmentos
Consta de 8 entradas de 32 bits (16 bits de dirección base física y 16 bits de tamaño en bytes):
- **Entrada 0 (Segmento de Código - CS):** Base `0x0000`, tamaño determinado por la cabecera del binario `.vmx`.
- **Entrada 1 (Segmento de Datos - DS):** Base física contigua al final del código, tamaño restante de la RAM (`16 KiB - tamCod`).
- **Entradas 2 a 7 (Inactivas):** Marcadas con el valor de descriptor inactivo `-1` (`0xFFFF` en base y tamaño).

### 2.3 Banco de Registros (32 registros de 32 bits)
| Índice | Mnemónico | Rol / Descripción |
| :---: | :---: | :--- |
| `0` | **IP** | Puntero de instrucción (Instruction Pointer) |
| `1` | **OPC** | Código de operación en ejecución |
| `2` | **OP1** | Metadatos y valor del primer operando (Destino) |
| `3` | **OP2** | Metadatos y valor del segundo operando (Fuente) |
| `4` | **LAR** | Dirección lógica del último acceso a memoria |
| `5` | **MAR** | Cantidad de bytes en parte alta y dirección física en parte baja |
| `6` | **MBR** | Búfer de datos leídos o escritos en memoria |
| `10..15` | **EAX..EFX** | Registros de propósito general para cómputo |
| `16` | **AC** | Acumulador / Resto en división entera |
| `17` | **CC** | Registro de Códigos de Condición |
| `26` | **CS** | Puntero lógico base del segmento de código (`0x00000000`) |
| `27` | **DS** | Puntero lógico base del segmento de datos (`0x00010000`) |

---

## 3. Registro CC y Banderas Aritméticas (NZCV)
El registro de estado utiliza los 4 bits más significativos para registrar el estado de la ALU:
- **Bit 31 (N - Signo):** Se activa en `1` si el bit 31 del resultado es 1 (número negativo en C2).
- **Bit 30 (Z - Cero):** Se activa en `1` si el resultado de la operación es exactamente 0.
- **Bit 29 (C - Acarreo / Carry):** Se activa en `1` ante desbordamiento sin signo o desplazamiento de bits fuera del operando.
- **Bit 28 (V - Overflow):** Se activa en `1` cuando una operación aritmética signada produce un desbordamiento incoherente en C2.

---

## 4. Traducción y Protección de Memoria
Toda dirección lógica se estructura en 32 bits: `[16 bits Segmento | 16 bits Desplazamiento]`:
1. Se extrae el índice de segmento (`dir_logica >> 16`). Si es $\ge 8$ o el descriptor está inactivo (`0xFFFF`), se produce **Fallo de Segmento**.
2. Se extrae el desplazamiento (`dir_logica & 0xFFFF`).
3. Se calcula la dirección física tentativa: `dir_fisica = seg[indice].base + desplazamiento`.
4. Se verifica que `dir_fisica + cant_bytes <= seg[indice].base + seg[indice].size`. De violarse esta regla, se interrumpe el proceso por **Fallo de Segmento**.

---

## 5. Estructura del Código Fuente
- `mv.h`: Contrato global de arquitectura, estructuras `TMV` y `TableSeg`, definiciones de flags y prototipos.
- `instrucciones.c`: Implementación de las 26 instrucciones, resolución de operandos (`[reg+offset]`), actualización del bus (`LAR`, `MAR`, `MBR`) y evaluación de flags `NZCV`.
- `dissasembler.c`: Desensamblado de instrucciones en memoria principal respetando el formato visual oficial.
- `main.c`: Lectura y validación de la cabecera `VMX26`, inicialización del sistema y ciclo de ejecución.

---

## 6. Compilación y Uso

### Compilación con GCC:
```bash
gcc -Wall -Wextra -std=c99 main.c instrucciones.c dissasembler.c -o vmx