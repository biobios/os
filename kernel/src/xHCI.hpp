#pragma once

#include <cstdint>

namespace xHCI {
struct SlotContext {
    std::uint32_t Offset00h;
    std::uint32_t Offset04h;
    std::uint32_t Offset08h;
    std::uint32_t Offset0Ch;
    std::uint32_t RsvdO[4];
};

struct EndpointContext {
    std::uint8_t EPState;
    std::uint8_t Offset01h;
    std::uint8_t Interval;
    std::uint8_t MaxESITPayloadHi;
    std::uint8_t Offset04h;
    std::uint8_t MaxBurstSize;
    std::uint16_t MaxPacketSize;
    std::uint32_t TRDequeuePointerLo;  // 0bit : DCS
    std::uint32_t TRDequeuePointerHi;
    std::uint16_t AverageTRBLength;
    std::uint16_t MaxESITPayloadLo;
    std::uint32_t RsvdO[3];
};

struct DeviceContext {
    SlotContext slotContext;
    EndpointContext endpointContext[31];
};

struct CapabilityRegisters {
    std::uint8_t capLength;
    std::uint8_t reserved;
    std::uint16_t hciVersion;
    std::uint32_t hcsParams1;
    std::uint32_t hcsParams2;
    std::uint32_t hcsParams3;
    std::uint32_t hccParams1;
    std::uint32_t dboff;
    std::uint32_t rtsoff;
    std::uint32_t hccParams2;
};

struct HostControllerUSBPortRegisterSet {
    std::uint32_t PortStatusAndControl;
    std::uint32_t PortPowerManagementStatusAndControl;
    std::uint32_t PortLinkInfo;
    std::uint32_t PortHardwareLPMControl;
};

struct OperationalRegisters {
    std::uint32_t usbCommand;
    std::uint32_t usbStatus;
    std::uint32_t pageSize;
    std::uint8_t RsvdZ1[0x14 - 0x0c];
    std::uint32_t deviceNotificationControl;
    std::uint64_t cmdRingControl;
    std::uint8_t reserved[0x10];
    std::uint64_t deviceContextBaseAddressArrayPointer;
    std::uint32_t configure;
    std::uint8_t reserved2[0x400 - 0x3c];
    HostControllerUSBPortRegisterSet ports[1];
};

namespace USBStatus {
constexpr std::uint32_t HostControllerHalted = 1 << 0;
constexpr std::uint32_t HostSystemError = 1 << 2;
constexpr std::uint32_t EventInterrupt = 1 << 3;
constexpr std::uint32_t PortChangeDetect = 1 << 4;
constexpr std::uint32_t SaveStateStatus = 1 << 8;
constexpr std::uint32_t RestoreStateStatus = 1 << 9;
constexpr std::uint32_t SaveRestoreError = 1 << 10;
constexpr std::uint32_t ControllerNotReady = 1 << 11;
constexpr std::uint32_t HostControllerError = 1 << 12;
}  // namespace USBStatus

namespace USBCommand {
constexpr std::uint32_t RunStop = 1 << 0;
constexpr std::uint32_t HostControllerReset = 1 << 1;
constexpr std::uint32_t InterrupterEnable = 1 << 2;
constexpr std::uint32_t HostSystemErrorEnable = 1 << 3;
constexpr std::uint32_t LightHostControllerReset = 1 << 7;
}  // namespace USBCommand

namespace Configure {
constexpr std::uint32_t MaxDeviceSlots = 0b11111111;
}

struct InterrupterRegisterSet {
    std::uint32_t InterrupterManagement;
    std::uint16_t InterrupterModerationInterval;
    std::uint16_t InterrupterModerationCounter;
    std::uint16_t EventRingSegmentTableSize;
    std::uint16_t RsvdP1;
    std::uint32_t RsvdP2;
    std::uint64_t EventRingSegmentTableBaseAddress;
    /// @brief
    /// ソフトウェアが処理した最後のイベントの位置を書き込むことで、xHCに通知する
    std::uint64_t EventRingDequeuePointer;
};

namespace IMAN {
constexpr std::uint32_t InterruptPending = 1 << 0;
constexpr std::uint32_t InterruptEnable = 1 << 1;
}  // namespace IMAN

struct RuntimeRegisters {
    std::uint32_t MicroframeIndex;
    std::uint8_t reserved[0x20 - 0x04];
    InterrupterRegisterSet IR[1024];
};

struct DoorbellRegister {
    std::uint8_t DBTarget;
    std::uint8_t reserved;
    std::uint16_t DBTaskID;
};

struct EventRingSegmentTableEntry {
    std::uint32_t RingSegmentBaseAddressLo;
    std::uint32_t RingSegmentBaseAddressHi;
    std::uint16_t RingSegmentSize;
    std::uint16_t reserved;
    std::uint32_t reserved2;
};

namespace TRB {
struct Dummy {
    std::uint8_t dummy[16];
};

struct Normal {
    std::uint64_t DataBufferPointer;
    std::uint16_t TRBTransferLengthLo;
    std::uint16_t InterrupterTarget_TDSize_TRBTransferLengthHi;
    std::uint32_t TRBControl;
};

struct SetupStage {
    std::uint8_t bmRequestType;
    std::uint8_t bRequest;
    std::uint16_t wValue;
    std::uint16_t wIndex;
    std::uint16_t wLength;
    std::uint16_t TRBTransferLengthLo;
    std::uint16_t InterrupterTarget_TRBTransferLengthHi;
    std::uint32_t TRBControl;
};

struct DataStage {
    std::uint64_t DataBuffer;
    std::uint16_t TRBTransferLengthLo;
    std::uint16_t InterrupterTarget_TDSize_TRBTransferLengthHi;
    std::uint32_t TRBControl;
};

struct StatusStage {
    std::uint8_t reserved[10];
    std::uint16_t InterrupterTarget;
    std::uint32_t TRBControl;
};

struct Isoch {
    std::uint64_t DataBufferPointer;
    std::uint16_t TRBTransferLengthLo;
    std::uint16_t InterrupterTarget_TDSizeOrTBC_TRBTransferLengthHi;
    std::uint32_t TRBControl;
};

struct NoOp {
    std::uint8_t reserved[10];
    std::uint16_t InterrupterTarget;
    std::uint32_t TRBControl;
};

struct TransferEvent {
    std::uint64_t TRBPointer;
    std::uint16_t TRBTransferLengthLo;
    std::uint8_t TRBTransferLengthHi;
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct CommandCompletionEvent {
    std::uint64_t CommandTRBPointer;
    std::uint16_t CommandCompletionParameterLo;
    std::uint8_t CommandCompletionParameterHi;
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct PortStatusChangeEvent {
    std::uint8_t reserved[3];
    std::uint8_t PortID;
    std::uint8_t reserved2[7];
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct BandwidthRequestEvent {
    std::uint8_t reserved[11];
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct DoorbellEvent {
    std::uint8_t DBReason;
    std::uint8_t reserved[10];
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct HostControllerEvent {
    std::uint8_t reserved[11];
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct DeviceNotificationEvent {
    std::uint32_t DeviceNotificationLo;
    std::uint32_t DeviceNotificationHi;
    std::uint8_t reserved[3];
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct MFINDEXWrapEvent {
    std::uint8_t reserved[11];
    std::uint8_t CompletionCode;
    std::uint32_t TRBControl;
};

struct NoOpCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct EnableSlotCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct DisableSlotCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct AddressDeviceCommand {
    std::uint64_t InputContextPointer;
    std::uint8_t reserved[4];
    std::uint32_t TRBControl;
};

struct ConfigureEndpointCommand {
    std::uint64_t InputContextPointer;
    std::uint8_t reserved[4];
    std::uint32_t TRBControl;
};

struct EvaluateContextCommand {
    std::uint64_t InputContextPointer;
    std::uint8_t reserved[4];
    std::uint32_t TRBControl;
};

struct ResetEndpointCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct StopEndpointCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct SetTRDequeuePointerCommand {
    std::uint64_t NewTRDequeuePointer;
    std::uint16_t reserved;
    std::uint16_t StreamID;
    std::uint32_t TRBControl;
};

struct ResetDeviceCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct ForceEventCommand {
    std::uint64_t EventTRBPointer;
    std::uint32_t VFInterrupterTarget;
    std::uint32_t TRBControl;
};

struct NegotiateBandwidthCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct SetLatencyToleranceValueCommand {
    std::uint8_t reserved[12];
    std::uint32_t TRBControl;
};

struct GetPortBandwidthCommand {
    std::uint64_t PortBandwidthContextPointer;
    std::uint32_t reserved;
    std::uint32_t TRBControl;
};

struct ForceHeaderCommand {
    std::uint32_t HeaderInfoLo_Type;
    std::uint32_t HeaderInfoMid;
    std::uint32_t HeaderInfoHi;
    std::uint32_t TRBControl;
};

struct GetExtendedPropertyCommand {
    std::uint64_t ExtendedPropertyContextPointer;
    std::uint16_t ExtendedCapabilityIdentifier;
    std::uint16_t reserved;
    std::uint32_t TRBControl;
};

struct SetExtendedPropertyCommand {
    std::uint8_t reserved[8];
    std::uint16_t ExtendedCapabilityIdentifier;
    std::uint8_t CapabilityParameter;
    std::uint8_t reserved2;
    std::uint32_t TRBControl;
};

struct Link {
    std::uint64_t RingSegmentPointer;
    std::uint16_t reserved;
    std::uint16_t InterrupterTarget;
    std::uint32_t TRBControl;
};

struct EventData {
    std::uint64_t EventData;
    std::uint16_t reserved;
    std::uint16_t InterrupterTarget;
    std::uint32_t TRBControl;
};
}  // namespace TRB
}  // namespace xHCI