//////////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 2004- Tactrix Inc.
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

// "OP2.0:" refers to items that may have special relevance to the Tactrix OpenPort 2.0

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif

/////////////////
// API Functions
/////////////////

#ifdef OP20PT32_LIB
#define PT_CALL
#define PT_API EXTERN_C
#else
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#define PT_CALL __stdcall
#else
#define PT_CALL
#endif
#define PT_API EXTERN_C
#endif

typedef long(PT_CALL PF_PassThruOpen)(const void *, unsigned long *);
typedef long(PT_CALL PF_PassThruClose)(unsigned long);
typedef long(PT_CALL PF_PassThruConnect)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
typedef long(PT_CALL PF_PassThruDisconnect)(unsigned long);
typedef long(PT_CALL PF_PassThruReadMsgs)(unsigned long, void *, unsigned long *, unsigned long);
typedef long(PT_CALL PF_PassThruWriteMsgs)(unsigned long, const void *, unsigned long *, unsigned long);
typedef long(PT_CALL PF_PassThruStartPeriodicMsg)(unsigned long, const void *, unsigned long *, unsigned long);
typedef long(PT_CALL PF_PassThruStopPeriodicMsg)(unsigned long, unsigned long);
typedef long(PT_CALL PF_PassThruStartMsgFilter)(unsigned long, unsigned long, const void *, const void *, const void *,
                                                unsigned long *);
typedef long(PT_CALL PF_PassThruStopMsgFilter)(unsigned long, unsigned long);
typedef long(PT_CALL PF_PassThruSetProgrammingVoltage)(unsigned long, unsigned long, unsigned long);
typedef long(PT_CALL PF_PassThruReadVersion)(unsigned long, char *, char *, char *);
typedef long(PT_CALL PF_PassThruGetLastError)(char *);
typedef long(PT_CALL PF_PassThruIoctl)(unsigned long, unsigned long, const void *, void *);

typedef void (*PF_StatusCallback)(const char *, int, int);

////////////////
// Protocol IDs
////////////////

// J2534-1
inline constexpr int kJ2534J1850Vpw = 0x01;
inline constexpr int kJ2534J1850Pwm = 0x02;
inline constexpr int kJ2534Iso9141 = 0x03;
inline constexpr int kJ2534Iso14230 = 0x04;
inline constexpr int kJ2534Can = 0x05;
inline constexpr int kJ2534Iso15765 = 0x06;
inline constexpr int kJ2534SciAEngine = 0x07;
inline constexpr int kJ2534SciATrans = 0x08;
inline constexpr int kJ2534SciBEngine = 0x09;
inline constexpr int kJ2534SciBTrans = 0x0A;

// J2534-2
inline constexpr int kJ2534CanCh1 = 0x00009000;
inline constexpr int kJ2534J1850VpwCh1 = 0x00009080;
inline constexpr int kJ2534J1850PwmCh1 = 0x00009160;
inline constexpr int kJ2534Iso9141Ch1 = 0x00009240;
inline constexpr int kJ2534Iso9141Ch2 = 0x00009241;
inline constexpr int kJ2534Iso9141Ch3 = 0x00009242;
inline constexpr int kJ2534Iso9141K = kJ2534Iso9141Ch1;
inline constexpr int kJ2534Iso9141L = kJ2534Iso9141Ch2; // OP2.0: Support for ISO9141 communications over the L line
inline constexpr int kJ2534Iso9141Inno =
    kJ2534Iso9141Ch3; // OP2.0: Support for RS-232 receive-only communications via the 2.5mm jack
inline constexpr int kJ2534Iso14230Ch1 = 0x00009320;
inline constexpr int kJ2534Iso14230Ch2 = 0x00009321;
inline constexpr int kJ2534Iso14230K = kJ2534Iso14230Ch1;
inline constexpr int kJ2534Iso14230L = kJ2534Iso14230Ch2; // OP2.0: Support for ISO14230 communications over the L line
inline constexpr int kJ2534Iso15765Ch1 = 0x00009400;

// J2534 device specific protocols
inline constexpr int kJ2534DstiIso9141 = 0x00020001;

/////////////
// IOCTL IDs
/////////////

// J2534-1
inline constexpr int kJ2534GetConfig = 0x01;                     // SCONFIG_LIST		NULL
inline constexpr int kJ2534SetConfig = 0x02;                     // SCONFIG_LIST		NULL
inline constexpr int kJ2534ReadVbatt = 0x03;                     // NULL			unsigned long
inline constexpr int kJ2534FiveBaudInit = 0x04;                  // SBYTE_ARRAY		SBYTE_ARRAY
inline constexpr int kJ2534FastInit = 0x05;                      // PASSTHRU_MSG		PASSTHRU_MSG
inline constexpr int kJ2534ClearTxBuffer = 0x07;                 // NULL			NULL
inline constexpr int kJ2534ClearRxBuffer = 0x08;                 // NULL			NULL
inline constexpr int kJ2534ClearPeriodicMsgs = 0x09;             // NULL			NULL
inline constexpr int kJ2534ClearMsgFilters = 0x0A;               // NULL			NULL
inline constexpr int kJ2534ClearFunctMsgLookupTable = 0x0B;      // NULL			NULL
inline constexpr int kJ2534AddToFunctMsgLookupTable = 0x0C;      // SBYTE_ARRAY		NULL
inline constexpr int kJ2534DeleteFromFunctMsgLookupTable = 0x0D; // SBYTE_ARRAY		NULL
inline constexpr int kJ2534ReadProgVoltage = 0x0E;               // NULL			unsigned long

// J2534-2
inline constexpr int kJ2534SwCanNs = 0x8000; // OP2.0: Not supported
inline constexpr int kJ2534SwCanHs = 0x8001; // OP2.0: Not supported
/*
// Tactrix specific IOCTLs
#define TX_IOCTL_BASE							0x70000
// OP2.0: The IOCTL below supports application-specific functions
// that can be built into the hardware
#define TX_IOCTL_APP_SERVICE					(TX_IOCTL_BASE+0)
#define TX_IOCTL_SET_DLL_DEBUG_FLAGS			(TX_IOCTL_BASE+1)
#define TX_IOCTL_DLL_DEBUG_FLAG_J2534_CALLS		0x00000001
#define TX_IOCTL_DLL_DEBUG_FLAG_ALL_DEV_COMMS	0x00000002
#define TX_IOCTL_SET_DEV_DEBUG_FLAGS			(TX_IOCTL_BASE+2)
#define TX_IOCTL_DEV_DEBUG_FLAG_USB_COMMS		0x00000001
#define TX_IOCTL_SET_DLL_STATUS_CALLBACK		(TX_IOCTL_BASE+3)
#define TX_IOCTL_GET_DEVICE_INSTANCES    		(TX_IOCTL_BASE+4)
*/
/////////////////
// Pin numbering
/////////////////

inline constexpr int kJ2534AuxPin = 0;      // aux jack	OP2.0: Supports GND and adj. voltage
inline constexpr int kJ2534J1962Pin1 = 1;   //			OP2.0: Supports GND and adj. voltage
inline constexpr int kJ2534J1962Pin2 = 2;   // J1850P	OP2.0: Supports 5V and 8V
inline constexpr int kJ2534J1962Pin3 = 3;   //			OP2.0: Supports GND and adj. voltage
inline constexpr int kJ2534J1962Pin4 = 4;   // GND
inline constexpr int kJ2534J1962Pin5 = 5;   // GND
inline constexpr int kJ2534J1962Pin6 = 6;   // CAN
inline constexpr int kJ2534J1962Pin7 = 7;   // K		OP2.0: Supports GND
inline constexpr int kJ2534J1962Pin8 = 8;   //			OP2.0: Supports reading voltage
inline constexpr int kJ2534J1962Pin9 = 9;   //			OP2.0: Supports GND and adj. voltage
inline constexpr int kJ2534J1962Pin10 = 10; // J1850M	OP2.0: Supports GND
inline constexpr int kJ2534J1962Pin11 = 11; //			OP2.0: Supports GND and adj. voltage
inline constexpr int kJ2534J1962Pin12 = 12; //			OP2.0: Supports GND and adj. voltage
inline constexpr int kJ2534J1962Pin13 = 13; //			OP2.0: Supports GND and adj. voltage
inline constexpr int kJ2534J1962Pin14 = 14; // CAN
inline constexpr int kJ2534J1962Pin15 = 15; // L		OP2.0: Supports GND
inline constexpr int kJ2534J1962Pin16 = 16; // VBAT		OP2.0: Supports reading voltage
inline constexpr int kJ2534PinVadj = 17;    // internal	OP2.0: Supports reading voltage

////////////////////////////////
// Special pin voltage settings
////////////////////////////////

inline constexpr unsigned int kJ2534ShortToGround = 0xFFFFFFFE;
inline constexpr unsigned int kJ2534VoltageOff = 0xFFFFFFFF;

/////////////////////////////////////////
// GET_CONFIG / SET_CONFIG Parameter IDs
/////////////////////////////////////////

// J2534-1
inline constexpr int kJ2534DataRate = 0x01;
inline constexpr int kJ2534Loopback = 0x03;
inline constexpr int kJ2534NodeAddress = 0x04; // OP2.0: Not yet supported
inline constexpr int kJ2534NetworkLine = 0x05; // OP2.0: Not yet supported
inline constexpr int kJ2534P1Min = 0x06;       // J2534 says this may not be changed
inline constexpr int kJ2534P1Max = 0x07;
inline constexpr int kJ2534P2Min = 0x08; // J2534 says this may not be changed
inline constexpr int kJ2534P2Max = 0x09; // J2534 says this may not be changed
inline constexpr int kJ2534P3Min = 0x0A;
inline constexpr int kJ2534P3Max = 0x0B; // J2534 says this may not be changed
inline constexpr int kJ2534P4Min = 0x0C;
inline constexpr int kJ2534P4Max = 0x0D; // J2534 says this may not be changed
inline constexpr int kJ2534W0 = 0x19;
inline constexpr int kJ2534W1 = 0x0E;
inline constexpr int kJ2534W2 = 0x0F;
inline constexpr int kJ2534W3 = 0x10;
inline constexpr int kJ2534W4 = 0x11;
inline constexpr int kJ2534W5 = 0x12;
inline constexpr int kJ2534Tidle = 0x13;
inline constexpr int kJ2534Tinil = 0x14;
inline constexpr int kJ2534Twup = 0x15;
inline constexpr int kJ2534Parity = 0x16;
inline constexpr int kJ2534BitSamplePoint = 0x17; // OP2.0: Not yet supported
inline constexpr int kJ2534SyncJumpWidth = 0x18;  // OP2.0: Not yet supported
inline constexpr int kJ2534T1Max = 0x1A;
inline constexpr int kJ2534T2Max = 0x1B;
inline constexpr int kJ2534T3Max = 0x24;
inline constexpr int kJ2534T4Max = 0x1C;
inline constexpr int kJ2534T5Max = 0x1D;
inline constexpr int kJ2534Iso15765Bs = 0x1E;
inline constexpr int kJ2534Iso15765Stmin = 0x1F;
inline constexpr int kJ2534DataBits = 0x20;
inline constexpr int kJ2534FiveBaudMod = 0x21;
inline constexpr int kJ2534BsTx = 0x22;
inline constexpr int kJ2534StminTx = 0x23;
inline constexpr int kJ2534Iso15765WftMax = 0x25;

// J2534-2
inline constexpr int kJ2534CanMixedFormat = 0x8000;
inline constexpr int kJ2534J1962Pins = 0x8001;              // OP2.0: Not supported
inline constexpr int kJ2534SwCanHsDataRate = 0x8010;        // OP2.0: Not supported
inline constexpr int kJ2534SwCanSpeedchangeEnable = 0x8011; // OP2.0: Not supported
inline constexpr int kJ2534SwCanResSwitch = 0x8012;         // OP2.0: Not supported
inline constexpr int kJ2534ActiveChannels = 0x8020;         // OP2.0: Not supported
inline constexpr int kJ2534SampleRate = 0x8021;             // OP2.0: Not supported
inline constexpr int kJ2534SamplesPerReading = 0x8022;      // OP2.0: Not supported
inline constexpr int kJ2534ReadingsPerMsg = 0x8023;         // OP2.0: Not supported
inline constexpr int kJ2534AveragingMethod = 0x8024;        // OP2.0: Not supported
inline constexpr int kJ2534SampleResolution = 0x8025;       // OP2.0: Not supported
inline constexpr int kJ2534InputRangeLow = 0x8026;          // OP2.0: Not supported
inline constexpr int kJ2534InputRangeHigh = 0x8027;         // OP2.0: Not supported

// Tactrix specific parameter IDs
// #define TX_PARAM_BASE					0x9000
// #define TX_PARAM_STOP_BITS				(TX_PARAM_BASE_BASE+0)

//////////////////////
// PARITY definitions
//////////////////////

inline constexpr int kJ2534NoParity = 0;
inline constexpr int kJ2534OddParity = 1;
inline constexpr int kJ2534EvenParity = 2;

////////////////////////////////
// CAN_MIXED_FORMAT definitions
////////////////////////////////

inline constexpr int kJ2534CanMixedFormatOff = 0;
inline constexpr int kJ2534CanMixedFormatOn = 1;
inline constexpr int kJ2534CanMixedFormatAllFrames = 2;

/////////////
// Error IDs
/////////////

// J2534-1
inline constexpr int kJ2534ErrSuccess = 0x00;
inline constexpr int kJ2534StatusNoerror = 0x00;
inline constexpr int kJ2534ErrNotSupported = 0x01;
inline constexpr int kJ2534ErrInvalidChannelId = 0x02;
inline constexpr int kJ2534ErrInvalidProtocolId = 0x03;
inline constexpr int kJ2534ErrNullParameter = 0x04;
inline constexpr int kJ2534ErrInvalidIoctlValue = 0x05;
inline constexpr int kJ2534ErrInvalidFlags = 0x06;
inline constexpr int kJ2534ErrFailed = 0x07;
inline constexpr int kJ2534ErrDeviceNotConnected = 0x08;
inline constexpr int kJ2534ErrTimeout = 0x09;
inline constexpr int kJ2534ErrInvalidMsg = 0x0A;
inline constexpr int kJ2534ErrInvalidTimeInterval = 0x0B;
inline constexpr int kJ2534ErrExceededLimit = 0x0C;
inline constexpr int kJ2534ErrInvalidMsgId = 0x0D;
inline constexpr int kJ2534ErrDeviceInUse = 0x0E;
inline constexpr int kJ2534ErrInvalidIoctlId = 0x0F;
inline constexpr int kJ2534ErrBufferEmpty = 0x10;
inline constexpr int kJ2534ErrBufferFull = 0x11;
inline constexpr int kJ2534ErrBufferOverflow = 0x12;
inline constexpr int kJ2534ErrPinInvalid = 0x13;
inline constexpr int kJ2534ErrChannelInUse = 0x14;
inline constexpr int kJ2534ErrMsgProtocolId = 0x15;
inline constexpr int kJ2534ErrInvalidFilterId = 0x16;
inline constexpr int kJ2534ErrNoFlowControl = 0x17;
inline constexpr int kJ2534ErrNotUnique = 0x18;
inline constexpr int kJ2534ErrInvalidBaudrate = 0x19;
inline constexpr int kJ2534ErrInvalidDeviceId = 0x1A;

// OP2.0 Tactrix specific
// #define ERR_OEM_VOLTAGE_TOO_LOW				0x78 // OP2.0: the requested output voltage is lower than the OP2.0
// capabilities #define ERR_OEM_VOLTAGE_TOO_HIGH			0x77 // OP2.0: the requested output voltage is higher than
// the OP2.0 capabilities

/////////////////////////
// PassThruConnect flags
/////////////////////////

inline constexpr int kJ2534Can29BitId = 0x00000100;
inline constexpr int kJ2534Iso9141NoChecksum = 0x00000200;
inline constexpr int kJ2534CanIdBoth = 0x00000800;
inline constexpr int kJ2534Iso9141KLineOnly = 0x00001000;
// #define SNIFF_MODE							0x10000000 // OP2.0: listens to a bus (e.g. CAN) without acknowledging

//////////////////
// RxStatus flags
//////////////////

inline constexpr int kJ2534TxMsgType = 0x00000001;
inline constexpr int kJ2534StartOfMessage = 0x00000002;
inline constexpr int kJ2534Iso15765FirstFrame = 0x00000002;
inline constexpr int kJ2534RxBreak = 0x00000004;
inline constexpr int kJ2534TxDone = 0x00000008;
inline constexpr int kJ2534Iso15765PaddingError = 0x00000010;
inline constexpr int kJ2534Iso15765ExtAddr = 0x00000080;
inline constexpr int kJ2534Iso15765AddrType = 0x00000080;
// #define CAN_29BIT_ID						0x00000100 // (already defined above)

//////////////////
// TxStatus flags
//////////////////

inline constexpr int kJ2534Iso15765FramePad = 0x00000040;
// #define ISO15765_ADDR_TYPE				0x00000080 // (already defined above)
// #define CAN_29BIT_ID						0x00000100 // (already defined above)
inline constexpr int kJ2534WaitP3MinOnly = 0x00000200;
inline constexpr int kJ2534SwCanHvTx = 0x00000400;    // OP2.0: Not supported
inline constexpr int kJ2534SciMode = 0x00400000;      // OP2.0: Not supported
inline constexpr int kJ2534SciTxVoltage = 0x00800000; // OP2.0: Not supported

////////////////
// Filter types
////////////////

inline constexpr int kJ2534PassFilter = 0x00000001;
inline constexpr int kJ2534BlockFilter = 0x00000002;
inline constexpr int kJ2534FlowControlFilter = 0x00000003;

/////////////////
// Message struct
/////////////////

inline constexpr int kJ2534PassthruMsgDataSize = 4128;

typedef struct
{
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    // The J2534 API defines this struct's layout; the vendor DLL reads and
    // writes it in place, so the trailing payload stays a C array.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    unsigned char Data[kJ2534PassthruMsgDataSize];
} PASSTHRU_MSG;

////////////////
// IOCTL structs
////////////////

typedef struct
{
    unsigned long Parameter;
    unsigned long Value;
} SCONFIG;

typedef struct
{
    unsigned long NumOfParams;
    SCONFIG *ConfigPtr;
} SCONFIG_LIST;

typedef struct
{
    unsigned long NumOfBytes;
    unsigned char *BytePtr;
} SBYTE_ARRAY;
