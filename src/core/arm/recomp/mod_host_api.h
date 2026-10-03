/*
 * mod_host_api.h: the stable interface between a recompiled game exe and an installable mod
 * loader DLL (Forge PC's forge.dll), in the spirit of Stracker's Loader for MH World.
 *
 * SPDX-License-Identifier: MIT
 *
 * The exported exe never changes for mods. Mods live in suyu's mod folder of the export,
 * <exe dir>/mods/<title id>/<mod name>/ (romfs/ there replaces game files, as in suyu). The exe
 * loads one loader DLL, game_settings.ini [Mods] loader (default "Forge/forge.dll", relative to
 * <exe dir>/mods/<title id>/; empty = none), and calls its export
 *
 *     MODHOST_EXPORT bool modhost_attach(const ModHostApi* api, ModHostCallbacks* callbacks);
 *
 * The loader fills in the callbacks it wants (leave the others NULL) and returns true. Everything
 * else (plugins, menus, hooks tables) lives in the loader and is updated by replacing files.
 *
 * Compatibility rules (this file is a contract with every exe already exported):
 *   - Structs only grow at the end. Each starts with its size and version; read a field only if the size
 *     the other side reports covers it (MODHOST_HAS(Type, ptr, field)).
 *   - MODHOST_API_VERSION grows with every addition. Nothing is ever removed or reordered.
 *
 * Game addresses are u32 guest addresses of the 32-bit ARM game, not host pointers.
 */
#ifndef MOD_HOST_API_H
#define MOD_HOST_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODHOST_API_VERSION 2 /* 2: guest_alloc, guest_free, guest_heap_start/size */

#ifdef _WIN32
#define MODHOST_EXPORT __declspec(dllexport)
#else
#define MODHOST_EXPORT __attribute__((visibility("default")))
#endif

#define MODHOST_HAS(type, ptr, field)                                                              \
    ((ptr)->size >= offsetof(type, field) + sizeof((ptr)->field))

/* Guest return addresses reserved for the loader: kReturn brings a hooked function's return
 * back to on_dispatch (post callbacks); kCall ends calls made through ModHostApi.call. */
#define MODHOST_RETURN_PC 0xFFFFE000u
#define MODHOST_CALL_RETURN_PC 0xFFFFE010u
#define MODHOST_RESERVED_PC 0xFFFFE020u /* reserved for a future use */

/* Guest CPU state at a dispatch (the recompiler's context prefix). r[13] sp, r[14] lr, r[15] pc.
 * vfp: s[0..63] / d[0..31] overlap as on ARM. The game uses the hard-float ABI. */
typedef struct ModHostCpu {
    uint32_t r[16];
    uint32_t n, z, c, v, q;
    uint32_t ge;
    uint32_t thumb;
    uint32_t fpscr;
    union {
        float s[64];
        double d[32];
        uint32_t sw[64];
        uint64_t dw[32];
    } vfp;
} ModHostCpu;

/* A loaded game module. Segment bounds are 0 when the exe can't tell (then use base/size). */
typedef struct ModHostModule {
    uint32_t size;      /* sizeof(ModHostModule) of the caller */
    uint32_t version;   /* MODHOST_API_VERSION of the writer */
    const char* name;   /* as loaded, e.g. "Rabbit_RoApplicationMTFMasterReleaseNX.nss" */
    const char* alias;  /* load-order name: "rtld", "main", "subsdk0", "sdk" ... */
    uint32_t base;
    uint32_t span;      /* bytes up to the next module (or the end of this one) */
    uint32_t text_start, text_end;     /* R-X */
    uint32_t rodata_start, rodata_end; /* R-- */
    uint32_t data_start, data_end;     /* RW- (.data + .bss) */
} ModHostModule;

typedef enum ModHostLogLevel {
    MODHOST_LOG_DEBUG = 0,
    MODHOST_LOG_INFO = 1,
    MODHOST_LOG_WARN = 2,
    MODHOST_LOG_ERROR = 3,
} ModHostLogLevel;

/* What the exe offers the loader. Valid from modhost_attach until on_shutdown returns. */
typedef struct ModHostApi {
    uint32_t size;    /* sizeof(ModHostApi) of the exe */
    uint32_t version; /* MODHOST_API_VERSION of the exe */
    const char* game_title_id; /* "0100770008DD8000" */
    const char* game_version;  /* "1.4.0" */
    const char* exe_dir;       /* UTF-8, no trailing slash */
    const char* user_dir;      /* the exe's user folder (logs, NAND, config) */
    const char* mods_dir;      /* <exe dir>/mods/<title id>: one folder per mod */
    const char* loader_dir;    /* folder of the loader DLL itself */

    /* exe log (the game's log file) */
    void (*log)(ModHostLogLevel level, const char* message);

    /* game_settings.ini value ("" if absent); returns the full length */
    uint32_t (*ini_get)(const char* section, const char* key, char* out, uint32_t out_size);

    /* modules: valid from on_game_start on (0 before) */
    uint32_t (*module_count)(void);
    bool (*module_get)(uint32_t index, ModHostModule* out); /* out->size set by the caller */

    /* guest memory */
    bool (*mem_valid)(uint32_t addr, uint32_t size);
    bool (*mem_read)(uint32_t addr, void* out, uint32_t size);
    bool (*mem_write)(uint32_t addr, const void* data, uint32_t size);
    void* (*mem_ptr)(uint32_t addr, uint32_t size); /* NULL unless host-contiguous */

    /* code: is_block = a recompiled block starts at pc (hookable). mark = the dispatcher calls
     * on_dispatch whenever the game is about to run pc (also when reached by a chained branch).
     * Marks are permanent; ignore unwanted ones in on_dispatch. */
    bool (*is_block)(uint32_t pc);
    void (*mark)(uint32_t pc);

    /* calls a guest function on the current game thread. Only from on_dispatch or
     * on_game_start: returns false when called from any other thread. Integer args in r0-r3
     * then the stack, float args in s0-s15. out_r[0..1] = r0, r1; out_d0 = d0 (s0 = its low
     * half). Hooks don't fire inside. Returns false if it could not complete (registers are
     * restored). */
    bool (*call)(uint32_t function, const uint32_t* args, uint32_t count, const float* fargs,
                 uint32_t fcount, uint32_t out_r[2], double* out_d0);

    /* while true, the game doesn't receive controller input (a menu has it) */
    void (*set_input_captured)(bool captured);

    /* while true, the window delivers MODHOST_INPUT_TEXT events (typing into a text field;
     * may show the system's IME). Off by default. */
    void (*set_text_input)(bool enabled);

    /* --- version 2 --- */

    /* guest memory for mods: a region the exe maps for the process before the game starts
     * (guest_heap_start, guest_heap_size; 0 if it couldn't), readable and writable by game code
     * like any other memory. guest_alloc returns zeroed memory (align: a power of two, 16 at
     * least) or 0 when the region is full; guest_free(0) does nothing. Thread-safe; addresses
     * stay valid until guest_free or the end of the process. */
    uint32_t (*guest_alloc)(uint32_t size, uint32_t align);
    void (*guest_free)(uint32_t addr);
    uint32_t guest_heap_start;
    uint32_t guest_heap_size;
} ModHostApi;

/* One presented frame, handed to on_present on the presentation thread (suyu's PresentThread
 * with async presentation, else the GPU thread): always the same thread, so a loader can keep
 * all its UI work (e.g. ImGui) there. The only ways to touch the GPU are recording into
 * vk_command_buffer, or a submit of your own made between lock_queue and unlock_queue. Vulkan handles are
 * passed as void* / uint64_t so this header needs no Vulkan headers. */
typedef struct ModHostPresent {
    uint32_t size;
    uint32_t version;
    uint32_t vk_api_version;
    void* vk_instance;             /* VkInstance */
    void* vk_get_instance_proc;    /* PFN_vkGetInstanceProcAddr */
    void* vk_physical_device;      /* VkPhysicalDevice */
    void* vk_device;               /* VkDevice */
    void* vk_queue;                /* VkQueue (shared with the emulator: see lock_queue) */
    uint32_t vk_queue_family;
    void* vk_command_buffer;       /* VkCommandBuffer being recorded for this present */
    uint64_t vk_image;             /* VkImage: the swapchain image, in TRANSFER_DST_OPTIMAL */
    uint32_t vk_format;            /* VkFormat of views on that image */
    uint32_t width, height;
    uint32_t image_count;          /* swapchain images; changes when it is recreated */
    uint32_t image_index;
    /* hold while submitting to vk_queue yourself (e.g. a texture upload) */
    void (*lock_queue)(void);
    void (*unlock_queue)(void);
} ModHostPresent;

typedef enum ModHostInputType {
    MODHOST_INPUT_KEY = 0,          /* scancode (USB HID usage, = SDL scancode), down, mods */
    MODHOST_INPUT_MOUSE_MOVE = 1,   /* x, y in window pixels */
    MODHOST_INPUT_MOUSE_BUTTON = 2, /* button 1 left, 2 middle, 3 right, 4/5 extra; down; x, y */
    MODHOST_INPUT_MOUSE_WHEEL = 3,  /* wheel_x, wheel_y */
    MODHOST_INPUT_TEXT = 4,         /* text (UTF-8) */
    MODHOST_INPUT_FOCUS = 5,        /* down = window focused */
} ModHostInputType;

typedef struct ModHostInput {
    uint32_t size;
    uint32_t version;
    ModHostInputType type;
    int32_t scancode;
    bool down;
    bool repeat;
    uint16_t mods; /* bit 0 shift, 1 ctrl, 2 alt, 3 super */
    float x, y;
    int32_t button;
    float wheel_x, wheel_y;
    char text[32];
} ModHostInput;

/* What the loader hands back. The exe zeroes it and sets size before modhost_attach. */
typedef struct ModHostCallbacks {
    uint32_t size;    /* sizeof(ModHostCallbacks) of the exe: don't write past it */
    uint32_t version; /* the loader sets its MODHOST_API_VERSION */

    /* modules loaded, before the game's first instruction (on an emulated CPU thread) */
    void (*on_game_start)(void);

    /* the game is about to run a marked pc, or MODHOST_RETURN_PC (the exe filters every other
     * pc itself, cheaply, so this is only called for those). Change cpu freely; return
     * true if r15 (cpu->r[15], cpu->thumb) now points somewhere else. Runs on the game's
     * threads, concurrently: keep it short and thread-safe. */
    bool (*on_dispatch)(uint32_t pc, ModHostCpu* cpu, uint32_t thread_key);

    /* draw over the game's frame: record into p->vk_command_buffer. Return true if anything
     * was recorded; the image must then be in COLOR_ATTACHMENT_OPTIMAL (the exe moves it on to
     * PRESENT_SRC). Return false to leave it untouched in TRANSFER_DST_OPTIMAL. */
    bool (*on_present)(const ModHostPresent* p);

    /* window input (on the window's event thread). Return true to hide it from the game. */
    bool (*on_input)(const ModHostInput* e);

    /* emulation is ending (no more dispatch / present calls after this returns) */
    void (*on_shutdown)(void);

    /* once per frame the game finishes (the emulator's system frame end, on that thread; not
     * the present thread) */
    void (*on_frame)(void);
} ModHostCallbacks;

typedef bool (*ModHostAttachFn)(const ModHostApi* api, ModHostCallbacks* callbacks);

#ifdef __cplusplus
}
#endif

#endif /* MOD_HOST_API_H */
