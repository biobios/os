#pragma once
#include "VFS.hpp"
#include "../common/utils/utils.hpp"
#include <cstdint>
#include <cstddef>
#include <memory>

namespace oz {

class TarFile : public File {
private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};

public:
    TarFile(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size), offset_(0) {}

    std::size_t read(void* buffer, std::size_t size) override {
        if (!buffer || offset_ >= size_) return 0;
        std::size_t to_read = size;
        if (offset_ + to_read > size_) {
            to_read = size_ - offset_;
        }
        utils::memcpy(buffer, data_ + offset_, to_read);
        offset_ += to_read;
        return to_read;
    }

    std::size_t write(const void* buffer, std::size_t size) override {
        (void)buffer; (void)size;
        return 0; // TarFS is read-only
    }

    bool seek(std::size_t offset) override {
        if (offset > size_) {
            offset_ = size_;
        } else {
            offset_ = offset;
        }
        return true;
    }

    std::size_t getSize() const override {
        return size_;
    }

    void close() override {}
};

class TarDirectory : public Directory {
private:
    const std::uint8_t* tar_ptr_{nullptr};
    std::size_t tar_size_{0};
    std::size_t current_offset_{0};
    char dir_prefix_[64]{};
    std::size_t dir_prefix_len_{0};

public:
    TarDirectory(const std::uint8_t* tar_ptr, std::size_t tar_size, const char* dir_path = nullptr)
        : tar_ptr_(tar_ptr), tar_size_(tar_size), current_offset_(0) {
        if (tar_ptr_ && tar_size_ >= 512) {
            for (std::size_t off = 0; off + 512 <= tar_size_ && off < 4096; ++off) {
                if (tar_ptr_[off + 257] == 'u' && tar_ptr_[off + 258] == 's' &&
                    tar_ptr_[off + 259] == 't' && tar_ptr_[off + 260] == 'a' &&
                    tar_ptr_[off + 261] == 'r') {
                    tar_ptr_ += off;
                    tar_size_ -= off;
                    break;
                }
            }
        }
        if (dir_path) {
            while (*dir_path == '/' || *dir_path == '.') dir_path++;
            std::size_t i = 0;
            while (dir_path[i] != '\0' && i < sizeof(dir_prefix_) - 2) {
                dir_prefix_[i] = dir_path[i];
                i++;
            }
            if (i > 0 && dir_prefix_[i - 1] != '/') {
                dir_prefix_[i++] = '/';
            }
            dir_prefix_[i] = '\0';
            dir_prefix_len_ = i;
        }
    }

    bool readEntry(char* nameOut, std::size_t maxLen, bool& isDirOut) override {
        if (!tar_ptr_ || !nameOut || maxLen == 0) return false;

        while (current_offset_ + 512 <= tar_size_) {
            const char* header = reinterpret_cast<const char*>(tar_ptr_ + current_offset_);
            if (header[0] == '\0') break; // end of tar

            // Parse file size
            const char* size_str = header + 124;
            std::size_t file_size = 0;
            for (int i = 0; i < 11; ++i) {
                if (size_str[i] >= '0' && size_str[i] <= '7') {
                    file_size = (file_size * 8) + (size_str[i] - '0');
                }
            }

            const char* entry_name = header;
            while (*entry_name == '/' || *entry_name == '.') entry_name++;

            std::size_t next_offset = current_offset_ + 512 + ((file_size + 511) & ~511ULL);
            current_offset_ = next_offset;

            // Check if matches dir prefix
            if (dir_prefix_len_ > 0) {
                bool match = true;
                for (std::size_t i = 0; i < dir_prefix_len_; ++i) {
                    if (entry_name[i] != dir_prefix_[i]) {
                        match = false;
                        break;
                    }
                }
                if (!match) continue;
                entry_name += dir_prefix_len_;
            }

            if (entry_name[0] == '\0') continue;

            // Check if directory
            bool is_dir = false;
            std::size_t out_len = 0;
            while (entry_name[out_len] != '\0' && entry_name[out_len] != '/' && out_len < maxLen - 1) {
                nameOut[out_len] = entry_name[out_len];
                out_len++;
            }
            if (entry_name[out_len] == '/') {
                is_dir = true;
            }
            nameOut[out_len] = '\0';
            isDirOut = is_dir;
            return true;
        }

        return false;
    }

    void close() override {}
};

class TarFileSystem : public FileSystem {
private:
    const std::uint8_t* tar_ptr_{nullptr};
    std::size_t tar_size_{0};

    std::size_t detect_tar_offset() const {
        if (!tar_ptr_ || tar_size_ < 512) return 0;
        for (std::size_t off = 0; off + 512 <= tar_size_ && off < 4096; off += 512) {
            if (tar_ptr_[off + 257] == 'u' && tar_ptr_[off + 258] == 's' &&
                tar_ptr_[off + 259] == 't' && tar_ptr_[off + 260] == 'a' &&
                tar_ptr_[off + 261] == 'r') {
                return off;
            }
        }
        for (std::size_t off = 0; off + 512 <= tar_size_ && off < 4096; ++off) {
            if (tar_ptr_[off + 257] == 'u' && tar_ptr_[off + 258] == 's' &&
                tar_ptr_[off + 259] == 't' && tar_ptr_[off + 260] == 'a' &&
                tar_ptr_[off + 261] == 'r') {
                return off;
            }
        }
        return 0;
    }

    static const char* normalize_path(const char* path) {
        if (!path) return "";
        while (*path == '/' || *path == '.') path++;
        return path;
    }

    bool find_file(const char* target, const std::uint8_t*& out_data, std::size_t& out_size) const {
        if (!tar_ptr_ || tar_size_ < 512 || !target) return false;
        target = normalize_path(target);

        std::size_t offset = 0;
        while (offset + 512 <= tar_size_) {
            const char* header = reinterpret_cast<const char*>(tar_ptr_ + offset);
            if (header[0] == '\0') break;

            const char* size_str = header + 124;
            std::size_t file_size = 0;
            for (int i = 0; i < 11; ++i) {
                if (size_str[i] >= '0' && size_str[i] <= '7') {
                    file_size = (file_size * 8) + (size_str[i] - '0');
                }
            }

            const char* name = header;
            while (*name == '/' || *name == '.') name++;

            bool match = true;
            std::size_t i = 0;
            while (target[i] != '\0') {
                if (name[i] != target[i]) {
                    match = false;
                    break;
                }
                i++;
            }
            if (match && (name[i] == '\0' || name[i] == '/')) {
                out_data = tar_ptr_ + offset + 512;
                out_size = file_size;
                return true;
            }

            offset += 512 + ((file_size + 511) & ~511ULL);
        }

        return false;
    }

public:
    TarFileSystem(const std::uint8_t* tar_ptr, std::size_t tar_size = 32 * 1024 * 1024)
        : tar_ptr_(tar_ptr), tar_size_(tar_size) {
        std::size_t off = detect_tar_offset();
        if (off > 0 && off < tar_size_) {
            tar_ptr_ += off;
            tar_size_ -= off;
        }
    }

    std::unique_ptr<File> openFile(const char* path) override {
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
        if (find_file(path, data, size)) {
            return std::make_unique<TarFile>(data, size);
        }
        return nullptr;
    }

    std::unique_ptr<Directory> openDir(const char* path) override {
        return std::make_unique<TarDirectory>(tar_ptr_, tar_size_, path);
    }

    std::unique_ptr<File> openFileAt(std::uint32_t base_id, const char* rel_path) override {
        (void)base_id;
        return openFile(rel_path);
    }

    std::unique_ptr<Directory> openDirAt(std::uint32_t base_id, const char* rel_path) override {
        (void)base_id;
        return openDir(rel_path);
    }
};

} // namespace oz
