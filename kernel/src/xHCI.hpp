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

struct InputControlContext {
    std::uint32_t DropContextFlags;
    std::uint32_t AddContextFlags;
    std::uint32_t RsvdZ1[5];
    std::uint8_t ConfigurationValue;
    std::uint8_t InterfaceNumber;
    std::uint8_t AlternateSetting;
    std::uint8_t RsvdZ2;
};

struct InputContext {
    InputControlContext inputControlContext;
    SlotContext slotContext;
    EndpointContext endpointContext[31];
};

enum class SlotState : std::uint8_t {
    DisabledOrEnabled = 0,
    Default = 1,
    Addressed = 2,
    Configured = 3,
};

enum class EndpointState : std::uint8_t {
    Disabled = 0,
    Running = 1,
    Halted = 2,
    Stopped = 3,
    Error = 4,
};

enum class CompletionCode : std::uint8_t {
    Invalid = 0,
    Success = 1,
    DataBufferError = 2,
    BabbleDetectedError = 3,
    USBTransactionError = 4,
    TRBError = 5,
    StallError = 6,
    ResourceError = 7,
    BandwidthError = 8,
    NoSlotsAvailableError = 9,
    InvalidStreamTypeError = 10,
    SlotNotEnabledError = 11,
    EndpointNotEnabledError = 12,
    ShortPacket = 13,
    RingUnderrun = 14,
    RingOverrun = 15,
    VFEventRingFullError = 16,
    ParameterError = 17,
    BandwidthOverrunError = 18,
    ContextStateError = 19,
    NoPingResponseError = 20,
    EventRingFullError = 21,
    IncompatibleDeviceError = 22,
    MissedServiceError = 23,
    CommandRingStopped = 24,
    CommandAborted = 25,
    Stopped = 26,
    StoppedLengthInvalid = 27,
    StoppedShortPacket = 28,
    MaxExitLatencyTooLargeError = 29,
    IsochBufferOverrun = 31,
    EventLostError = 32,
    UndefinedError = 33,
    InvalidStreamIDError = 34,
    SecondaryBandwidthError = 35,
    SplitTransactionError = 36,
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

namespace ExtendedCapability {
constexpr std::uint32_t USBLegacySupport = 1;
constexpr std::uint32_t SupportedProtocol = 2;
constexpr std::uint32_t ExtendedPowerManagement = 3;
constexpr std::uint32_t IOLocalization = 4;
constexpr std::uint32_t MessageInterrupt = 5;
constexpr std::uint32_t LocalMemory = 6;
constexpr std::uint32_t USBDIAC = 7;
constexpr std::uint32_t xHCIExtendedCapability = 10;
}  // namespace ExtendedCapability

struct HostControllerUSBPortRegisterSet {
    std::uint32_t PortStatusAndControl;
    std::uint32_t PortPowerManagementStatusAndControl;
    std::uint32_t PortLinkInfo;
    std::uint32_t PortHardwareLPMControl;
};

namespace PORTSC {
constexpr std::uint32_t CurrentConnectStatus = 1 << 0;
constexpr std::uint32_t PortEnabledDisabled = 1 << 1;
constexpr std::uint32_t PortReset = 1 << 4;
constexpr std::uint32_t PortLinkStateMask = 0b1111 << 5;
constexpr std::uint32_t PortPower = 1 << 9;
constexpr std::uint32_t PortSpeedMask = 0b1111 << 10;
constexpr std::uint32_t PortIndicatorControlMask = 0b11 << 14;
constexpr std::uint32_t PortLinkStateWriteStrobe = 1 << 16;
constexpr std::uint32_t ConnectStatusChange = 1 << 17;
constexpr std::uint32_t PortEnabledDisabledChange = 1 << 18;
constexpr std::uint32_t WarmPortResetChange = 1 << 19;
constexpr std::uint32_t OverCurrentChange = 1 << 20;
constexpr std::uint32_t PortResetChange = 1 << 21;
constexpr std::uint32_t PortLinkStateChange = 1 << 22;
constexpr std::uint32_t PortConfigErrorChange = 1 << 23;
constexpr std::uint32_t ColdAttachStatus = 1 << 24;
constexpr std::uint32_t WakeOnConnectEnable = 1 << 25;
constexpr std::uint32_t WakeOnDisconnectEnable = 1 << 26;
constexpr std::uint32_t WakeOnOverCurrentEnable = 1 << 27;
constexpr std::uint32_t DeviceRemovable = 1 << 30;
constexpr std::uint32_t WarmPortReset = 1u << 31;
}  // namespace PORTSC

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

enum class Type : std::uint8_t {
    Normal = 1,
    SetupStage = 2,
    DataStage = 3,
    StatusStage = 4,
    Isoch = 5,
    Link = 6,
    EventData = 7,
    NoOp = 8,
    EnableSlotCommand = 9,
    DisableSlotCommand = 10,
    AddressDeviceCommand = 11,
    ConfigureEndpointCommand = 12,
    EvaluateContextCommand = 13,
    ResetEndpointCommand = 14,
    StopEndpointCommand = 15,
    SetTRDequeuePointerCommand = 16,
    ResetDeviceCommand = 17,
    ForceEventCommand = 18,
    NegotiateBandwidthCommand = 19,
    SetLatencyToleranceValueCommand = 20,
    GetPortBandwidthCommand = 21,
    ForceHeaderCommand = 22,
    NoOpCommand = 23,
    GetExtendedPropertyCommand = 24,
    SetExtendedPropertyCommand = 25,
    TransferEvent = 32,
    CommandCompletionEvent = 33,
    PortStatusChangeEvent = 34,
    BandwidthRequestEvent = 35,
    DoorbellEvent = 36,
    HostControllerEvent = 37,
    DeviceNotificationEvent = 38,
    MFINDEXWrapEvent = 39,
};

union Any {
    std::uint32_t data[4];
    Dummy dummy;
    Normal normal;
    SetupStage setup_stage;
    DataStage data_stage;
    StatusStage status_stage;
    Isoch isoch;
    NoOp no_op;
    TransferEvent transfer_event;
    CommandCompletionEvent command_completion_event;
    PortStatusChangeEvent port_status_change_event;
    BandwidthRequestEvent bandwidth_request_event;
    DoorbellEvent doorbell_event;
    HostControllerEvent host_controller_event;
    DeviceNotificationEvent device_notification_event;
    MFINDEXWrapEvent mfindex_wrap_event;
    NoOpCommand no_op_command;
    EnableSlotCommand enable_slot_command;
    DisableSlotCommand disable_slot_command;
    AddressDeviceCommand address_device_command;
    ConfigureEndpointCommand configure_endpoint_command;
    EvaluateContextCommand evaluate_context_command;
    ResetEndpointCommand reset_endpoint_command;
    StopEndpointCommand stop_endpoint_command;
    SetTRDequeuePointerCommand set_tr_dequeue_pointer_command;
    ResetDeviceCommand reset_device_command;
    ForceEventCommand force_event_command;
    NegotiateBandwidthCommand negotiate_bandwidth_command;
    SetLatencyToleranceValueCommand set_latency_tolerance_value_command;
    GetPortBandwidthCommand get_port_bandwidth_command;
    ForceHeaderCommand force_header_command;
    GetExtendedPropertyCommand get_extended_property_command;
    SetExtendedPropertyCommand set_extended_property_command;
    Link link;
    EventData event_data;
};
}  // namespace TRB
}  // namespace xHCI