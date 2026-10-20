#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void run_cmd(const char *cmd) {
    int ret = system(cmd);
    assert(WIFEXITED(ret) && WEXITSTATUS(ret) == 0);
}

int main(void) {
    // 1. Version and Vulkan device discovery
    run_cmd("./vkcc --version > /dev/null");

    // 2. C99 Standard compliance
    FILE *f = fopen("/tmp/vkcc_t_c99.c", "w");
    assert(f);
    fprintf(f, "#include <assert.h>\n"
               "int main() {\n"
               "    int arr[] = { [2] = 42 };\n"
               "    int n = 3;\n"
               "    int vla[n];\n"
               "    vla[2] = arr[2];\n"
               "    assert(vla[2] == 42);\n"
               "    return 0;\n"
               "}\n");
    fclose(f);
    run_cmd("./vkcc -std=c99 -Wall -Wextra /tmp/vkcc_t_c99.c -o /tmp/vkcc_t_c99 && /tmp/vkcc_t_c99");

    // 3. C23 Standard compliance
    f = fopen("/tmp/vkcc_t_c23.c", "w");
    assert(f);
    fprintf(f, "#include <assert.h>\n"
               "int main() {\n"
               "    auto val = 100;\n"
               "    constexpr int k = 5;\n"
               "    static_assert(k == 5);\n"
               "    assert(val * k == 500);\n"
               "    return 0;\n"
               "}\n");
    fclose(f);
    run_cmd("./vkcc -std=c23 /tmp/vkcc_t_c23.c -o /tmp/vkcc_t_c23 && /tmp/vkcc_t_c23");

    // 4. C++20 Standard compliance
    f = fopen("/tmp/vkcc_t_cpp.cpp", "w");
    assert(f);
    fprintf(f, "#include <cassert>\n"
               "#include <concepts>\n"
               "template <std::integral T>\n"
               "T mul2(T x) { return x * 2; }\n"
               "int main() {\n"
               "    assert(mul2(21) == 42);\n"
               "    return 0;\n"
               "}\n");
    fclose(f);
    run_cmd("./vkc++ -std=c++20 /tmp/vkcc_t_cpp.cpp -o /tmp/vkcc_t_cpp && /tmp/vkcc_t_cpp");

    // 5. GCC syntax and extensions
    f = fopen("/tmp/vkcc_t_gnu.c", "w");
    assert(f);
    fprintf(f, "#include <assert.h>\n"
               "int main() {\n"
               "    int r = ({ int a = 10; int b = 20; a + b; });\n"
               "    assert(r == 30);\n"
               "    return 0;\n"
               "}\n");
    fclose(f);
    run_cmd("./vkgcc -std=gnu99 /tmp/vkcc_t_gnu.c -o /tmp/vkcc_t_gnu && /tmp/vkcc_t_gnu");

    // 6. From-scratch native ELF compilation (Zero external tools)
    f = fopen("/tmp/vkcc_t_scratch.c", "w");
    assert(f);
    fprintf(f, "int add(int x) { return x + 10; }\n"
               "int main() {\n"
               "    int val = 32;\n"
               "    if (val == 32) return add(val);\n"
               "    return 0;\n"
               "}\n");
    fclose(f);
    run_cmd("./vkcc --scratch /tmp/vkcc_t_scratch.c -o /tmp/vkcc_t_scratch");
    int scratch_ret = system("/tmp/vkcc_t_scratch");
    assert(WIFEXITED(scratch_ret) && WEXITSTATUS(scratch_ret) == 42);

    // 7. Vulkan-only syntax analysis
    run_cmd("./vkcc --vk-only /tmp/vkcc_t_c99.c > /dev/null");

    // Clean up temporary test files
    unlink("/tmp/vkcc_t_c99.c");
    unlink("/tmp/vkcc_t_c99");
    unlink("/tmp/vkcc_t_c23.c");
    unlink("/tmp/vkcc_t_c23");
    unlink("/tmp/vkcc_t_cpp.cpp");
    unlink("/tmp/vkcc_t_cpp");
    unlink("/tmp/vkcc_t_gnu.c");
    unlink("/tmp/vkcc_t_gnu");
    unlink("/tmp/vkcc_t_scratch.c");
    unlink("/tmp/vkcc_t_scratch");

    printf("ALL VKCC ASSERTION CHECKS PASSED.\n");
    return 0;
}
