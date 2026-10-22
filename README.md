# 🖥️ Máquina Virtual - Parte I (Arquitectura 2026)
Trabajo Práctico para la asignatura **Fundamentos de la Arquitectura de Computadoras**  
Facultad de Ingeniería - Universidad Nacional de Mar del Plata (UNMDP)

---

## 1. Descripción General
Este proyecto implementa un emulador o Máquina Virtual capaz de ejecutar programas compilados a lenguaje máquina con extensión `.vmx`, traducidos por la herramienta `vmt` provista por la cátedra a partir de archivos fuente Assembler (`.asm`).

El emulador simula a bajo nivel los subsistemas esenciales de un computador:
- Memoria RAM física de direccionamiento lineal y tamaño variable.
- Segmentación de memoria mediante tabla de descriptores para hasta seis segmentos (PS, KS, CS, DS, ES y SS).
- Banco de registros de propósito general, punteros y registros de control de bus.
- Ciclo de instrucción estructurado en **Fetch**, **Decode**, **Update IP** y **Execute**.
- Registro de Códigos de Condición (`CC`) con banderas aritmético-lógicas `NZCV`.

---

## 2. Componentes de la Arquitectura

### 2.1 Memoria Principal (RAM)
- Tamaño variable de **M KiB** indicado con el parámetro opcional `m=M` (por defecto **16 KiB**).
- Se reserva dinámicamente (`calloc`) al iniciar y se libera al finalizar.
- `M` debe estar entre **1 y 64 KiB**: la base y el tamaño de cada descriptor, y la dirección física de `MAR`, son de 16 bits, por lo que no es posible direccionar más de 64 KiB.
- Rango de direcciones físicas válidas: `0x0000` hasta `M * 1024 - 1`.

### 2.2 Tabla de Descriptores de Segmentos
Consta de 8 entradas de 32 bits (16 bits de dirección base física y 16 bits de tamaño en bytes).
- Los segmentos se ubican de forma contigua en la RAM en el orden **PS, KS, CS, DS, ES, SS**.
- Solo se crean entradas para los segmentos de tamaño mayor a cero, en el mismo orden que su ubicación física (por ejemplo, si no existen el Param Segment ni el Const Segment, el Code Segment ocupa la entrada 0).
- El Code Segment es obligatorio, y en los ejecutables versión 2 también lo es el Stack Segment.
- En los ejecutables versión 1 solo existen CS y DS (el Data Segment ocupa el resto de la memoria).
- Si la suma de los segmentos supera la memoria principal, se informa **Memoria insuficiente** y no se ejecuta el programa.
- Las entradas no utilizadas quedan inactivas, con `0xFFFF` en base y tamaño.

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
| `7` | **SP** | Puntero al tope de la pila (inicia en `SS` + tamaño de la pila) |
| `8` | **BP** | Puntero base de la pila (no requiere inicialización) |
| `10..15` | **EAX..EFX** | Registros de propósito general para cómputo |
| `16` | **AC** | Acumulador / Resto en división entera |
| `17` | **CC** | Registro de Códigos de Condición |
| `26` | **CS** | Puntero al comienzo del Code Segment |
| `27` | **DS** | Puntero al comienzo del Data Segment |
| `28` | **ES** | Puntero al comienzo del Extra Segment |
| `29` | **SS** | Puntero al comienzo del Stack Segment |
| `30` | **KS** | Puntero al comienzo del Const Segment |
| `31` | **PS** | Puntero al comienzo del Param Segment |

Los punteros a segmento contienen `[16 bits entrada en la tabla | 16 bits offset 0]`. Si un segmento no existe, su registro queda cargado con `-1` (`0xFFFFFFFF`). Los índices 9 y 18 a 25 están reservados.

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
- `main.c`: Reserva y liberación de la memoria principal, lectura y validación de la cabecera `VMX26` (versiones 1 y 2), ubicación de los segmentos, inicialización de registros y ciclo de ejecución.

---

## 6. Requisitos, Compilación y Uso

### 6.1 Sistema Operativo y Entorno:
- **SO:** Windows 10.
- **Entorno:** Desarrollado y testeado en la terminal integrada de Visual Studio Code utilizando **MSYS2**.

### 6.2 Requisitos Previos:
- Entorno **MSYS2** instalado para proveer la colección de compiladores GNU.
- Compilador **GCC** (compatible con el estándar C99) instalado a través de MSYS2 y configurado correctamente en las variables de entorno (`PATH`) del sistema.

### 6.3 Instrucciones de Uso:

1. Traducción del código fuente (Assembler a Código Máquina):
Antes de utilizar la máquina virtual, se debe traducir el programa escrito en lenguaje Assembler (`.asm`) al formato binario ejecutable (`.vmx`) utilizando el traductor provisto por la cátedra. Desde la terminal, ejecutar:
```bash
.\vmt.exe programa.asm
```
2. Compilación de la Máquina Virtual:
Para generar el archivo ejecutable del emulador a partir de los códigos fuente en C, utilizar GCC con banderas estrictas:
```bash
gcc -Wall -Wextra -std=c99 main.c instrucciones.c dissasembler.c -o vmx.exe
```
3. Ejecución de la Máquina Virtual:
Para inicializar la máquina, cargar en memoria y correr el programa traducido, ejecutar el emulador pasándole el archivo binario como argumento:
```bash
./vmx.exe programa.vmx
```
4. Ejecución con Desensamblador (Disassembler):
Si se desea forzar a la máquina virtual a mostrar por consola la traza del código desensamblado (dirección física, volcado hexadecimal y mnemónicos con sus operandos correspondientes), se debe agregar el flag opcional -d al final del comando:
```bash
./vmx.exe programa.vmx -d
```
5. Tamaño de la memoria principal:
Para indicar el tamaño de la memoria en KiB (entre 1 y 64; por defecto 16), se agrega el parámetro opcional `m=M`:
```bash
./vmx.exe programa.vmx m=32
```
