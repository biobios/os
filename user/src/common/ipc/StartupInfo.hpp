#pragma once
#include <cstdint>
#include <cstddef>

namespace oz {

// ============================================================================
// 1. Process Manifest (Declared in ELF .oz_manifest section)
// ============================================================================
struct ProcessManifest {
    static constexpr std::uint32_t MAGIC = 0x464D5A4FULL; // 'OZMF'

    std::uint32_t magic{MAGIC};
    std::uint32_t version{1};
    
    // Requested standard capabilities
    bool needs_console{true};      // Requires stdin / stdout
    bool needs_vfs_root{false};    // Requires full root VFS directory
    bool needs_display{false};     // Requires Display Server Channel
    bool needs_wm{false};          // Requires Window Manager Channel
    bool needs_untyped{false};     // Requires UntypedMemory (privileged)

    // Requested dedicated subdirectories (if needs_vfs_root is false)
    char default_data_dir[32]{};   // e.g. "data" or "system"
};

#define OZ_MANIFEST(manifest_obj) \
    __attribute__((section(".oz_manifest"), used)) \
    const oz::ProcessManifest g_oz_manifest = manifest_obj;

// ============================================================================
// 2. Process Startup Info (Local Namespace passed at startup)
// ============================================================================
enum class CapRole : std::uint32_t {
    NONE         = 0,
    STDIN        = 1, // Standard input channel
    STDOUT       = 2, // Standard output channel
    STDERR       = 3, // Standard error channel
    ROOT_DIR     = 4, // Primary directory capability (root or sandboxed folder)
    DISPLAY      = 5, // Display Server channel
    WM           = 6, // Window Manager session channel
    UNTYPED      = 7, // UntypedMemory capability
    CUSTOM       = 8, // Named custom service / channel
};

enum class CapType : std::uint32_t {
    STREAM  = 0, // Byte stream channel (Pipe, Console, STDIN/STDOUT)
    FILE    = 1, // Dedicated VFS File Channel (RPC FileCmd)
    DIR     = 2, // Directory Channel
};

struct StartupCapEntry {
    std::uint32_t slot{0};       // Slot ID in child's CSpace
    CapRole       role{CapRole::NONE};
    CapType       type{CapType::STREAM};
    char          name[32]{};    // Logical name (e.g. "stdin", "/", "memo.txt")
};

struct ProcessStartupInfo {
    static constexpr std::uint32_t MAGIC = 0x49535A4FULL; // 'OZSI'
    static constexpr std::size_t MAX_CAPS = 16;
    static constexpr std::size_t MAX_ARGS = 16;
    static constexpr std::size_t ARG_BUF_SIZE = 512;

    std::uint32_t magic{MAGIC};
    std::uint32_t num_caps{0};
    StartupCapEntry caps[MAX_CAPS]{};

    // Command-line arguments
    std::uint32_t argc{0};
    std::uint32_t arg_buf_len{0};
    std::uint32_t arg_offsets[MAX_ARGS]{};
    char          arg_buf[ARG_BUF_SIZE]{};

    // Helper: find slot by role
    std::uint32_t find_slot(CapRole role) const {
        for (std::size_t i = 0; i < num_caps; ++i) {
            if (caps[i].role == role) return caps[i].slot;
        }
        return 0; // INVALID_CAP_ID
    }

    // Helper: find slot by name
    std::uint32_t find_slot_by_name(const char* name_query) const {
        if (!name_query) return 0;
        for (std::size_t i = 0; i < num_caps; ++i) {
            const char* a = caps[i].name;
            const char* b = name_query;
            bool match = true;
            while (*a && *b) {
                if (*a++ != *b++) { match = false; break; }
            }
            if (match && *a == *b) return caps[i].slot;
        }
        return 0;
    }

    // Helper: add capability to startup info
    bool add_cap(std::uint32_t slot, CapRole role, const char* name_str, CapType type = CapType::STREAM) {
        if (num_caps >= MAX_CAPS) return false;
        caps[num_caps].slot = slot;
        caps[num_caps].role = role;
        caps[num_caps].type = type;
        std::size_t i = 0;
        if (name_str) {
            for (; i < sizeof(caps[num_caps].name) - 1 && name_str[i] != '\0'; ++i) {
                caps[num_caps].name[i] = name_str[i];
            }
        }
        caps[num_caps].name[i] = '\0';
        num_caps++;
        return true;
    }

    // Helper: get argument by index
    const char* get_arg(std::size_t index) const {
        if (index >= argc || index >= MAX_ARGS) return nullptr;
        return &arg_buf[arg_offsets[index]];
    }

    // Helper: add argument
    bool add_arg(const char* str) {
        if (argc >= MAX_ARGS || !str) return false;
        std::size_t len = 0;
        while (str[len] != '\0') len++;

        if (arg_buf_len + len + 1 > ARG_BUF_SIZE) return false;

        arg_offsets[argc] = arg_buf_len;
        for (std::size_t i = 0; i < len; ++i) {
            arg_buf[arg_buf_len++] = str[i];
        }
        arg_buf[arg_buf_len++] = '\0';
        argc++;
        return true;
    }
};

// ============================================================================
// 3. Local Namespace runtime helper for client processes
// ============================================================================
class LocalNamespace {
private:
    static inline const ProcessStartupInfo* g_info{nullptr};

public:
    static void init(const ProcessStartupInfo* info) {
        if (info && info->magic == ProcessStartupInfo::MAGIC) {
            g_info = info;
        }
    }

    static const ProcessStartupInfo* get() {
        return g_info;
    }

    static std::uint32_t get_cap(CapRole role, std::uint32_t fallback = 0) {
        if (!g_info) return fallback;
        std::uint32_t slot = g_info->find_slot(role);
        return (slot != 0) ? slot : fallback;
    }

    static std::uint32_t get_cap(const char* name, std::uint32_t fallback = 0) {
        if (!g_info) return fallback;
        std::uint32_t slot = g_info->find_slot_by_name(name);
        return (slot != 0) ? slot : fallback;
    }

    static CapType get_cap_type(std::uint32_t slot) {
        if (!g_info) return CapType::STREAM;
        for (std::size_t i = 0; i < g_info->num_caps; ++i) {
            if (g_info->caps[i].slot == slot) return g_info->caps[i].type;
        }
        return CapType::STREAM;
    }

    static const StartupCapEntry* get_cap_entry(CapRole role) {
        if (!g_info) return nullptr;
        for (std::size_t i = 0; i < g_info->num_caps; ++i) {
            if (g_info->caps[i].role == role) return &g_info->caps[i];
        }
        return nullptr;
    }

    static const StartupCapEntry* get_cap_entry(const char* name) {
        if (!g_info || !name) return nullptr;
        for (std::size_t i = 0; i < g_info->num_caps; ++i) {
            const char* a = g_info->caps[i].name;
            const char* b = name;
            bool match = true;
            while (*a && *b) {
                if (*a++ != *b++) { match = false; break; }
            }
            if (match && *a == *b) return &g_info->caps[i];
        }
        return nullptr;
    }

    static std::uint32_t get_argc() {
        return g_info ? g_info->argc : 0;
    }

    static const char* get_argv(std::size_t index) {
        return g_info ? g_info->get_arg(index) : nullptr;
    }
};

} // namespace oz

extern "C" void sys_exit(int status);

// ============================================================================
// 4. Standard entry point macro for capability-aware processes
// ============================================================================
#define OZ_PROCESS_ENTRY() \
    extern "C" void _start_main(const oz::ProcessStartupInfo* info); \
    extern "C" __attribute__((naked)) void _start() { \
        __asm__ volatile ( \
            "movq (%%rsp), %%rdi\n" \
            "andq $-16, %%rsp\n" \
            "call _start_main\n" \
            "movq $0, %%rdi\n" \
            "call sys_exit\n" \
            ::: "memory" \
        ); \
    } \
    extern "C" void _start_main(const oz::ProcessStartupInfo* info)

