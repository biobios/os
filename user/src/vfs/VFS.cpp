#include "VFS.hpp"

namespace oz {

VFS::VFS() : root_fs(nullptr), disk_fs(nullptr) {}

VFS::~VFS() {}

void VFS::mountRoot(FileSystem* fs) {
    root_fs = fs;
}

void VFS::mountDisk(FileSystem* fs) {
    disk_fs = fs;
}

static bool is_disk_path(const char* path, const char*& out_subpath) {
    if (!path) return false;
    while (*path == '/') path++;
    if (path[0] == 'd' && path[1] == 'i' && path[2] == 's' && path[3] == 'k') {
        if (path[4] == '/') {
            out_subpath = path + 5;
            return true;
        } else if (path[4] == '\0') {
            out_subpath = "";
            return true;
        }
    }
    return false;
}

std::unique_ptr<File> VFS::open(const char* path) {
    const char* disk_sub = nullptr;
    if (disk_fs && is_disk_path(path, disk_sub)) {
        return disk_fs->openFile(disk_sub);
    }
    if (root_fs) {
        auto f = root_fs->openFile(path);
        if (f) return f;
    }
    if (disk_fs) {
        return disk_fs->openFile(path);
    }
    return nullptr;
}

std::unique_ptr<Directory> VFS::openDir(const char* path) {
    const char* disk_sub = nullptr;
    if (disk_fs && is_disk_path(path, disk_sub)) {
        return disk_fs->openDir(disk_sub);
    }
    if (root_fs) {
        auto d = root_fs->openDir(path);
        if (d) return d;
    }
    if (disk_fs) {
        return disk_fs->openDir(path);
    }
    return nullptr;
}

std::unique_ptr<File> VFS::openAt(std::uint32_t base_id, const char* rel_path) {
    const char* disk_sub = nullptr;
    if (disk_fs && is_disk_path(rel_path, disk_sub)) {
        return disk_fs->openFileAt(base_id, disk_sub);
    }
    if (root_fs) {
        auto f = root_fs->openFileAt(base_id, rel_path);
        if (f) return f;
    }
    if (disk_fs) {
        return disk_fs->openFileAt(base_id, rel_path);
    }
    return nullptr;
}

std::unique_ptr<Directory> VFS::openDirAt(std::uint32_t base_id, const char* rel_path) {
    const char* disk_sub = nullptr;
    if (disk_fs && is_disk_path(rel_path, disk_sub)) {
        return disk_fs->openDirAt(base_id, disk_sub);
    }
    if (root_fs) {
        auto d = root_fs->openDirAt(base_id, rel_path);
        if (d) return d;
    }
    if (disk_fs) {
        return disk_fs->openDirAt(base_id, rel_path);
    }
    return nullptr;
}

} // namespace oz

