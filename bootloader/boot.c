#include "uefi.h"
#include "elf.h"

#define FILE_INFO_BUF_SIZE 180
#define MEM_DESC_SIZE 0x8000

typedef struct
{
    uint64_t frame_buffer_base;
    uint64_t frame_buffer_size;
    uint32_t frame_buffer_horizontal;
    uint32_t frame_buffer_vertical;
    uint64_t RootSytemDescriptionPointer;
} PlatformInfo;

int equalsEFI_GUID(EFI_GUID* guid1, EFI_GUID* guid2){
    if((guid1->Data1 == guid2->Data1) 
        && (guid1->Data2 == guid2->Data2)
        && (guid1->Data3 == guid2->Data3)){
            unsigned char is_equal = TRUE;
            unsigned char j;
            for(j = 0; j < 8; j++){
                if(guid1->Data4[j] != guid2->Data4[j]){
                    is_equal = FALSE;
                    break;
                }
            }
            if(is_equal)return TRUE;
    }
    return FALSE;
}

unsigned char mem_desc[MEM_DESC_SIZE];

EFI_STATUS uefi_main(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE* systemTable)
{

    EFI_STATUS status;
    //ウォッチドッグタイマーをオフにする
    systemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    systemTable->ConOut->ClearScreen(systemTable->ConOut);
    systemTable->ConOut->OutputString(systemTable->ConOut, L"Hello UEFI!\n\r");

    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;

    //カーネルに伝える情報を収集する
    PlatformInfo info;
    status = systemTable->BootServices->LocateProtocol(&gop_guid, NULL, (void**)&gop);

    info.frame_buffer_base = gop->Mode->FrameBufferBase;
    info.frame_buffer_size = gop->Mode->FrameBufferSize;
    info.frame_buffer_horizontal = gop->Mode->Info->HorizontalResolution;
    info.frame_buffer_vertical = gop->Mode->Info->VerticalRsolution;

    EFI_GUID acpi_guid = EFI_ACPI_TABLE_GUID;
    EFI_CONFIGURATION_TABLE* configTable = systemTable->ConfigurationTable;
    UINTN configTableElementCount = systemTable->NumberOfTableEntries;
    void* rsdp = NULL;
    UINTN i;
    for(i = 0; i < configTableElementCount; i++, configTable++){
        if(equalsEFI_GUID(&(configTable->VendorGuid), &acpi_guid)){
            rsdp = configTable->VendorTable;
            break;
        }
    }
    if(rsdp == NULL){
        systemTable->ConOut->OutputString(systemTable->ConOut, L"RSDP not found!\n\r");
    }else{
        info.RootSytemDescriptionPointer = (uint64_t)rsdp;
    }

    //カーネルファイルを探す
    EFI_GUID sfsp_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfsp;
    
    status = systemTable->BootServices->LocateProtocol(&sfsp_guid, NULL, (void**)&sfsp);

    EFI_FILE_PROTOCOL* root;
    EFI_FILE_PROTOCOL* os_dir;
    EFI_FILE_PROTOCOL* kernel_file;

    sfsp->OpenVolume(sfsp, &root);
    status = root->Open(root, &os_dir, L"os", EFI_FILE_MODE_READ, 0);
    if(status != EFI_SUCCESS){
        systemTable->ConOut->OutputString(systemTable->ConOut, L"os Dir is not found!\n\r");
        while(1)__asm__ volatile("hlt");
    }
    status = os_dir->Open(os_dir, &kernel_file, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if(status != EFI_SUCCESS){
        systemTable->ConOut->OutputString(systemTable->ConOut, L"kernel.bin is not found!\n\r");
        while(TRUE)__asm__ volatile("hlt");
    }

    UINTN file_info_size = FILE_INFO_BUF_SIZE;
    UINTN fi_buf[FILE_INFO_BUF_SIZE];
    EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    EFI_FILE_INFO* fi_ptr;

    status = kernel_file->GetInfo(kernel_file, &fi_guid, &file_info_size, fi_buf);
    fi_ptr = (EFI_FILE_INFO*)fi_buf;
    UINT64 KernelSize = fi_ptr->FileSize;

    //バッファにカーネルをロードする
    void* loaderBuffer;
    status = systemTable->BootServices->AllocatePool(EfiLoaderData, KernelSize, &loaderBuffer);
    if(status != EFI_SUCCESS){
        systemTable->ConOut->OutputString(systemTable->ConOut, L"Buffer can't be allocated!\n\r");
        while(TRUE)__asm__ volatile("hlt");
    }
    status = kernel_file->Read(kernel_file, &KernelSize, loaderBuffer);
    if(status != EFI_SUCCESS){
        systemTable->ConOut->OutputString(systemTable->ConOut, L"Failed To Load Kernel!\n\r");
        while(TRUE)__asm__ volatile("hlt");
    }

    kernel_file->Close(kernel_file);

    //ProgramHeaderをもとにメモリを確保しバッファから移す
    Elf64_Ehdr* efiHeader = (Elf64_Ehdr*)loaderBuffer;
    Elf64_Phdr* progHeader = (Elf64_Phdr*)((char*)efiHeader + efiHeader->e_phoff);
    for(Elf64_Half i = 0; i < efiHeader->e_phnum; i++){
        if(progHeader[i].p_type != PT_LOAD)continue;

        UINT64 allocateSize = (progHeader[i].p_memsz + 0xfff) / 0x1000;//Page数に換算
        void* allocateAddress = (void*)(progHeader[i].p_vaddr & ~0xfff);//Page境界にAlignment

        status = systemTable->BootServices->AllocatePages(
            AllocateAddress,
            EfiLoaderData,
            allocateSize,
            (void*)&allocateAddress);

        if(status != EFI_SUCCESS){
            systemTable->ConOut->OutputString(systemTable->ConOut, L"Failed To Allocate Memory For Kernel!\n\r");
            while(TRUE)__asm__ volatile("hlt");
        }

        systemTable->BootServices->CopyMem((void*)progHeader[i].p_vaddr, loaderBuffer + progHeader[i].p_offset, progHeader[i].p_filesz);
        //確保するメモリ領域のうちファイルにデータのない部分は０クリアする
        UINTN clearSize = progHeader[i].p_memsz - progHeader[i].p_filesz;
        systemTable->BootServices->SetMem((void*)(progHeader[i].p_vaddr + progHeader[i].p_filesz), clearSize, 0);
    }

    //ブートサービスを終了する
    unsigned long long mem_desc_num;
    unsigned long long mem_desc_unit_size;
    unsigned long long map_key;
    unsigned long long mmap_size;
    unsigned int desc_ver;


    mmap_size = MEM_DESC_SIZE;
    status = systemTable->BootServices->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR*)mem_desc, &map_key, &mem_desc_unit_size, &desc_ver);

    if(status != EFI_SUCCESS){
        systemTable->ConOut->OutputString(systemTable->ConOut, L"Failed Get Memory Map\n\r");
        while(TRUE)__asm__ volatile("hlt");
    }
    
    status = systemTable->BootServices->ExitBootServices(imageHandle, map_key);
    if(status != EFI_SUCCESS){
        systemTable->ConOut->OutputString(systemTable->ConOut, L"Failed Exit Boot\n\r");
        while(TRUE)__asm__ volatile("hlt");
    }

    //カーネルのエントリポイントへジャンプする
    //UEFIの呼び出し規約ms_abi
    //Kernelの呼び出し規約SystemV ABI
    unsigned long long arg1 = (unsigned long long)&info;
    unsigned long long _sb = 0x0000000000210000;
    unsigned long long _ep = efiHeader->e_entry;
    __asm__ volatile("   mov %0, %%rdi\n"
            "   mov %1, %%rsp\n"
            "   jmp *%2\n"
            ::"m"(arg1), "m"(_sb), "m"(_ep));
    
    while(1);
    return EFI_SUCCESS;
}