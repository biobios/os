#include "VFS.hpp"

namespace oz {

VFS::VFS() : root_fs(nullptr) {}

VFS::~VFS() {}

void VFS::mountRoot(FileSystem* fs) {
    root_fs = fs;
}

std::unique_ptr<File> VFS::open(const char* path) {
    if (!root_fs) return nullptr;
    
    // For now, we simply pass the path to the root filesystem.
    // In the future, this should parse the path and resolve mount points.
    return root_fs->openFile(path);
}

std::unique_ptr<Directory> VFS::openDir(const char* path) {
    if (!root_fs) return nullptr;
    return root_fs->openDir(path);
}

std::unique_ptr<File> VFS::openAt(std::uint32_t base_id, const char* rel_path) {
    if (!root_fs) return nullptr;
    return root_fs->openFileAt(base_id, rel_path);
}

std::unique_ptr<Directory> VFS::openDirAt(std::uint32_t base_id, const char* rel_path) {
    if (!root_fs) return nullptr;
    return root_fs->openDirAt(base_id, rel_path);
}

} // namespace oz

