#pragma once
#include "syscall.hpp"
#include "../src/common/ipc/StartupInfo.hpp"
#include "../src/common/ipc/Protocol.hpp"
#include "../src/common/utils/utils.hpp"
#include <cstdint>
#include <cstddef>

namespace oz::io {

class FileHandle {
public:
    FileHandle() = default;
    FileHandle(std::uint32_t slot, CapType type = CapType::STREAM)
        : cap_slot_(slot), type_(type) {}

    static FileHandle from_role(CapRole role) {
        std::uint32_t slot = LocalNamespace::get_cap(role);
        if (slot == 0) return FileHandle{};
        CapType type = LocalNamespace::get_cap_type(slot);
        return FileHandle{slot, type};
    }

    static FileHandle from_name(const char* name) {
        if (!name) return FileHandle{};
        std::uint32_t slot = LocalNamespace::get_cap(name);
        if (slot == 0) return FileHandle{};
        CapType type = LocalNamespace::get_cap_type(slot);
        return FileHandle{slot, type};
    }

    static FileHandle from_slot(std::uint32_t slot, CapType type = CapType::STREAM) {
        return FileHandle{slot, type};
    }

    bool is_valid() const {
        return cap_slot_ != 0 && cap_slot_ != INVALID_CAP_ID;
    }

    std::uint32_t slot() const {
        return cap_slot_;
    }

    CapType type() const {
        return type_;
    }

    std::size_t read(void* buf, std::size_t size) {
        if (!is_valid() || !buf || size == 0) return 0;

        char* dst = static_cast<char*>(buf);
        std::size_t total_read = 0;

        if (type_ == CapType::FILE) {
            while (total_read < size) {
                std::size_t want = size - total_read;
                if (want > 24) want = 24;

                IPCMessage req{};
                req.data[0] = ipc::file_make_cmd(ipc::FileCmd::READ, static_cast<std::uint32_t>(want));
                req.data[1] = ipc::FILE_STREAM_OFFSET; // sequential stream read

                IPCMessage resp{};
                if (!sys_call(cap_slot_, &req, &resp)) break;

                auto status = static_cast<ipc::VfsStatus>(resp.data[0] & 0xFFFFFFFFULL);
                if (status != ipc::VfsStatus::OK) break;

                std::uint32_t bytes_read = ipc::file_get_len(resp.data[0]);
                if (bytes_read == 0) break; // EOF

                utils::memcpy(dst + total_read, &resp.data[1], bytes_read);
                total_read += bytes_read;

                if (bytes_read < want) break; // Reached end of file
            }
        } else {
            // Stream (Pipe / STDIN)
            IPCMessage msg{};
            ReplyToken tok = sys_reply_recv(INVALID_REPLY_TOKEN, cap_slot_, nullptr, &msg);
            if (tok == INVALID_REPLY_TOKEN) {
                return 0; // Peer closed or error -> EOF
            }
            IPCMessage resp{};
            sys_reply_recv(tok, 0, &resp, nullptr);

            std::uint32_t len = ipc::stream_get_len(msg.data[0]);
            if (len > 0 && len <= 24) {
                // Formatted stream message
                std::size_t to_copy = (len < size) ? len : size;
                utils::memcpy(dst, &msg.data[1], to_copy);
                total_read = to_copy;
            } else {
                // Raw 32-byte message (e.g. from console or raw text)
                alignas(8) char tmp[33]{};
                for (int k = 0; k < 4; ++k) {
                    *reinterpret_cast<std::uint64_t*>(&tmp[k * 8]) = msg.data[k];
                }
                tmp[32] = '\0';
                std::size_t actual_len = 0;
                while (actual_len < 32 && tmp[actual_len] != '\0') actual_len++;
                std::size_t to_copy = (actual_len < size) ? actual_len : size;
                utils::memcpy(dst, tmp, to_copy);
                total_read = to_copy;
            }
        }

        return total_read;
    }

    std::size_t write(const void* buf, std::size_t size) {
        if (!is_valid() || !buf || size == 0) return 0;

        const char* src = static_cast<const char*>(buf);
        std::size_t total_written = 0;

        if (type_ == CapType::FILE) {
            while (total_written < size) {
                std::size_t chunk = size - total_written;
                if (chunk > 16) chunk = 16;

                IPCMessage req{};
                req.data[0] = ipc::file_make_cmd(ipc::FileCmd::WRITE, static_cast<std::uint32_t>(chunk));
                req.data[1] = ipc::FILE_STREAM_OFFSET;
                utils::memcpy(&req.data[2], src + total_written, chunk);

                IPCMessage resp{};
                if (!sys_call(cap_slot_, &req, &resp)) break;

                auto status = static_cast<ipc::VfsStatus>(resp.data[0] & 0xFFFFFFFFULL);
                if (status != ipc::VfsStatus::OK) break;

                std::uint32_t bytes_written = ipc::file_get_len(resp.data[0]);
                total_written += bytes_written;
                if (bytes_written < chunk) break;
            }
        } else {
            // Stream (Pipe / STDOUT)
            if (cap_slot_ == 1) {
                // Console STDOUT: uses standard 32-byte chunks
                std::size_t i = 0;
                while (i < size) {
                    alignas(8) char cbuf[32]{};
                    int j = 0;
                    for (; j < 31 && i < size; ++j, ++i) {
                        cbuf[j] = src[i];
                    }
                    cbuf[j] = '\0';

                    IPCMessage msg{};
                    for (int k = 0; k < 4; ++k) {
                        msg.data[k] = *reinterpret_cast<std::uint64_t*>(&cbuf[k * 8]);
                    }
                    IPCMessage resp{};
                    if (!sys_call(cap_slot_, &msg, &resp)) break;
                    total_written = i;
                }
            } else {
                // Pipe Stream: uses stream_make_header(len) + payload in data[1..3]
                while (total_written < size) {
                    std::size_t chunk = size - total_written;
                    if (chunk > 24) chunk = 24;

                    IPCMessage msg{};
                    msg.data[0] = ipc::stream_make_header(static_cast<std::uint32_t>(chunk));
                    utils::memcpy(&msg.data[1], src + total_written, chunk);

                    IPCMessage resp{};
                    if (!sys_call(cap_slot_, &msg, &resp)) break;
                    total_written += chunk;
                }
            }
        }

        return total_written;
    }

    std::size_t size() const {
        if (!is_valid() || type_ != CapType::FILE) return 0;
        IPCMessage req{};
        req.data[0] = ipc::file_make_cmd(ipc::FileCmd::GET_SIZE);
        IPCMessage resp{};
        if (!sys_call(cap_slot_, &req, &resp)) return 0;
        return static_cast<std::size_t>(resp.data[1]);
    }

    void close() {
        if (!is_valid()) return;
        if (type_ == CapType::FILE) {
            IPCMessage req{};
            req.data[0] = ipc::file_make_cmd(ipc::FileCmd::CLOSE);
            IPCMessage resp{};
            sys_call(cap_slot_, &req, &resp);
        }
        sys_cap_delete(cap_slot_);
        cap_slot_ = 0;
    }

private:
    std::uint32_t cap_slot_{0};
    CapType       type_{CapType::STREAM};
};

// C-style helper functions
inline std::size_t read_cap(std::uint32_t cap_slot, void* buf, std::size_t size) {
    CapType type = LocalNamespace::get_cap_type(cap_slot);
    FileHandle h(cap_slot, type);
    return h.read(buf, size);
}

inline std::size_t write_cap(std::uint32_t cap_slot, const void* buf, std::size_t size) {
    CapType type = LocalNamespace::get_cap_type(cap_slot);
    FileHandle h(cap_slot, type);
    return h.write(buf, size);
}

} // namespace oz::io
