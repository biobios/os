#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>

namespace oz {

class File {
public:
    virtual ~File() = default;
    
    // Read from file into buffer, returns bytes read
    virtual std::size_t read(void* buffer, std::size_t size) = 0;
    
    // Write to file from buffer, returns bytes written
    virtual std::size_t write(const void* buffer, std::size_t size) = 0;
    
    // Move the file cursor to the specified offset
    virtual bool seek(std::size_t offset) = 0;
    
    // Get the total size of the file in bytes
    virtual std::size_t getSize() const = 0;
    
    // Close the file and free associated resources
    virtual void close() = 0;
};

class Directory {
public:
    virtual ~Directory() = default;
    
    // Read the next entry in the directory
    // Returns true if an entry was read, false if end of directory
    virtual bool readEntry(char* nameOut, std::size_t maxLen, bool& isDirOut) = 0;
    
    // Close the directory and free associated resources
    virtual void close() = 0;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    
    virtual std::unique_ptr<File> openFile(const char* path) = 0;
    virtual std::unique_ptr<Directory> openDir(const char* path) = 0;

    virtual std::unique_ptr<File> openFileAt(std::uint32_t base_id, const char* rel_path) {
        (void)base_id; (void)rel_path; return nullptr;
    }
    virtual std::unique_ptr<Directory> openDirAt(std::uint32_t base_id, const char* rel_path) {
        (void)base_id; (void)rel_path; return nullptr;
    }
};

class VFS {
private:
    FileSystem* root_fs;
    FileSystem* disk_fs{nullptr};

public:
    VFS();
    ~VFS();

    // Mount the root filesystem (e.g. TarFS)
    void mountRoot(FileSystem* fs);

    // Mount the secondary disk filesystem (e.g. FAT32 at /disk)
    void mountDisk(FileSystem* fs);
    FileSystem* getDiskFS() const { return disk_fs; }
    
    // Open a file by path
    std::unique_ptr<File> open(const char* path);
    
    // Open a directory by path
    std::unique_ptr<Directory> openDir(const char* path);

    // Capability-based relative open operations
    std::unique_ptr<File> openAt(std::uint32_t base_id, const char* rel_path);
    std::unique_ptr<Directory> openDirAt(std::uint32_t base_id, const char* rel_path);
};


} // namespace oz
