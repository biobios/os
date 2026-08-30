#pragma once
#include "fs/VFS.hpp"
#include "drivers/storage/BlockDevice.hpp"
#include <cstdint>
#include <memory>

namespace oz {

#pragma pack(push, 1)
struct FAT32_BPB {
    std::uint8_t  jmp[3];
    char          oem[8];
    std::uint16_t bytes_per_sector;
    std::uint8_t  sectors_per_cluster;
    std::uint16_t reserved_sectors;
    std::uint8_t  fat_count;
    std::uint16_t root_dir_entries;
    std::uint16_t total_sectors_16;
    std::uint8_t  media_descriptor;
    std::uint16_t sectors_per_fat_16;
    std::uint16_t sectors_per_track;
    std::uint16_t head_count;
    std::uint32_t hidden_sectors;
    std::uint32_t total_sectors_32;
    
    // FAT32 Extended Boot Record
    std::uint32_t sectors_per_fat_32;
    std::uint16_t flags;
    std::uint16_t fat_version;
    std::uint32_t root_cluster;
    std::uint16_t fsinfo_sector;
    std::uint16_t backup_boot_sector;
    std::uint8_t  reserved[12];
    std::uint8_t  drive_number;
    std::uint8_t  nt_flags;
    std::uint8_t  signature;
    std::uint32_t volume_id;
    char          volume_label[11];
    char          sys_identifier[8];
};

struct FAT_DirEntry {
    char          name[11];
    std::uint8_t  attr;
    std::uint8_t  nt_reserved;
    std::uint8_t  creation_time_tenth;
    std::uint16_t creation_time;
    std::uint16_t creation_date;
    std::uint16_t last_access_date;
    std::uint16_t cluster_high;
    std::uint16_t write_time;
    std::uint16_t write_date;
    std::uint16_t cluster_low;
    std::uint32_t size;
};
#pragma pack(pop)

class FAT32FileSystem;

class FAT32File : public File {
private:
    FAT32FileSystem* fs;
    std::uint32_t first_cluster;
    std::size_t size;
    std::size_t current_offset;

public:
    FAT32File(FAT32FileSystem* fs, std::uint32_t first_cluster, std::size_t size);
    ~FAT32File() override = default;

    std::size_t read(void* buffer, std::size_t size) override;
    std::size_t write(const void* buffer, std::size_t size) override;
    bool seek(std::size_t offset) override;
    std::size_t getSize() const override;
    void close() override;
};

class FAT32Directory : public Directory {
private:
    FAT32FileSystem* fs;
    std::uint32_t first_cluster;
    std::uint32_t current_cluster;
    std::uint32_t offset_in_cluster;

public:
    FAT32Directory(FAT32FileSystem* fs, std::uint32_t first_cluster);
    ~FAT32Directory() override = default;

    bool readEntry(char* nameOut, std::size_t maxLen, bool& isDirOut) override;
    void close() override;
};

class FAT32FileSystem : public FileSystem {
private:
    BlockDevice* block_device;
    FAT32_BPB bpb;
    
    std::uint32_t fat_start_sector;
    std::uint32_t data_start_sector;
    
public:
    FAT32FileSystem(BlockDevice* device);
    ~FAT32FileSystem() override = default;

    bool init();

    std::unique_ptr<File> openFile(const char* path) override;
    std::unique_ptr<Directory> openDir(const char* path) override;

    // Helper functions for reading clusters
    std::uint32_t getNextCluster(std::uint32_t current_cluster);
    bool readCluster(std::uint32_t cluster, void* buffer);
    std::uint32_t getClusterSize() const;
    
private:
    std::uint32_t findEntryInDir(std::uint32_t dir_cluster, const char* name, FAT_DirEntry& out_entry);
    void formatShortName(const char* name, char* out_name11);
};

} // namespace oz
