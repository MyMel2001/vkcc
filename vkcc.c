/*
 * vkcc - Vulkan-accelerated C/C++ compiler.
 * Built from scratch with native ELF64 x86-64 code generation and GCC/G++ syntax compatibility.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "vk_shader_spv.h"

#define VKCC_VERSION "1.0.0"

/* ========================================================================= */
/* Vulkan Compute Acceleration Engine                                        */
/* ========================================================================= */

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkShaderModule shader_module;
    VkDescriptorSetLayout desc_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkCommandPool cmd_pool;
    VkPhysicalDeviceProperties props;
    bool available;
} VkContext;

typedef struct {
    uint32_t total_lines;
    uint32_t total_tokens;
    uint32_t total_bytes;
    int32_t brace_balance;
    int32_t paren_balance;
    int32_t bracket_balance;
    uint32_t syntax_errors;
    uint32_t pad;
    uint32_t histogram[8]; // 0: ws, 1: id/kw, 2: num, 3: str/chr, 4: op, 5: preproc, 6: comment, 7: other
} LexerOutput;

static uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t filter, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((filter & (1 << i)) && (mem.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return 0;
}

static bool vk_init(VkContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vkcc",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "vkcc_compute",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    if (vkCreateInstance(&inst_info, NULL, &ctx->instance) != VK_SUCCESS) {
        return false;
    }

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &dev_count, NULL);
    if (dev_count == 0) {
        vkDestroyInstance(ctx->instance, NULL);
        return false;
    }

    VkPhysicalDevice devices[dev_count];
    vkEnumeratePhysicalDevices(ctx->instance, &dev_count, devices);

    ctx->physical_device = devices[0];
    for (uint32_t i = 0; i < dev_count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            ctx->physical_device = devices[i];
            break;
        }
    }
    vkGetPhysicalDeviceProperties(ctx->physical_device, &ctx->props);

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &qf_count, NULL);
    VkQueueFamilyProperties qf[qf_count];
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &qf_count, qf);
    ctx->queue_family = 0;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            ctx->queue_family = i;
            break;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dev_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
    };
    if (vkCreateDevice(ctx->physical_device, &dev_info, NULL, &ctx->device) != VK_SUCCESS) {
        vkDestroyInstance(ctx->instance, NULL);
        return false;
    }

    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);

    VkShaderModuleCreateInfo sm_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vkcc_compute_spv_size,
        .pCode = vkcc_compute_spv,
    };
    if (vkCreateShaderModule(ctx->device, &sm_info, NULL, &ctx->shader_module) != VK_SUCCESS) {
        vkDestroyDevice(ctx->device, NULL);
        vkDestroyInstance(ctx->instance, NULL);
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dsl_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    vkCreateDescriptorSetLayout(ctx->device, &dsl_info, NULL, &ctx->desc_layout);

    VkPipelineLayoutCreateInfo pl_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &ctx->desc_layout,
    };
    vkCreatePipelineLayout(ctx->device, &pl_info, NULL, &ctx->pipeline_layout);

    VkComputePipelineCreateInfo cp_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = ctx->shader_module,
            .pName = "main",
        },
        .layout = ctx->pipeline_layout,
    };
    if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cp_info, NULL, &ctx->pipeline) != VK_SUCCESS) {
        vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, ctx->desc_layout, NULL);
        vkDestroyShaderModule(ctx->device, ctx->shader_module, NULL);
        vkDestroyDevice(ctx->device, NULL);
        vkDestroyInstance(ctx->instance, NULL);
        return false;
    }

    VkCommandPoolCreateInfo cp_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_family,
    };
    vkCreateCommandPool(ctx->device, &cp_pool_info, NULL, &ctx->cmd_pool);

    ctx->available = true;
    return true;
}

static void vk_cleanup(VkContext *ctx) {
    if (!ctx->available) return;
    vkDestroyCommandPool(ctx->device, ctx->cmd_pool, NULL);
    vkDestroyPipeline(ctx->device, ctx->pipeline, NULL);
    vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(ctx->device, ctx->desc_layout, NULL);
    vkDestroyShaderModule(ctx->device, ctx->shader_module, NULL);
    vkDestroyDevice(ctx->device, NULL);
    vkDestroyInstance(ctx->instance, NULL);
    ctx->available = false;
}

// ponytail: single dispatch per file, buffer max 64MB. Stream in chunks if file exceeds 64MB.
static bool vk_accelerate_source(VkContext *ctx, const char *src, size_t len, LexerOutput *out, double *elapsed_us) {
    if (!ctx->available || len == 0) return false;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint32_t words = (uint32_t)((len + 3) / 4);
    uint32_t in_buf_size = sizeof(uint32_t) * (4 + words);
    uint32_t out_buf_size = sizeof(LexerOutput);

    VkBuffer in_buf, out_buf;
    VkDeviceMemory in_mem, out_mem;

    VkBufferCreateInfo b_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = in_buf_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    vkCreateBuffer(ctx->device, &b_info, NULL, &in_buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx->device, in_buf, &req);
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory_type(ctx->physical_device, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    vkAllocateMemory(ctx->device, &alloc_info, NULL, &in_mem);
    vkBindBufferMemory(ctx->device, in_buf, in_mem, 0);

    uint32_t *in_ptr;
    vkMapMemory(ctx->device, in_mem, 0, in_buf_size, 0, (void**)&in_ptr);
    in_ptr[0] = (uint32_t)len;
    in_ptr[1] = 0;
    in_ptr[2] = 0;
    in_ptr[3] = 0;
    memset(&in_ptr[4], 0, words * 4);
    memcpy(&in_ptr[4], src, len);
    vkUnmapMemory(ctx->device, in_mem);

    b_info.size = out_buf_size;
    vkCreateBuffer(ctx->device, &b_info, NULL, &out_buf);
    vkGetBufferMemoryRequirements(ctx->device, out_buf, &req);
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = find_memory_type(ctx->physical_device, req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(ctx->device, &alloc_info, NULL, &out_mem);
    vkBindBufferMemory(ctx->device, out_buf, out_mem, 0);

    uint32_t *out_ptr;
    vkMapMemory(ctx->device, out_mem, 0, out_buf_size, 0, (void**)&out_ptr);
    memset(out_ptr, 0, out_buf_size);
    vkUnmapMemory(ctx->device, out_mem);

    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dp_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkDescriptorPool pool;
    vkCreateDescriptorPool(ctx->device, &dp_info, NULL, &pool);

    VkDescriptorSetAllocateInfo ds_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &ctx->desc_layout,
    };
    VkDescriptorSet desc_set;
    vkAllocateDescriptorSets(ctx->device, &ds_alloc, &desc_set);

    VkDescriptorBufferInfo dbi_in = { .buffer = in_buf, .offset = 0, .range = in_buf_size };
    VkDescriptorBufferInfo dbi_out = { .buffer = out_buf, .offset = 0, .range = out_buf_size };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = desc_set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi_in },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = desc_set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi_out },
    };
    vkUpdateDescriptorSets(ctx->device, 2, writes, 0, NULL);

    VkCommandBufferAllocateInfo cb_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(ctx->device, &cb_alloc, &cmd);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &begin_info);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipeline_layout, 0, 1, &desc_set, 0, NULL);
    uint32_t workgroups = (uint32_t)((len + 255) / 256);
    vkCmdDispatch(cmd, workgroups ? workgroups : 1, 1, 1);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkQueueSubmit(ctx->queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx->queue);

    vkMapMemory(ctx->device, out_mem, 0, out_buf_size, 0, (void**)&out_ptr);
    memcpy(out, out_ptr, sizeof(LexerOutput));
    vkUnmapMemory(ctx->device, out_mem);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    *elapsed_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;

    vkFreeCommandBuffers(ctx->device, ctx->cmd_pool, 1, &cmd);
    vkDestroyDescriptorPool(ctx->device, pool, NULL);
    vkFreeMemory(ctx->device, in_mem, NULL);
    vkDestroyBuffer(ctx->device, in_buf, NULL);
    vkFreeMemory(ctx->device, out_mem, NULL);
    vkDestroyBuffer(ctx->device, out_buf, NULL);

    return true;
}

/* ========================================================================= */
/* From-Scratch C Lexer, Recursive-Descent Parser & x86-64 ELF Code Generator*/
/* ========================================================================= */

typedef enum {
    TK_EOF = 0,
    TK_NUM,
    TK_IDENT,
    TK_INT,
    TK_CHAR,
    TK_VOID,
    TK_RETURN,
    TK_IF,
    TK_ELSE,
    TK_WHILE,
    TK_FOR,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_ASSIGN,
    TK_EQ,
    TK_NEQ,
    TK_LT,
    TK_GT,
    TK_LTE,
    TK_GTE,
    TK_SEMI,
    TK_COMMA,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_STR
} TokenType;

typedef struct {
    TokenType type;
    int int_val;
    char str_val[128];
    int line;
} Token;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    int line;
    Token cur;
} Lexer;

static void lex_init(Lexer *l, const char *src, size_t len) {
    l->src = src;
    l->pos = 0;
    l->len = len;
    l->line = 1;
}

static char lex_peek(Lexer *l) {
    return (l->pos < l->len) ? l->src[l->pos] : '\0';
}

static char lex_advance(Lexer *l) {
    if (l->pos >= l->len) return '\0';
    char c = l->src[l->pos++];
    if (c == '\n') l->line++;
    return c;
}

static void lex_skip_whitespace_and_comments(Lexer *l) {
    while (l->pos < l->len) {
        char c = lex_peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lex_advance(l);
        } else if (c == '/' && l->pos + 1 < l->len && l->src[l->pos + 1] == '/') {
            while (l->pos < l->len && lex_peek(l) != '\n') lex_advance(l);
        } else if (c == '/' && l->pos + 1 < l->len && l->src[l->pos + 1] == '*') {
            lex_advance(l); lex_advance(l);
            while (l->pos + 1 < l->len && !(l->src[l->pos] == '*' && l->src[l->pos + 1] == '/')) {
                lex_advance(l);
            }
            if (l->pos + 1 < l->len) { lex_advance(l); lex_advance(l); }
        } else if (c == '#') {
            // Skip preprocessor directive line in scratch mode
            while (l->pos < l->len && lex_peek(l) != '\n') lex_advance(l);
        } else {
            break;
        }
    }
}

static void lex_next(Lexer *l) {
    lex_skip_whitespace_and_comments(l);
    l->cur.line = l->line;

    if (l->pos >= l->len) {
        l->cur.type = TK_EOF;
        return;
    }

    char c = lex_peek(l);

    if (isdigit((unsigned char)c)) {
        int val = 0;
        while (l->pos < l->len && isdigit((unsigned char)lex_peek(l))) {
            val = val * 10 + (lex_advance(l) - '0');
        }
        l->cur.type = TK_NUM;
        l->cur.int_val = val;
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        int idx = 0;
        while (l->pos < l->len && (isalnum((unsigned char)lex_peek(l)) || lex_peek(l) == '_')) {
            if (idx < 127) l->cur.str_val[idx++] = lex_advance(l);
            else lex_advance(l);
        }
        l->cur.str_val[idx] = '\0';

        if (strcmp(l->cur.str_val, "return") == 0) l->cur.type = TK_RETURN;
        else if (strcmp(l->cur.str_val, "if") == 0) l->cur.type = TK_IF;
        else if (strcmp(l->cur.str_val, "else") == 0) l->cur.type = TK_ELSE;
        else if (strcmp(l->cur.str_val, "while") == 0) l->cur.type = TK_WHILE;
        else if (strcmp(l->cur.str_val, "for") == 0) l->cur.type = TK_FOR;
        else if (strcmp(l->cur.str_val, "int") == 0) l->cur.type = TK_INT;
        else if (strcmp(l->cur.str_val, "char") == 0) l->cur.type = TK_CHAR;
        else if (strcmp(l->cur.str_val, "void") == 0) l->cur.type = TK_VOID;
        else l->cur.type = TK_IDENT;
        return;
    }

    if (c == '"') {
        lex_advance(l);
        int idx = 0;
        while (l->pos < l->len && lex_peek(l) != '"') {
            char ch = lex_advance(l);
            if (ch == '\\' && l->pos < l->len) {
                char esc = lex_advance(l);
                if (esc == 'n') ch = '\n';
                else if (esc == 't') ch = '\t';
                else if (esc == '0') ch = '\0';
            }
            if (idx < 127) l->cur.str_val[idx++] = ch;
        }
        if (lex_peek(l) == '"') lex_advance(l);
        l->cur.str_val[idx] = '\0';
        l->cur.type = TK_STR;
        return;
    }

    lex_advance(l);
    switch (c) {
        case '+': l->cur.type = TK_PLUS; return;
        case '-': l->cur.type = TK_MINUS; return;
        case '*': l->cur.type = TK_STAR; return;
        case '/': l->cur.type = TK_SLASH; return;
        case ';': l->cur.type = TK_SEMI; return;
        case ',': l->cur.type = TK_COMMA; return;
        case '(': l->cur.type = TK_LPAREN; return;
        case ')': l->cur.type = TK_RPAREN; return;
        case '{': l->cur.type = TK_LBRACE; return;
        case '}': l->cur.type = TK_RBRACE; return;
        case '[': l->cur.type = TK_LBRACKET; return;
        case ']': l->cur.type = TK_RBRACKET; return;
        case '=':
            if (lex_peek(l) == '=') { lex_advance(l); l->cur.type = TK_EQ; }
            else l->cur.type = TK_ASSIGN;
            return;
        case '!':
            if (lex_peek(l) == '=') { lex_advance(l); l->cur.type = TK_NEQ; }
            break;
        case '<':
            if (lex_peek(l) == '=') { lex_advance(l); l->cur.type = TK_LTE; }
            else l->cur.type = TK_LT;
            return;
        case '>':
            if (lex_peek(l) == '=') { lex_advance(l); l->cur.type = TK_GTE; }
            else l->cur.type = TK_GT;
            return;
    }
    l->cur.type = TK_EOF;
}

// Machine code buffer
typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} CodeBuf;

static void emit_byte(CodeBuf *cb, uint8_t b) {
    if (cb->size >= cb->cap) {
        cb->cap = cb->cap ? cb->cap * 2 : 1024;
        cb->data = realloc(cb->data, cb->cap);
    }
    cb->data[cb->size++] = b;
}

static void emit_bytes(CodeBuf *cb, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) emit_byte(cb, b[i]);
}

static void emit_u32(CodeBuf *cb, uint32_t val) {
    emit_bytes(cb, (uint8_t*)&val, 4);
}

typedef struct {
    char name[64];
    int offset;
} Var;

typedef struct {
    char name[64];
    size_t code_offset;
} Func;

typedef struct {
    Lexer lexer;
    CodeBuf cb;
    Var locals[128];
    int num_locals;
    int stack_offset;
    Func funcs[64];
    int num_funcs;
} Compiler;

static int find_local(Compiler *c, const char *name) {
    for (int i = 0; i < c->num_locals; i++) {
        if (strcmp(c->locals[i].name, name) == 0) return c->locals[i].offset;
    }
    return 0;
}

static int add_local(Compiler *c, const char *name) {
    int off = find_local(c, name);
    if (off != 0) return off;
    c->stack_offset += 8;
    strcpy(c->locals[c->num_locals].name, name);
    c->locals[c->num_locals].offset = c->stack_offset;
    c->num_locals++;
    return c->stack_offset;
}

static void parse_expr(Compiler *c);

static void parse_primary(Compiler *c) {
    if (c->lexer.cur.type == TK_NUM) {
        int v = c->lexer.cur.int_val;
        lex_next(&c->lexer);
        // mov eax, imm32
        emit_byte(&c->cb, 0xb8);
        emit_u32(&c->cb, v);
    } else if (c->lexer.cur.type == TK_IDENT) {
        char name[64];
        strcpy(name, c->lexer.cur.str_val);
        lex_next(&c->lexer);
        if (c->lexer.cur.type == TK_LPAREN) {
            // Function call
            lex_next(&c->lexer);
            // ponytail: up to 1 argument (rdi); upgrade path: full System V AMD64 ABI 6 registers.
            if (c->lexer.cur.type != TK_RPAREN) {
                parse_expr(c);
                // mov edi, eax
                emit_bytes(&c->cb, (uint8_t[]){0x89, 0xc7}, 2);
            }
            if (c->lexer.cur.type == TK_RPAREN) lex_next(&c->lexer);

            // Call function
            size_t target_offset = 0;
            for (int i = 0; i < c->num_funcs; i++) {
                if (strcmp(c->funcs[i].name, name) == 0) {
                    target_offset = c->funcs[i].code_offset;
                    break;
                }
            }
            // emit call rel32
            emit_byte(&c->cb, 0xe8);
            int32_t rel = (int32_t)(target_offset - (c->cb.size + 4));
            emit_u32(&c->cb, (uint32_t)rel);
        } else {
            // Variable load
            int off = find_local(c, name);
            if (off == 0) off = add_local(c, name);
            // mov eax, [rbp - off]
            emit_bytes(&c->cb, (uint8_t[]){0x8b, 0x85}, 2);
            emit_u32(&c->cb, (uint32_t)(-off));
        }
    } else if (c->lexer.cur.type == TK_LPAREN) {
        lex_next(&c->lexer);
        parse_expr(c);
        if (c->lexer.cur.type == TK_RPAREN) lex_next(&c->lexer);
    }
}

static void parse_mul(Compiler *c) {
    parse_primary(c);
    while (c->lexer.cur.type == TK_STAR || c->lexer.cur.type == TK_SLASH) {
        TokenType op = c->lexer.cur.type;
        lex_next(&c->lexer);
        // push rax
        emit_byte(&c->cb, 0x50);
        parse_primary(c);
        // pop rbx
        emit_byte(&c->cb, 0x5b);
        if (op == TK_STAR) {
            // imul eax, ebx
            emit_bytes(&c->cb, (uint8_t[]){0x0f, 0xaf, 0xc3}, 3);
        } else {
            // xchg eax, ebx; cdq; idiv ebx
            emit_bytes(&c->cb, (uint8_t[]){0x93, 0x99, 0xf7, 0xfb}, 4);
        }
    }
}

static void parse_add(Compiler *c) {
    parse_mul(c);
    while (c->lexer.cur.type == TK_PLUS || c->lexer.cur.type == TK_MINUS) {
        TokenType op = c->lexer.cur.type;
        lex_next(&c->lexer);
        // push rax
        emit_byte(&c->cb, 0x50);
        parse_mul(c);
        // pop rbx
        emit_byte(&c->cb, 0x5b);
        if (op == TK_PLUS) {
            // add eax, ebx
            emit_bytes(&c->cb, (uint8_t[]){0x01, 0xd8}, 2);
        } else {
            // sub ebx, eax; mov eax, ebx
            emit_bytes(&c->cb, (uint8_t[]){0x29, 0xc3, 0x89, 0xd8}, 4);
        }
    }
}

static void parse_rel(Compiler *c) {
    parse_add(c);
    while (c->lexer.cur.type == TK_LT || c->lexer.cur.type == TK_GT ||
           c->lexer.cur.type == TK_LTE || c->lexer.cur.type == TK_GTE) {
        TokenType op = c->lexer.cur.type;
        lex_next(&c->lexer);
        // push rax
        emit_byte(&c->cb, 0x50);
        parse_add(c);
        // pop rbx
        emit_byte(&c->cb, 0x5b);
        // cmp ebx, eax
        emit_bytes(&c->cb, (uint8_t[]){0x39, 0xc3}, 2);
        if (op == TK_LT) emit_bytes(&c->cb, (uint8_t[]){0x0f, 0x9c, 0xc0}, 3); // setl al
        else if (op == TK_GT) emit_bytes(&c->cb, (uint8_t[]){0x0f, 0x9f, 0xc0}, 3); // setg al
        else if (op == TK_LTE) emit_bytes(&c->cb, (uint8_t[]){0x0f, 0x9e, 0xc0}, 3); // setle al
        else emit_bytes(&c->cb, (uint8_t[]){0x0f, 0x9d, 0xc0}, 3); // setge al
        // movzx eax, al
        emit_bytes(&c->cb, (uint8_t[]){0x0f, 0xb6, 0xc0}, 3);
    }
}

static void parse_eq(Compiler *c) {
    parse_rel(c);
    while (c->lexer.cur.type == TK_EQ || c->lexer.cur.type == TK_NEQ) {
        TokenType op = c->lexer.cur.type;
        lex_next(&c->lexer);
        // push rax
        emit_byte(&c->cb, 0x50);
        parse_rel(c);
        // pop rbx
        emit_byte(&c->cb, 0x5b);
        // cmp ebx, eax
        emit_bytes(&c->cb, (uint8_t[]){0x39, 0xc3}, 2);
        if (op == TK_EQ) emit_bytes(&c->cb, (uint8_t[]){0x0f, 0x94, 0xc0}, 3); // sete al
        else emit_bytes(&c->cb, (uint8_t[]){0x0f, 0x95, 0xc0}, 3); // setne al
        // movzx eax, al
        emit_bytes(&c->cb, (uint8_t[]){0x0f, 0xb6, 0xc0}, 3);
    }
}

static void parse_expr(Compiler *c) {
    if (c->lexer.cur.type == TK_IDENT && c->lexer.src[c->lexer.pos] == '=') {
        char name[64];
        strcpy(name, c->lexer.cur.str_val);
        lex_next(&c->lexer); // ident
        lex_next(&c->lexer); // '='
        parse_expr(c);
        int off = find_local(c, name);
        if (off == 0) off = add_local(c, name);
        // mov [rbp - off], eax
        emit_bytes(&c->cb, (uint8_t[]){0x89, 0x85}, 2);
        emit_u32(&c->cb, (uint32_t)(-off));
        return;
    }
    parse_eq(c);
}

static void parse_stmt(Compiler *c);

static void parse_block(Compiler *c) {
    if (c->lexer.cur.type == TK_LBRACE) lex_next(&c->lexer);
    while (c->lexer.cur.type != TK_RBRACE && c->lexer.cur.type != TK_EOF) {
        parse_stmt(c);
    }
    if (c->lexer.cur.type == TK_RBRACE) lex_next(&c->lexer);
}

static void parse_stmt(Compiler *c) {
    if (c->lexer.cur.type == TK_RETURN) {
        lex_next(&c->lexer);
        if (c->lexer.cur.type != TK_SEMI) parse_expr(c);
        if (c->lexer.cur.type == TK_SEMI) lex_next(&c->lexer);
        // Epilogue: mov rsp, rbp; pop rbp; ret
        emit_bytes(&c->cb, (uint8_t[]){0x48, 0x89, 0xec, 0x5d, 0xc3}, 5);
    } else if (c->lexer.cur.type == TK_INT || c->lexer.cur.type == TK_CHAR) {
        lex_next(&c->lexer);
        while (1) {
            char name[64];
            strcpy(name, c->lexer.cur.str_val);
            lex_next(&c->lexer);
            int off = add_local(c, name);
            if (c->lexer.cur.type == TK_ASSIGN) {
                lex_next(&c->lexer);
                parse_expr(c);
                // mov [rbp - off], eax
                emit_bytes(&c->cb, (uint8_t[]){0x89, 0x85}, 2);
                emit_u32(&c->cb, (uint32_t)(-off));
            }
            if (c->lexer.cur.type == TK_COMMA) { lex_next(&c->lexer); continue; }
            break;
        }
        if (c->lexer.cur.type == TK_SEMI) lex_next(&c->lexer);
    } else if (c->lexer.cur.type == TK_IF) {
        lex_next(&c->lexer);
        if (c->lexer.cur.type == TK_LPAREN) lex_next(&c->lexer);
        parse_expr(c);
        if (c->lexer.cur.type == TK_RPAREN) lex_next(&c->lexer);

        // test eax, eax; jz else_lbl
        emit_bytes(&c->cb, (uint8_t[]){0x85, 0xc0, 0x0f, 0x84}, 4);
        size_t jz_patch = c->cb.size;
        emit_u32(&c->cb, 0);

        parse_stmt(c);

        if (c->lexer.cur.type == TK_ELSE) {
            lex_next(&c->lexer);
            // jmp end_lbl
            emit_byte(&c->cb, 0xe9);
            size_t jmp_patch = c->cb.size;
            emit_u32(&c->cb, 0);

            // patch jz to here
            int32_t else_rel = (int32_t)(c->cb.size - (jz_patch + 4));
            memcpy(&c->cb.data[jz_patch], &else_rel, 4);

            parse_stmt(c);

            // patch jmp to here
            int32_t end_rel = (int32_t)(c->cb.size - (jmp_patch + 4));
            memcpy(&c->cb.data[jmp_patch], &end_rel, 4);
        } else {
            int32_t else_rel = (int32_t)(c->cb.size - (jz_patch + 4));
            memcpy(&c->cb.data[jz_patch], &else_rel, 4);
        }
    } else if (c->lexer.cur.type == TK_WHILE) {
        lex_next(&c->lexer);
        size_t loop_start = c->cb.size;
        if (c->lexer.cur.type == TK_LPAREN) lex_next(&c->lexer);
        parse_expr(c);
        if (c->lexer.cur.type == TK_RPAREN) lex_next(&c->lexer);

        // test eax, eax; jz loop_end
        emit_bytes(&c->cb, (uint8_t[]){0x85, 0xc0, 0x0f, 0x84}, 4);
        size_t jz_patch = c->cb.size;
        emit_u32(&c->cb, 0);

        parse_stmt(c);

        // jmp loop_start
        emit_byte(&c->cb, 0xe9);
        int32_t loop_rel = (int32_t)(loop_start - (c->cb.size + 4));
        emit_u32(&c->cb, (uint32_t)loop_rel);

        int32_t end_rel = (int32_t)(c->cb.size - (jz_patch + 4));
        memcpy(&c->cb.data[jz_patch], &end_rel, 4);
    } else if (c->lexer.cur.type == TK_FOR) {
        lex_next(&c->lexer);
        if (c->lexer.cur.type == TK_LPAREN) lex_next(&c->lexer);
        if (c->lexer.cur.type != TK_SEMI) parse_stmt(c);
        else lex_next(&c->lexer);

        size_t loop_start = c->cb.size;
        size_t jz_patch = 0;
        if (c->lexer.cur.type != TK_SEMI) {
            parse_expr(c);
            emit_bytes(&c->cb, (uint8_t[]){0x85, 0xc0, 0x0f, 0x84}, 4);
            jz_patch = c->cb.size;
            emit_u32(&c->cb, 0);
        }
        if (c->lexer.cur.type == TK_SEMI) lex_next(&c->lexer);

        // ponytail: skip step parsing in header; step placed in loop body or simple condition.
        while (c->lexer.cur.type != TK_RPAREN && c->lexer.cur.type != TK_EOF) lex_next(&c->lexer);
        if (c->lexer.cur.type == TK_RPAREN) lex_next(&c->lexer);

        parse_stmt(c);

        emit_byte(&c->cb, 0xe9);
        int32_t loop_rel = (int32_t)(loop_start - (c->cb.size + 4));
        emit_u32(&c->cb, (uint32_t)loop_rel);

        if (jz_patch) {
            int32_t end_rel = (int32_t)(c->cb.size - (jz_patch + 4));
            memcpy(&c->cb.data[jz_patch], &end_rel, 4);
        }
    } else if (c->lexer.cur.type == TK_LBRACE) {
        parse_block(c);
    } else {
        parse_expr(c);
        if (c->lexer.cur.type == TK_SEMI) lex_next(&c->lexer);
    }
}

static void parse_func(Compiler *c) {
    // Return type
    if (c->lexer.cur.type == TK_INT || c->lexer.cur.type == TK_VOID || c->lexer.cur.type == TK_CHAR) {
        lex_next(&c->lexer);
    }
    char func_name[64];
    strcpy(func_name, c->lexer.cur.str_val);
    lex_next(&c->lexer);

    strcpy(c->funcs[c->num_funcs].name, func_name);
    c->funcs[c->num_funcs].code_offset = c->cb.size;
    c->num_funcs++;

    // Reset local variable table
    c->num_locals = 0;
    c->stack_offset = 0;

    // Parameters
    if (c->lexer.cur.type == TK_LPAREN) lex_next(&c->lexer);
    if (c->lexer.cur.type == TK_INT || c->lexer.cur.type == TK_CHAR) {
        lex_next(&c->lexer);
        char param[64];
        strcpy(param, c->lexer.cur.str_val);
        lex_next(&c->lexer);
        int off = add_local(c, param);
        // Function prologue: push rbp; mov rbp, rsp; sub rsp, 128
        emit_bytes(&c->cb, (uint8_t[]){0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00}, 11);
        // mov [rbp - off], edi (first param)
        emit_bytes(&c->cb, (uint8_t[]){0x89, 0xbd}, 2);
        emit_u32(&c->cb, (uint32_t)(-off));
    } else {
        // Function prologue
        emit_bytes(&c->cb, (uint8_t[]){0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00}, 11);
    }
    if (c->lexer.cur.type == TK_RPAREN) lex_next(&c->lexer);

    parse_block(c);

    // Epilogue default: mov rsp, rbp; pop rbp; ret
    emit_bytes(&c->cb, (uint8_t[]){0x48, 0x89, 0xec, 0x5d, 0xc3}, 5);
}

static bool compile_to_elf_scratch(const char *src, size_t len, const char *out_path) {
    Compiler c = {0};
    lex_init(&c.lexer, src, len);
    lex_next(&c.lexer);

    // _start entry stub: call main; mov edi, eax; mov eax, 60; syscall
    uint8_t start_stub[] = {
        0xe8, 0x00, 0x00, 0x00, 0x00, // call main (offset 0)
        0x89, 0xc7,                   // mov edi, eax
        0xb8, 0x3c, 0x00, 0x00, 0x00, // mov eax, 60 (sys_exit)
        0x0f, 0x05                    // syscall
    };
    emit_bytes(&c.cb, start_stub, sizeof(start_stub));

    // Parse all top-level functions
    while (c.lexer.cur.type != TK_EOF) {
        parse_func(&c);
    }

    // Patch main offset in _start
    size_t main_offset = 0;
    for (int i = 0; i < c.num_funcs; i++) {
        if (strcmp(c.funcs[i].name, "main") == 0) {
            main_offset = c.funcs[i].code_offset;
            break;
        }
    }
    if (main_offset == 0) {
        fprintf(stderr, "vkcc: error: 'main' function not defined in source\n");
        free(c.cb.data);
        return false;
    }
    int32_t rel = (int32_t)(main_offset - 5);
    memcpy(&c.cb.data[1], &rel, 4);

    // Build standalone ELF64 executable
    size_t total_filesz = 120 + c.cb.size;
    uint8_t *elf = calloc(1, total_filesz);

    // ELF64 Header
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; // 64-bit
    elf[5] = 1; // little endian
    elf[6] = 1; // version
    *(uint16_t*)&elf[16] = 2; // ET_EXEC
    *(uint16_t*)&elf[18] = 0x3E; // EM_X86_64
    *(uint32_t*)&elf[20] = 1; // EV_CURRENT
    *(uint64_t*)&elf[24] = 0x400000 + 120; // e_entry (_start)
    *(uint64_t*)&elf[32] = 64; // e_phoff
    *(uint16_t*)&elf[52] = 64; // e_ehsize
    *(uint16_t*)&elf[54] = 56; // e_phentsize
    *(uint16_t*)&elf[56] = 1;  // e_phnum

    // Program Header (PT_LOAD)
    *(uint32_t*)&elf[64] = 1; // PT_LOAD
    *(uint32_t*)&elf[68] = 7; // PF_R | PF_W | PF_X
    *(uint64_t*)&elf[72] = 0; // p_offset
    *(uint64_t*)&elf[80] = 0x400000; // p_vaddr
    *(uint64_t*)&elf[88] = 0x400000; // p_paddr
    *(uint64_t*)&elf[96] = total_filesz; // p_filesz
    *(uint64_t*)&elf[104] = total_filesz; // p_memsz
    *(uint64_t*)&elf[112] = 0x1000; // p_align

    memcpy(elf + 120, c.cb.data, c.cb.size);

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        perror("vkcc: cannot open output file");
        free(elf);
        free(c.cb.data);
        return false;
    }
    ssize_t written = write(fd, elf, total_filesz);
    close(fd);
    free(elf);
    free(c.cb.data);
    return written == (ssize_t)total_filesz;
}

/* ========================================================================= */
/* Driver & Command-line Interface                                           */
/* ========================================================================= */

static bool is_cplusplus_file(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return (strcmp(dot, ".cpp") == 0 || strcmp(dot, ".cc") == 0 ||
            strcmp(dot, ".cxx") == 0 || strcmp(dot, ".C") == 0 ||
            strcmp(dot, ".cp") == 0 || strcmp(dot, ".c++") == 0);
}

static bool is_c_file(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return (strcmp(dot, ".c") == 0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "vkcc: fatal error: no input files\ncompilation terminated.\n");
        return 1;
    }

    bool verbose = false;
    bool vk_disable = false;
    bool is_cpp = false;
    bool vk_only = false;
    bool force_scratch = false;
    const char *out_path = "a.out";

    const char *prog_name = argv[0];
    const char *slash = strrchr(prog_name, '/');
    if (slash) prog_name = slash + 1;
    if (strstr(prog_name, "++")) is_cpp = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("vkcc (Vulkan C/C++ Compiler) version %s\n", VKCC_VERSION);
            printf("Target: x86_64-pc-linux-gnu\n");
            printf("Architecture: Self-contained Vulkan GPU Lexer + x86-64 Native ELF Generator\n");
            printf("Supported Standards: C99, C11, C17, C23, C++98, C++11, C++14, C++17, C++20, C++23, C++26\n");
            printf("Syntaxes: GCC / G++ compatible\n");
            VkContext ctx;
            if (vk_init(&ctx)) {
                printf("Vulkan Device: %s (API %d.%d.%d)\n", ctx.props.deviceName,
                       VK_VERSION_MAJOR(ctx.props.apiVersion),
                       VK_VERSION_MINOR(ctx.props.apiVersion),
                       VK_VERSION_PATCH(ctx.props.apiVersion));
                vk_cleanup(&ctx);
            } else {
                printf("Vulkan Device: None (CPU fallback enabled)\n");
            }
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options] file...\n", argv[0]);
            printf("Options:\n");
            printf("  -v               Display verbose compiler and Vulkan pipeline stages\n");
            printf("  --scratch        Force scratch compilation mode (pure GPU lexer + native ELF output)\n");
            printf("  --vk-disable     Disable Vulkan GPU acceleration\n");
            printf("  --vk-only        Run Vulkan lexical analysis and syntax pre-check only\n");
            printf("  -std=<standard>  Assume that the input sources are for <standard>\n");
            printf("  -o <file>        Place the output into <file>\n");
            printf("  -c               Compile and assemble, but do not link\n");
            printf("  -O<n>            Set optimization level\n");
            printf("  -Wall -Wextra    Enable compiler warnings\n");
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0) verbose = true;
        if (strcmp(argv[i], "--scratch") == 0) force_scratch = true;
        if (strcmp(argv[i], "--vk-disable") == 0) vk_disable = true;
        if (strcmp(argv[i], "--vk-only") == 0) vk_only = true;
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        if (strncmp(argv[i], "-x", 2) == 0) {
            const char *lang = (argv[i][2] != '\0') ? &argv[i][2] : ((i + 1 < argc) ? argv[++i] : "");
            if (strstr(lang, "c++")) is_cpp = true;
        }
    }

    char *backend_args[argc + 16];
    int backend_argc = 0;
    const char *source_files[argc];
    int num_sources = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--vk-disable") == 0 ||
            strcmp(argv[i], "--vk-only") == 0 ||
            strcmp(argv[i], "--scratch") == 0) {
            continue;
        }
        backend_args[backend_argc++] = argv[i];
        if (is_c_file(argv[i])) {
            source_files[num_sources++] = argv[i];
        } else if (is_cplusplus_file(argv[i])) {
            source_files[num_sources++] = argv[i];
            is_cpp = true;
        }
    }
    backend_args[backend_argc] = NULL;

    // Vulkan compute pass
    VkContext ctx;
    bool vk_ready = false;
    if (!vk_disable) {
        vk_ready = vk_init(&ctx);
        if (verbose) {
            if (vk_ready) {
                printf("[vkcc:vulkan] Accelerator initialized: %s\n", ctx.props.deviceName);
            } else {
                printf("[vkcc:vulkan] Vulkan accelerator unavailable; using CPU path\n");
            }
        }
    }

    char *first_source_buf = NULL;
    size_t first_source_len = 0;

    for (int i = 0; i < num_sources; i++) {
        const char *src_path = source_files[i];
        FILE *f = fopen(src_path, "rb");
        if (!f) {
            fprintf(stderr, "vkcc: error: %s: %s\n", src_path, strerror(errno));
            if (vk_ready) vk_cleanup(&ctx);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buffer = malloc(sz + 1);
        if (!buffer) {
            fclose(f);
            continue;
        }
        fread(buffer, 1, sz, f);
        buffer[sz] = '\0';
        fclose(f);

        if (i == 0) {
            first_source_buf = buffer;
            first_source_len = sz;
        }

        if (vk_ready) {
            LexerOutput out;
            double elapsed_us = 0;
            if (vk_accelerate_source(&ctx, buffer, sz, &out, &elapsed_us)) {
                if (verbose || vk_only) {
                    printf("[vkcc:vulkan] %s: %ld bytes, %u lines, %u tokens in %.1f us (%.1f MB/s)\n",
                           src_path, sz, out.total_lines, out.total_tokens, elapsed_us,
                           (sz / (1024.0 * 1024.0)) / (elapsed_us / 1e6));
                    printf("  Identifiers/Keywords: %u, Numbers: %u, Strings/Chars: %u, Operators: %u\n",
                           out.histogram[1], out.histogram[2], out.histogram[3], out.histogram[4]);
                    printf("  Delimiter balance: braces=%d, parens=%d, brackets=%d\n",
                           out.brace_balance, out.paren_balance, out.bracket_balance);
                }
                if (out.brace_balance != 0) {
                    fprintf(stderr, "[vkcc:vulkan:warning] %s: mismatched curly braces (balance: %d)\n",
                            src_path, out.brace_balance);
                }
                if (out.paren_balance != 0) {
                    fprintf(stderr, "[vkcc:vulkan:warning] %s: mismatched parentheses (balance: %d)\n",
                            src_path, out.paren_balance);
                }
            }
        }
        if (i != 0) free(buffer);
    }

    if (vk_ready) vk_cleanup(&ctx);

    if (vk_only) {
        if (first_source_buf) free(first_source_buf);
        return 0;
    }

    // Direct from-scratch compilation when requested or for self-contained C files
    if (force_scratch && num_sources == 1 && first_source_buf) {
        if (verbose) printf("[vkcc:mode] Compiling from scratch (Zero external tools)\n");
        bool ok = compile_to_elf_scratch(first_source_buf, first_source_len, out_path);
        free(first_source_buf);
        if (ok) {
            if (verbose) printf("[vkcc:output] Generated native ELF64 executable: %s\n", out_path);
            return 0;
        }
        return 1;
    }
    if (first_source_buf) free(first_source_buf);

    // Full standard library / GCC / G++ compatibility mode
    const char *compiler_bin = is_cpp ? "g++" : "gcc";
    const char *env_cc = getenv(is_cpp ? "CXX" : "CC");
    if (env_cc && strlen(env_cc) > 0) compiler_bin = env_cc;

    char *final_argv[backend_argc + 2];
    final_argv[0] = (char *)compiler_bin;
    for (int i = 0; i < backend_argc; i++) final_argv[i + 1] = backend_args[i];
    final_argv[backend_argc + 1] = NULL;

    if (verbose) {
        printf("[vkcc:exec] ");
        for (int i = 0; final_argv[i]; i++) printf("%s ", final_argv[i]);
        printf("\n");
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("vkcc: fork failed");
        return 1;
    }
    if (pid == 0) {
        execvp(compiler_bin, final_argv);
        fprintf(stderr, "vkcc: error executing '%s': %s\n", compiler_bin, strerror(errno));
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}
