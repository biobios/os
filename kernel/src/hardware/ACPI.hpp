#pragma once
#include <cstddef>
#include <cstdint>

namespace ACPI {

struct SystemDescriptionTableHeader {
    std::uint8_t Signature[4];
    std::uint32_t Length;
    std::uint8_t Revision;
    std::uint8_t Checksum;
    std::uint8_t OEMID[6];
    std::uint8_t OEMTableID[8];
    std::uint32_t OEMRevision;
    std::uint32_t CreatorID;
    std::uint32_t CreatorRevision;
    void getSignature(char* out) {
        for (std::size_t i = 0; i < sizeof(Signature); i++) {
            out[i] = this->Signature[i];
        }
    }

    bool sameSignature(const std::uint8_t* sig){
        for(std::size_t i = 0; i < 4; i++){
            if(Signature[i] != sig[i]){
                return false;
            }
        }
        return true;
    }
};

struct RootSystemDescriptionTable {
    SystemDescriptionTableHeader SDTH;

    std::uint32_t getEntry(std::size_t index) const {
        return reinterpret_cast<const std::uint32_t*>(this + 1)[index];
    }
    std::size_t size() const {
        std::size_t length = SDTH.Length - sizeof(SystemDescriptionTableHeader);
        return length / sizeof(std::uint32_t);
    }
};

struct ExtendedSystemDescriptionTable {
    SystemDescriptionTableHeader SDTH;
    
    std::uint64_t getEntry(std::size_t index) const {
        const std::uint32_t* entries = reinterpret_cast<const std::uint32_t*>(this + 1) + (2 * index);
        return static_cast<std::uint64_t>(entries[0]) | (static_cast<std::uint64_t>(entries[1]) << 32);
    }
    std::size_t size() const {
        std::size_t length = SDTH.Length - sizeof(SystemDescriptionTableHeader);
        return length / (sizeof(std::uint64_t));
    }
};

struct RootSystemDescriptionPointer {
    std::uint8_t Signature[8];
    std::uint8_t Checksum;
    std::uint8_t OEMID[6];
    std::uint8_t Revision;
    std::uint32_t RsdtAddress;
    std::uint32_t Length;
    std::uint64_t XsdtAddress;
    std::uint8_t ExtendedChecksum;
    std::uint8_t Reserved[3];
};
}  // namespace ACPI