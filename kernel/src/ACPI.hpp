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

struct Byte8AlignedBy4 {
    std::uint32_t lowerData;
    std::uint32_t upperData;
    std::uint64_t getData() {
        return (static_cast<std::uint64_t>(this->upperData) << 32) |
               static_cast<std::uint64_t>(this->lowerData);
    }
};

struct ExtendedSystemDescriptionTable {
    SystemDescriptionTableHeader SDTH;
    SystemDescriptionTableHeader* getEntry(std::size_t index) {
        Byte8AlignedBy4* entry = reinterpret_cast<Byte8AlignedBy4*>(
            reinterpret_cast<std::uint8_t*>(this) +
            sizeof(SystemDescriptionTableHeader));

        entry += index;
        return reinterpret_cast<SystemDescriptionTableHeader*>(
            entry->getData());
    }

    std::size_t size() {
        std::uint32_t length =
            this->SDTH.Length - sizeof(SystemDescriptionTableHeader);
        return length / sizeof(Byte8AlignedBy4);
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