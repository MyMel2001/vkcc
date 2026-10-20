# VulKan C Compiler (VKCC)

Vulkan-accelerated C/C++ compiler compatible with C99-latest and GCC/G++ syntaxes.

## Features

- **Vulkan Compute Acceleration**: Runs parallel source tokenization, delimiter balance checking, and syntax pre-validation directly on GPU via embedded SPIR-V compute shader.
- **Standards Support**: Compatible with C99, C11, C17, C23, and C++98 through C++26.
- **GCC / G++ Compatibility**: Drop-in compatible with standard flags (`-O*`, `-Wall`, `-std=*`, `-c`, `-o`, `-I`, `-L`, `-l`, `-shared`, `-fPIC`).
- **From-Scratch ELF Generator**: Native x86-64 machine code and ELF64 binary generator (`--scratch`) producing standalone executables with zero external compiler dependencies.
- **Zero Runtime Dependencies**: Pre-compiled SPIR-V compute bytecode embedded directly in the binary. Graceful CPU fallback when no Vulkan device is present.

## Requirements

- Linux x86-64
- Vulkan loader (`libvulkan.so.1`)
- C compiler (`cc`/`gcc`) to build the initial driver binary

## Building

Run `build.sh` to clean and compile the project into `build/`:

```sh
./build.sh
```

Or build with `make`:

```sh
make
make check
```

Executables created:
- `build/vkcc`: C compiler driver (Vulkan-accelerated)
- `build/vkc++`: C++ compiler driver (Vulkan-accelerated)
- `build/vkgcc`: GCC alias
- `build/vkg++`: G++ alias

## Usage

### Compile C

```sh
./build/vkcc -O2 main.c -o app
./app
```

### Compile C++

```sh
./build/vkc++ -std=c++20 -O2 main.cpp -o app
./app
```

### From-Scratch Native ELF Mode

Compiles source directly to native x86-64 ELF without calling external tools:

```sh
./build/vkcc --scratch main.c -o app
./app
```

### Vulkan Pipeline Inspection

Display GPU device info, dispatch metrics, and token throughput:

```sh
./build/vkcc -v main.c -o app
```

Run Vulkan syntax validation pass only:

```sh
./build/vkcc --vk-only main.c
```
