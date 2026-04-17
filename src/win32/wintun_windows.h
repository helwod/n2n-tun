/**
 * (C) 2024 - n2n-wintun integration
 *
 * wintun.h - Wintun network adapter API definitions
 *
 * Based on WireGuard's wintun project: https://git.zx2c4.com/wintun
 * Licensed under GPLv2
 */

#ifndef _WINTUN_WINDOWS_H_
#define _WINTUN_WINDOWS_H_

#ifdef _WIN32

#include <windows.h>

/* Define Windows SAL annotations if not available */
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _In_readable_bytes_
#define _In_readable_bytes_(x)
#endif
#ifndef _Out_writable_bytes_
#define _Out_writable_bytes_(x)
#endif
#ifndef _When_
#define _When_(x,y)
#endif
#ifndef _Outptr_result_bytebuffer_maybenull_
#define _Outptr_result_bytebuffer_maybenull_(x)
#endif
#ifndef _Outptr_result_bytebuffer_
#define _Outptr_result_bytebuffer_(x)
#endif

/**
 * Wintun API version - bump when API changes
 */
#define WINTUN_API_VERSION 1

/**
 * Maximum length of adapter pool name (including null terminator)
 */
#define WINTUN_MAX_POOL 256

/**
 * Minimum ring buffer capacity (128 KiB)
 */
#define WINTUN_MIN_RING_CAPACITY 0x20000

/**
 * Maximum ring buffer capacity (64 MiB)
 */
#define WINTUN_MAX_RING_CAPACITY 0x4000000

/**
 * Maximum IP packet size
 */
#define WINTUN_MAX_IP_PACKET_SIZE 0xFFFF

/**
 * Default ring buffer capacity
 */
#define WINTUN_RING_CAPACITY 0x400000

/**
 * Opaque adapter handle
 */
typedef void* WINTUN_ADAPTER_HANDLE;

/**
 * Opaque session handle
 */
typedef void* WINTUN_SESSION_HANDLE;

/**
 * Logger callback function type
 */
typedef void (WINAPI* WINTUN_LOGGER_CALLBACK)(
    _In_ int Level,
    _In_ const WCHAR* Message
);

/**
 * Logger levels
 */
typedef enum WINTUN_LOGGER_LEVEL {
    WINTUN_LOG_INFO,
    WINTUN_LOG_WARN,
    WINTUN_LOG_ERR
} WINTUN_LOGGER_LEVEL;

/**
 * Wintun DLL function pointer types
 * These must match the official wintun.dll exports exactly.
 * Official API: https://git.zx2c4.com/wintun/tree/api/wintun.h
 * Errors are retrieved via GetLastError(), NOT via output parameters.
 */

typedef WINTUN_ADAPTER_HANDLE (WINAPI* WINTUN_CREATE_ADAPTER_FUNC)(
    _In_ LPCWSTR Name,
    _In_ LPCWSTR TunnelType,
    _In_opt_ const GUID* RequestedGUID
);

typedef WINTUN_ADAPTER_HANDLE (WINAPI* WINTUN_OPEN_ADAPTER_FUNC)(
    _In_ LPCWSTR Name
);

typedef VOID (WINAPI* WINTUN_CLOSE_ADAPTER_FUNC)(
    _In_ WINTUN_ADAPTER_HANDLE Adapter
);

typedef BOOL (WINAPI* WINTUN_DELETE_DRIVER_FUNC)(VOID);

typedef DWORD (WINAPI* WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC)(VOID);

typedef VOID (WINAPI* WINTUN_SET_LOGGER_FUNC)(
    _In_ WINTUN_LOGGER_CALLBACK Callback
);

typedef WINTUN_SESSION_HANDLE (WINAPI* WINTUN_START_SESSION_FUNC)(
    _In_ WINTUN_ADAPTER_HANDLE Adapter,
    _In_ DWORD Capacity
);

typedef VOID (WINAPI* WINTUN_END_SESSION_FUNC)(
    _In_ WINTUN_SESSION_HANDLE Session
);

typedef HANDLE (WINAPI* WINTUN_GET_READ_WAIT_EVENT_FUNC)(
    _In_ WINTUN_SESSION_HANDLE Session
);

typedef BYTE* (WINAPI* WINTUN_RECEIVE_PACKET_FUNC)(
    _In_ WINTUN_SESSION_HANDLE Session,
    _Out_ DWORD* PacketSize
);

typedef VOID (WINAPI* WINTUN_RELEASE_RECEIVE_PACKET_FUNC)(
    _In_ WINTUN_SESSION_HANDLE Session,
    _In_ const BYTE* Packet
);

typedef BYTE* (WINAPI* WINTUN_ALLOCATE_SEND_PACKET_FUNC)(
    _In_ WINTUN_SESSION_HANDLE Session,
    _In_ DWORD PacketSize
);

typedef VOID (WINAPI* WINTUN_SEND_PACKET_FUNC)(
    _In_ WINTUN_SESSION_HANDLE Session,
    _In_ const BYTE* Packet
);

typedef VOID (WINAPI* WINTUN_GET_ADAPTER_LUID_FUNC)(
    _In_ WINTUN_ADAPTER_HANDLE Adapter,
    _Out_ NET_LUID* Luid
);

/**
 * Wintun function table
 */
typedef struct _WINTUN_FUNCTIONS {
    WINTUN_CREATE_ADAPTER_FUNC WintunCreateAdapter;
    WINTUN_OPEN_ADAPTER_FUNC WintunOpenAdapter;
    WINTUN_CLOSE_ADAPTER_FUNC WintunCloseAdapter;
    WINTUN_DELETE_DRIVER_FUNC WintunDeleteDriver;
    WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC WintunGetRunningDriverVersion;
    WINTUN_SET_LOGGER_FUNC WintunSetLogger;
    WINTUN_START_SESSION_FUNC WintunStartSession;
    WINTUN_END_SESSION_FUNC WintunEndSession;
    WINTUN_GET_READ_WAIT_EVENT_FUNC WintunGetReadWaitEvent;
    WINTUN_RECEIVE_PACKET_FUNC WintunReceivePacket;
    WINTUN_RELEASE_RECEIVE_PACKET_FUNC WintunReleaseReceivePacket;
    WINTUN_ALLOCATE_SEND_PACKET_FUNC WintunAllocateSendPacket;
    WINTUN_SEND_PACKET_FUNC WintunSendPacket;
    WINTUN_GET_ADAPTER_LUID_FUNC WintunGetAdapterLuid;
} WINTUN_FUNCTIONS;

/**
 * Initialize wintun DLL and get function pointers
 * @return TRUE if successful, FALSE otherwise
 */
BOOL WINAPI InitializeWintun(
    _In_ const WCHAR* DllPath,
    _Out_ WINTUN_FUNCTIONS* Functions,
    _Out_ DWORD* Error
);

/**
 * Cleanup wintun DLL
 */
void WINAPI FinalizeWintun(void);

/**
 * Check if wintun driver is installed
 * @return TRUE if installed, FALSE otherwise
 */
BOOL WINAPI IsWintunInstalled(
    _Out_ DWORD* Error
);

/**
 * Check if wintun adapter with given name exists
 * @return TRUE if exists, FALSE otherwise
 */
BOOL WINAPI WintunAdapterExists(
    _In_ const WCHAR* Name,
    _Out_ DWORD* Error
);

#endif /* _WIN32 */

#endif /* _WINTUN_WINDOWS_H_ */
