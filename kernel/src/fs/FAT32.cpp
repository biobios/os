#include "fs/FAT32.hpp"
#include "utils/utils.hpp"


namespace oz {

// --- FAT32File ---

FAT32File::FAT32File(FAT32FileSystem* fs, std::uint32_t first_cluster, std::size_t size)
    : fs(fs), first_cluster(first_cluster), size(size), current_offset(0) {}

std::size_t FAT32File::read(void* buffer, std::size_t count) {
    if (current_offset >= size) return 0;
    
    std::size_t bytes_to_read = count;
    if (current_offset + bytes_to_read > size) {
        bytes_to_read = size - current_offset;
    }

    std::uint32_t cluster_size = fs->getClusterSize();
    std::uint32_t current_cluster = first_cluster;
    
    // Skip clusters based on current_offset
    std::size_t clusters_to_skip = current_offset / cluster_size;
    for (std::size_t i = 0; i < clusters_to_skip; ++i) {
        current_cluster = fs->getNextCluster(current_cluster);
        if (current_cluster >= 0x0FFFFFF8) return 0; // EOF
    }

    std::uint8_t* out = static_cast<std::uint8_t*>(buffer);
    std::size_t bytes_read = 0;
    std::size_t offset_in_cluster = current_offset % cluster_size;

    auto cluster_buf = std::impl::make_unique<std::uint8_t[]>(cluster_size); // Note: needs dynamic memory

    while (bytes_read < bytes_to_read) {
        if (!fs->readCluster(current_cluster, cluster_buf.get())) {
            break;
        }

        std::size_t chunk_size = cluster_size - offset_in_cluster;
        if (bytes_read + chunk_size > bytes_to_read) {
            chunk_size = bytes_to_read - bytes_read;
        }

        oz::utils::memcpy(out + bytes_read, cluster_buf.get() + offset_in_cluster, chunk_size);
        bytes_read += chunk_size;
        offset_in_cluster = 0; // Next clusters start at 0

        current_cluster = fs->getNextCluster(current_cluster);
        if (current_cluster >= 0x0FFFFFF8) break; // EOF
    }

    current_offset += bytes_read;
    return bytes_read;
}

std::size_t FAT32File::write(const void* buffer, std::size_t size) {
    // TODO: Implement file writing
    return 0;
}

bool FAT32File::seek(std::size_t offset) {
    if (offset > size) return false;
    current_offset = offset;
    return true;
}

std::size_t FAT32File::getSize() const {
    return size;
}

void FAT32File::close() {
    delete this;
}

// --- FAT32Directory ---

FAT32Directory::FAT32Directory(FAT32FileSystem* fs, std::uint32_t first_cluster)
    : fs(fs), first_cluster(first_cluster), current_cluster(first_cluster), offset_in_cluster(0) {}

bool FAT32Directory::readEntry(char* nameOut, std::size_t maxLen, bool& isDirOut) {
    if (current_cluster >= 0x0FFFFFF8) return false;
    
    std::uint32_t cluster_size = fs->getClusterSize();
    auto cluster_buf = std::impl::make_unique<std::uint8_t[]>(cluster_size);

    while (current_cluster < 0x0FFFFFF8) {
        if (!fs->readCluster(current_cluster, cluster_buf.get())) break;
        
        while (offset_in_cluster < cluster_size) {
            FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(cluster_buf.get() + offset_in_cluster);
            offset_in_cluster += sizeof(FAT_DirEntry);

            if (entry->name[0] == 0x00) {
                // End of directory
                return false;
            }
            if (static_cast<unsigned char>(entry->name[0]) == 0xE5) {
                // Deleted entry
                continue;
            }
            if ((entry->attr & 0x0F) == 0x0F) {
                // Long File Name (LFN) entry, skip for now
                continue;
            }
            if (entry->attr & 0x08) {
                // Volume ID, skip
                continue;
            }

            // Normal entry found
            int name_idx = 0;
            for (int i = 0; i < 8 && entry->name[i] != ' '; ++i) {
                if (name_idx < maxLen - 1) nameOut[name_idx++] = entry->name[i];
            }
            if (entry->name[8] != ' ') {
                if (name_idx < maxLen - 1) nameOut[name_idx++] = '.';
                for (int i = 8; i < 11 && entry->name[i] != ' '; ++i) {
                    if (name_idx < maxLen - 1) nameOut[name_idx++] = entry->name[i];
                }
            }
            nameOut[name_idx] = '\0';
            
            isDirOut = (entry->attr & 0x10) != 0;
            
            return true;
        }

        offset_in_cluster = 0;
        current_cluster = fs->getNextCluster(current_cluster);
    }
    
    return false;
}

void FAT32Directory::close() {
    delete this;
}

// --- FAT32FileSystem ---

FAT32FileSystem::FAT32FileSystem(BlockDevice* device) 
    : block_device(device), fat_start_sector(0), data_start_sector(0) {}

bool FAT32FileSystem::init() {
    std::uint8_t boot_sector[512];
    if (!block_device->readSectors(0, 1, boot_sector)) {
        return false;
    }

    oz::utils::memcpy(&bpb, boot_sector, sizeof(FAT32_BPB));
    
    if (bpb.signature != 0x28 && bpb.signature != 0x29) {
        // May not be FAT32, but let's be lenient or add more strict checks
    }

    fat_start_sector = bpb.reserved_sectors;
    std::uint32_t root_dir_sectors = ((bpb.root_dir_entries * 32) + (bpb.bytes_per_sector - 1)) / bpb.bytes_per_sector; // 0 for FAT32
    std::uint32_t fat_size = bpb.sectors_per_fat_16 != 0 ? bpb.sectors_per_fat_16 : bpb.sectors_per_fat_32;
    data_start_sector = bpb.reserved_sectors + (bpb.fat_count * fat_size) + root_dir_sectors;

    return true;
}

std::uint32_t FAT32FileSystem::getNextCluster(std::uint32_t current_cluster) {
    std::uint32_t fat_offset = current_cluster * 4;
    std::uint32_t fat_sector = fat_start_sector + (fat_offset / bpb.bytes_per_sector);
    std::uint32_t entry_offset = fat_offset % bpb.bytes_per_sector;

    std::uint8_t sector_buf[512]; // Assuming 512-byte sectors for FAT
    if (!block_device->readSectors(fat_sector, 1, sector_buf)) {
        return 0x0FFFFFFF; // Error reading FAT -> EOF
    }

    std::uint32_t next_cluster = *reinterpret_cast<std::uint32_t*>(&sector_buf[entry_offset]);
    return next_cluster & 0x0FFFFFFF;
}

bool FAT32FileSystem::readCluster(std::uint32_t cluster, void* buffer) {
    if (cluster < 2) return false;
    std::uint32_t sector = data_start_sector + (cluster - 2) * bpb.sectors_per_cluster;
    return block_device->readSectors(sector, bpb.sectors_per_cluster, buffer);
}

std::uint32_t FAT32FileSystem::getClusterSize() const {
    return bpb.bytes_per_sector * bpb.sectors_per_cluster;
}

void FAT32FileSystem::formatShortName(const char* name, char* out_name11) {
    oz::utils::memset(out_name11, ' ', 11);
    
    int i = 0;
    int j = 0;
    while (name[i] != '\0' && name[i] != '.' && j < 8) {
        out_name11[j++] = oz::utils::toupper(name[i++]);
    }
    
    if (name[i] == '.') {
        i++;
        j = 8;
        while (name[i] != '\0' && j < 11) {
            out_name11[j++] = oz::utils::toupper(name[i++]);
        }
    }
}

std::uint32_t FAT32FileSystem::findEntryInDir(std::uint32_t dir_cluster, const char* name, FAT_DirEntry& out_entry) {
    char short_name[11];
    formatShortName(name, short_name);
    
    std::uint32_t current_cluster = dir_cluster;
    std::uint32_t cluster_size = getClusterSize();
    auto cluster_buf = std::impl::make_unique<std::uint8_t[]>(cluster_size);

    while (current_cluster < 0x0FFFFFF8) {
        if (!readCluster(current_cluster, cluster_buf.get())) break;
        
        for (std::uint32_t offset = 0; offset < cluster_size; offset += sizeof(FAT_DirEntry)) {
            FAT_DirEntry* entry = reinterpret_cast<FAT_DirEntry*>(cluster_buf.get() + offset);
            
            if (entry->name[0] == 0x00) {
                return 0; // End of directory
            }
            if (static_cast<unsigned char>(entry->name[0]) == 0xE5) continue; // Deleted
            if ((entry->attr & 0x0F) == 0x0F) continue; // LFN
            if (entry->attr & 0x08) continue; // Volume ID
            
            if (oz::utils::memcmp(entry->name, short_name, 11) == 0) {
                out_entry = *entry;
                return (entry->cluster_high << 16) | entry->cluster_low;
            }
        }
        
        current_cluster = getNextCluster(current_cluster);
    }
    
    return 0; // Not found
}

std::unique_ptr<File> FAT32FileSystem::openFile(const char* path) {
    const char* p = path;
    if (*p == '/') p++;
    
    std::uint32_t current_dir_cluster = bpb.root_cluster;
    FAT_DirEntry entry;
    
    char name_buf[13];
    while (*p != '\0') {
        int i = 0;
        while (*p != '\0' && *p != '/' && i < 12) {
            name_buf[i++] = *p++;
        }
        name_buf[i] = '\0';
        
        while (*p == '/') p++; // Skip multiple slashes
        
        std::uint32_t cluster = findEntryInDir(current_dir_cluster, name_buf, entry);
        if (cluster == 0) return nullptr;
        
        if (*p == '\0') {
            // This is the last component (the file)
            if (entry.attr & 0x10) return nullptr; // Expected a file, got a directory
            return std::impl::make_unique<FAT32File>(this, cluster, entry.size);
        } else {
            // More components follow, so this must be a directory
            if (!(entry.attr & 0x10)) return nullptr; // Expected a directory, got a file
            current_dir_cluster = cluster;
        }
    }
    return nullptr;
}

std::unique_ptr<Directory> FAT32FileSystem::openDir(const char* path) {
    const char* p = path;
    if (*p == '/') p++;
    
    if (*p == '\0') {
        return std::impl::make_unique<FAT32Directory>(this, bpb.root_cluster);
    }
    
    std::uint32_t current_dir_cluster = bpb.root_cluster;
    FAT_DirEntry entry;
    
    char name_buf[13];
    while (*p != '\0') {
        int i = 0;
        while (*p != '\0' && *p != '/' && i < 12) {
            name_buf[i++] = *p++;
        }
        name_buf[i] = '\0';
        
        while (*p == '/') p++; // Skip slashes
        
        std::uint32_t cluster = findEntryInDir(current_dir_cluster, name_buf, entry);
        if (cluster == 0) return nullptr;
        
        if (!(entry.attr & 0x10)) return nullptr; // Expected a directory, got a file
        
        if (*p == '\0') {
            return std::impl::make_unique<FAT32Directory>(this, cluster);
        } else {
            current_dir_cluster = cluster;
        }
    }
    return nullptr;
}

} // namespace oz
