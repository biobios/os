#pragma once
#include <stdint.h>

#define TRUE 1
#define FALSE 0

#define EFI_SUCCESS 0

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL 0x00000002
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL 0x00000004
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x00000008
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x00000010
#define EFI_OPEN_PROTOCOL_EXCLUSIVE 0x00000020

#define EFI_ACPI_TABLE_GUID \
{0x8868e871,0xe4f1,0x11d3,\
{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}}

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;


typedef uint8_t BOOLEAN;
typedef int64_t INTN;
typedef uint64_t UINTN;
typedef int8_t INT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef uint8_t CHAR8;
typedef uint16_t CHAR16;
typedef UINTN EFI_STATUS;
typedef void* EFI_HANDLE;
typedef void* EFI_EVENT;
typedef UINT64 EFI_LBA;
typedef UINTN EFI_TPL;

typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

typedef struct
{
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct
{
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct _EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct _EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct _EFI_DEVICE_PATH_PROTOCOL EFI_DEVICE_PATH_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct
{
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalRsolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;


typedef struct
{
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
{0x9042a9de,0x23dc,0x4a38,\
{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL
{
    unsigned long long _buf[3];
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
};

typedef struct
{
    UINT16 Year;
    UINT8 Month;
    UINT8 Day;
    UINT8 Hour;
    UINT8 Minute;
    UINT8 Second;
    UINT8 Pad1;
    UINT32 Nanosecond;
    INT16 TimeZone;
    UINT8 Daylight;
    UINT8 Pad2;
} EFI_TIME;

typedef struct
{
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_FILE_INFO_ID \
{0x09576e92,0x6d3f,0x11d2,\
{0x8e, 0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
typedef struct
{
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[];
} EFI_FILE_INFO;

//******************************************************
// Open Modes
//******************************************************
#define EFI_FILE_MODE_READ 0x0000000000000001
#define EFI_FILE_MODE_WRITE 0x0000000000000002
#define EFI_FILE_MODE_CREATE 0x8000000000000000
//******************************************************
// File Attributes
//******************************************************
#define EFI_FILE_READ_ONLY 0x0000000000000001
#define EFI_FILE_HIDDEN 0x0000000000000002
#define EFI_FILE_SYSTEM 0x0000000000000004
#define EFI_FILE_RESERVED 0x0000000000000008
#define EFI_FILE_DIRECTORY 0x0000000000000010
#define EFI_FILE_ARCHIVE 0x0000000000000020
#define EFI_FILE_VALID_ATTR 0x0000000000000037
typedef EFI_STATUS (*EFI_FILE_OPEN)(EFI_FILE_PROTOCOL* This, EFI_FILE_PROTOCOL** NewHandle, CHAR16* FileName, UINT64 OpenMode, UINT64 Attributes);
typedef EFI_STATUS (*EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL* This);
typedef EFI_STATUS (*EFI_FILE_DELETE)(EFI_FILE_PROTOCOL* This);
typedef EFI_STATUS (*EFI_FILE_READ)(EFI_FILE_PROTOCOL* This, UINTN* BufferSize, void* Buffer);
typedef EFI_STATUS (*EFI_FILE_GET_INFO)(EFI_FILE_PROTOCOL* This, EFI_GUID* InfomationType, UINTN* BufferSize, void* Buffer);

struct _EFI_FILE_PROTOCOL
{
    UINT64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    EFI_FILE_DELETE Delete;
    EFI_FILE_READ Read;
    unsigned long long _buf[3];
    EFI_FILE_GET_INFO GetInfo;
};

typedef EFI_STATUS (*EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL** Root);

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
{0x0964e5b22,0x6459,0x11d2,\
{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
{
    UINT64 Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
};


typedef EFI_DEVICE_PATH_PROTOCOL* (*EFI_DEVICE_PATH_UTILS_APPEND_NODE)(const EFI_DEVICE_PATH_PROTOCOL* DevicePath, const EFI_DEVICE_PATH_PROTOCOL* DeviceNode);

#define EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID \
{0x379be4e,0xd706,0x437d,\
{0xb0,0x37,0xed,0xb8,0x2f,0xb7,0x72,0xa4 }}
typedef struct
{
    unsigned long long _buf[3];
    EFI_DEVICE_PATH_UTILS_APPEND_NODE AppendDeviceNode;
} EFI_DEVICE_PATH_UTILITIES_PROTOCOL;

typedef EFI_DEVICE_PATH_PROTOCOL* (*EFI_DEVICE_PATH_FROM_TEXT_NODE)(const CHAR16* TextDeviceNode);

#define EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL_GUID \
{0x5c99a21,0xc70f,0x4ad2,\
{0x8a,0x5f,0x35,0xdf,0x33,0x43,0xf5, 0x1e}}
typedef struct
{
    EFI_DEVICE_PATH_FROM_TEXT_NODE ConvertTextToDeviceNode;
} EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL;

typedef EFI_STATUS (*EFI_IMAGE_UNLOAD)(EFI_HANDLE ImageHandle);

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
{0x5B1B31A1,0x9562,0x11d2,\
{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
typedef struct
{
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL* FilePath;
    void* Reserved;
    UINT32 LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_IMAGE_UNLOAD Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
{0x09576e91,0x6d3f,0x11d2,\
{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
struct _EFI_DEVICE_PATH_PROTOCOL
{
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];
};

typedef CHAR16* (*EFI_DEVICE_PATH_TO_TEXT_NODE)(const EFI_DEVICE_PATH_PROTOCOL* DeviceNode, BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts);
typedef CHAR16* (*EFI_DEVICE_PATH_TO_TEXT_PATH)(const EFI_DEVICE_PATH_PROTOCOL* DevicePath, BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts);

#define EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID \
{0x8b843e20,0x8132,0x4852,\
{0x90,0xcc,0x55,0x1a,0x4e,0x4a,0x7f,0x1c}}
typedef struct
{
    EFI_DEVICE_PATH_TO_TEXT_NODE ConvertDeviceNodeToText;
    EFI_DEVICE_PATH_TO_TEXT_PATH ConvertDevicePathToText;
} EFI_DEVICE_PATH_TO_TEXT_PROTOCOL;

typedef EFI_STATUS (*EFI_INPUT_READ_KEY)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL* This, EFI_INPUT_KEY* Key);

struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL
{
    unsigned long long _buf;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    EFI_EVENT WaitForKey;
};


typedef EFI_STATUS (*EFI_TEXT_RESET)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (*EFI_TEXT_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, CHAR16* String);
typedef EFI_STATUS (*EFI_TEXT_CLEAR_SCREEN)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
{
    EFI_TEXT_RESET Reset;
    EFI_TEXT_STRING OutputString;
    unsigned long long _buf[4];
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
};

typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS* Memory);
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(UINTN* MemoryMapSize, EFI_MEMORY_DESCRIPTOR* MemoryMap, UINTN* MapKey, UINTN* DescriptorSize, UINT32* DescriptorVersion);
typedef EFI_STATUS (*EFI_WAIT_FOR_EVENT)(UINTN NumberOfvents, EFI_EVENT* Event, UINTN* Index);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (*EFI_SET_WATCHDOG_TIMER)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16* WatchdogData);
typedef EFI_STATUS (*EFI_OPEN_PROTOCOL)(EFI_HANDLE Handle, EFI_GUID* Protocol, void** Interface, EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, UINT32 Attributes);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(EFI_GUID* Protocol, void* Registration, void** Interface);
typedef EFI_STATUS (*EFI_SET_MEM)(void* Buffer, UINTN Size, UINT8 Value);

struct _EFI_BOOT_SERVICES
{
    char _buf1[24];
    unsigned long long _buf2[2];
    EFI_ALLOCATE_PAGES AllocatePages;
    unsigned long long _buf3[1];
    EFI_GET_MEMORY_MAP GetMemoryMap;
    unsigned long long _buf3_2[2];
    unsigned long long _buf4[2];
    EFI_WAIT_FOR_EVENT WaitForEvent;
    unsigned long long _buf4_2[3];
    unsigned long long _buf5[9];
    unsigned long long _buf6[4];
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    unsigned long long _buf7[2];
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
    unsigned long long _buf8[2];
    EFI_OPEN_PROTOCOL OpenProtocol;
    unsigned long long _buf9[2];
    unsigned long long _buf10[2];
    EFI_LOCATE_PROTOCOL LocateProtocol;
    unsigned long long _buf10_2[2];
    unsigned long long _buf11;
    unsigned long long _buf12;
    EFI_SET_MEM SetMem;
};

typedef struct{
    EFI_GUID VendorGuid;
    void* VendorTable;
} EFI_CONFIGURATION_TABLE;

struct _EFI_SYSTEM_TABLE
{
    char _buf[44];
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL* ConIn;
    unsigned long long _buf1;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    unsigned long long _buf2[3];
    EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE* ConfigurationTable;
};