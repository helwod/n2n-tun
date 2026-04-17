/**
 * (C) 2024 - n2n-wintun integration
 *
 * wintun_device.c - Wintun network adapter implementation for n2n
 *
 * This module provides wintun driver support for n2n on Windows,
 * allowing automatic creation of virtual network adapters when
 * the specified device does not exist.
 *
 * Based on WireGuard's wintun project: https://git.zx2c4.com/wintun
 * Licensed under GPLv2
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <newdev.h>
#include <objbase.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Include n2n headers */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "n2n.h"
#include "n2n_win32.h"
#include "wintap.h"
#include "wintun_windows.h"
#include "n2n_define.h"
#include "n2n_typedefs.h"

/* ***************************************************** */

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "ole32.lib")

/* wintun DLL name */
#define WINTUN_DLL_NAME L"wintun.dll"

/* Default adapter name prefix */
#define N2N_WINTUN_ADAPTER_PREFIX L"n2n"

/* ***************************************************** */

/**
 * Wintun function pointers
 */
static WINTUN_FUNCTIONS g_WintunFunctions;
static HMODULE g_WintunModule = NULL;
static BOOL g_WintunInitialized = FALSE;

/**
 * Wintun adapter context
 */
/* Maximum number of IP-to-MAC entries in the neighbor cache */
#define N2N_NEIGHBOR_CACHE_SIZE 32

typedef struct _N2N_WINTUN_CONTEXT {
    WINTUN_ADAPTER_HANDLE adapter;
    WINTUN_SESSION_HANDLE session;
    HANDLE read_event;
    WCHAR adapter_name[MAX_PATH];  /* Wide adapter name as passed to Wintun API */
    char  friendly_name[256];      /* Friendly name for netsh commands */
    int if_idx;
    uint8_t mac_addr[N2N_MAC_SIZE];
    uint32_t ip_addr;
    uint32_t device_mask;
    unsigned int mtu;
    CRITICAL_SECTION send_cs;
    /* ARP reply cache: when wintun_write receives an ARP request for our IP,
     * we generate a reply and cache it here for wintun_read to return to n2n.
     * This is needed because wintun is L3-only and cannot handle ARP natively,
     * but n2n peers need ARP to resolve our MAC. */
    uint8_t  arp_reply[64];        /* Cached ARP reply frame (Ethernet + ARP) */
    int      arp_reply_len;        /* Length of cached ARP reply (0 = none) */
    CRITICAL_SECTION arp_cs;       /* Protect arp_reply access */
    /* Neighbor cache: maps IP addresses to MAC addresses learned from
     * incoming Ethernet frames. Used by wintun_read to set correct
     * destination MAC for outgoing IP packets, enabling unicast P2P
     * instead of broadcast-via-supernode. */
    struct {
        uint32_t ip;               /* Network byte order */
        uint8_t  mac[N2N_MAC_SIZE];
    } neighbor_cache[N2N_NEIGHBOR_CACHE_SIZE];
    int neighbor_count;
    CRITICAL_SECTION neighbor_cs;
} N2N_WINTUN_CONTEXT;

/* ***************************************************** */

/**
 * Convert wide string to UTF-8 string
 */
static char* wchar_to_utf8(const wchar_t* wstr) {
    if (!wstr) return NULL;
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0) return NULL;
    char* result = (char*)malloc(size_needed);
    if (result) {
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result, size_needed, NULL, NULL);
    }
    return result;
}

/**
 * Convert UTF-8 string to wide string
 */
static wchar_t* utf8_to_wchar(const char* str) {
    if (!str) return NULL;
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (size_needed <= 0) return NULL;
    wchar_t* result = (wchar_t*)malloc(size_needed * sizeof(wchar_t));
    if (result) {
        MultiByteToWideChar(CP_UTF8, 0, str, -1, result, size_needed);
    }
    return result;
}

/* ***************************************************** */

/**
 * Initialize wintun DLL - load and resolve function pointers
 */
BOOL InitializeWintun(_In_ const WCHAR* DllPath, _Out_ WINTUN_FUNCTIONS* Functions, _Out_ DWORD* Error) {
    HMODULE module;
    WCHAR dll_path[MAX_PATH];

    if (Error) *Error = 0;

    /* Build DLL path */
    if (DllPath && DllPath[0]) {
        wcscpy_s(dll_path, MAX_PATH, DllPath);
    } else {
        /* Try to find wintun.dll in application directory first */
        if (GetModuleFileNameW(NULL, dll_path, MAX_PATH) == 0) {
            if (Error) *Error = GetLastError();
            return FALSE;
        }
        WCHAR* last_sep = wcsrchr(dll_path, L'\\');
        if (last_sep) last_sep[1] = L'\0';
        wcscat_s(dll_path, MAX_PATH, WINTUN_DLL_NAME);
    }

    /* Load wintun.dll */
    module = LoadLibraryExW(dll_path, NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        /* Try system32 directory */
        wcscpy_s(dll_path, MAX_PATH, L"C:\\Windows\\System32\\" WINTUN_DLL_NAME);
        module = LoadLibraryExW(dll_path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            if (Error) *Error = GetLastError();
            return FALSE;
        }
    }

    /* Store module handle for later cleanup */
    g_WintunModule = module;

    /* Get function pointers */
    memset(Functions, 0, sizeof(*Functions));

#define GET_PROC(name, type) \
    Functions->name = (type)GetProcAddress(module, #name); \
    if (!Functions->name) { \
        if (Error) *Error = GetLastError(); \
        FreeLibrary(module); \
        g_WintunModule = NULL; \
        return FALSE; \
    }

    GET_PROC(WintunCreateAdapter, WINTUN_CREATE_ADAPTER_FUNC)
    GET_PROC(WintunOpenAdapter, WINTUN_OPEN_ADAPTER_FUNC)
    GET_PROC(WintunCloseAdapter, WINTUN_CLOSE_ADAPTER_FUNC)
    GET_PROC(WintunDeleteDriver, WINTUN_DELETE_DRIVER_FUNC)
    GET_PROC(WintunGetRunningDriverVersion, WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC)
    GET_PROC(WintunStartSession, WINTUN_START_SESSION_FUNC)
    GET_PROC(WintunEndSession, WINTUN_END_SESSION_FUNC)
    GET_PROC(WintunGetReadWaitEvent, WINTUN_GET_READ_WAIT_EVENT_FUNC)
    GET_PROC(WintunReceivePacket, WINTUN_RECEIVE_PACKET_FUNC)
    GET_PROC(WintunReleaseReceivePacket, WINTUN_RELEASE_RECEIVE_PACKET_FUNC)
    GET_PROC(WintunAllocateSendPacket, WINTUN_ALLOCATE_SEND_PACKET_FUNC)
    GET_PROC(WintunSendPacket, WINTUN_SEND_PACKET_FUNC)

    /* WintunSetLogger is optional - don't fail if missing */
    Functions->WintunSetLogger = (WINTUN_SET_LOGGER_FUNC)GetProcAddress(module, "WintunSetLogger");

    /* WintunGetAdapterLuid is optional - older wintun.dll versions don't have it */
    Functions->WintunGetAdapterLuid = (WINTUN_GET_ADAPTER_LUID_FUNC)GetProcAddress(module, "WintunGetAdapterLuid");

#undef GET_PROC

    return TRUE;
}

/* ***************************************************** */

/**
 * Cleanup wintun DLL
 */
void FinalizeWintun(void) {
    if (g_WintunModule) {
        FreeLibrary(g_WintunModule);
        g_WintunModule = NULL;
    }
    memset(&g_WintunFunctions, 0, sizeof(g_WintunFunctions));
    g_WintunInitialized = FALSE;
}

/* ***************************************************** */

/**
 * Check if wintun DLL is loadable (driver will be auto-installed on first use)
 */
BOOL IsWintunInstalled(_Out_ DWORD* Error) {
    WINTUN_FUNCTIONS func_tmp;
    HMODULE test_module = NULL;
    WCHAR dll_path[MAX_PATH];

    if (Error) *Error = 0;

    /* If already initialized, check driver version */
    if (g_WintunInitialized) {
        SetLastError(0);
        DWORD version = g_WintunFunctions.WintunGetRunningDriverVersion();
        DWORD err = GetLastError();
        if (Error) *Error = err;
        return (version != 0);
    }

    /* Try to load wintun.dll to see if it exists */
    if (GetModuleFileNameW(NULL, dll_path, MAX_PATH) == 0) {
        if (Error) *Error = GetLastError();
        return FALSE;
    }
    WCHAR* last_sep = wcsrchr(dll_path, L'\\');
    if (last_sep) last_sep[1] = L'\0';
    wcscat_s(dll_path, MAX_PATH, WINTUN_DLL_NAME);

    test_module = LoadLibraryExW(dll_path, NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!test_module) {
        /* Try system32 */
        test_module = LoadLibraryExW(L"C:\\Windows\\System32\\" WINTUN_DLL_NAME, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    if (!test_module) {
        if (Error) *Error = GetLastError();
        return FALSE;
    }

    /* DLL exists, check if key functions are available */
    if (!GetProcAddress(test_module, "WintunCreateAdapter") ||
        !GetProcAddress(test_module, "WintunOpenAdapter")) {
        FreeLibrary(test_module);
        if (Error) *Error = ERROR_PROC_NOT_FOUND;
        return FALSE;
    }

    FreeLibrary(test_module);
    return TRUE;
}

/* ***************************************************** */

/**
 * Check if wintun adapter with given name exists
 */
BOOL WintunAdapterExists(_In_ const WCHAR* Name, _Out_ DWORD* Error) {
    if (!g_WintunInitialized) {
        if (!InitializeWintun(NULL, &g_WintunFunctions, Error)) {
            return FALSE;
        }
        g_WintunInitialized = TRUE;
    }

    SetLastError(0);
    WINTUN_ADAPTER_HANDLE adapter = g_WintunFunctions.WintunOpenAdapter(Name);
    DWORD err = GetLastError();
    if (adapter) {
        g_WintunFunctions.WintunCloseAdapter(adapter);
        if (Error) *Error = 0;
        return TRUE;
    }

    if (Error) *Error = err;
    return FALSE;
}

/* ***************************************************** */

/**
 * Create a new wintun adapter with the specified name
 */
static WINTUN_ADAPTER_HANDLE CreateWintunAdapterInternal(_In_ const WCHAR* Name, _Out_ DWORD* Error) {
    WINTUN_ADAPTER_HANDLE adapter;
    GUID guid;

    if (!g_WintunInitialized) {
        if (!InitializeWintun(NULL, &g_WintunFunctions, Error)) {
            return NULL;
        }
        g_WintunInitialized = TRUE;
    }

    /* Initialize COM for CoCreateGuid */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* Generate a random GUID for the adapter */
    if (CoCreateGuid(&guid) != S_OK) {
        if (Error) *Error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }

    traceEvent(TRACE_NORMAL, "Creating wintun adapter '%ls'...", Name);
    SetLastError(0);
    adapter = g_WintunFunctions.WintunCreateAdapter(Name, L"n2n", &guid);
    DWORD err = GetLastError();

    if (adapter) {
        traceEvent(TRACE_NORMAL, "Successfully created wintun adapter '%ls'", Name);
    } else {
        traceEvent(TRACE_ERROR, "Failed to create wintun adapter: error %lu", err);
    }

    if (Error) *Error = err;
    return adapter;
}

/* ***************************************************** */

/**
 * Get friendly name (interface alias) and interface index for a wintun adapter
 * by its LUID. This is the most reliable way to find the correct adapter,
 * avoiding name collisions with TAP-Win32 adapters that may share the same
 * friendly name.
 */
static BOOL GetAdapterInfoByLuid(_In_ const NET_LUID* luid,
                                  _Out_ char* friendly_name,
                                  _In_ size_t friendly_name_size,
                                  _Out_ int* if_idx) {
    ULONG buffer_len = 0;
    PIP_ADAPTER_ADDRESSES addresses = NULL;
    PIP_ADAPTER_ADDRESSES addr;
    ULONG flags = 0;
    DWORD result;
    BOOL found = FALSE;

    *if_idx = -1;
    friendly_name[0] = '\0';

    /* First call to get buffer size */
    result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &buffer_len);
    if (result != ERROR_BUFFER_OVERFLOW) {
        traceEvent(TRACE_INFO, "GetAdapterInfoByLuid(1st call) returned %lu", result);
        return FALSE;
    }

    addresses = (PIP_ADAPTER_ADDRESSES)malloc(buffer_len);
    if (!addresses) return FALSE;

    result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addresses, &buffer_len);
    if (result != NO_ERROR) {
        traceEvent(TRACE_INFO, "GetAdapterInfoByLuid(2nd call) failed: %lu", result);
        free(addresses);
        return FALSE;
    }

    /* Search for adapter matching the LUID */
    for (addr = addresses; addr; addr = addr->Next) {
        if (addr->Luid.Value == luid->Value) {
            /* Found it by LUID - this is the correct adapter */
            if (addr->FriendlyName) {
                WideCharToMultiByte(CP_UTF8, 0, addr->FriendlyName, -1,
                                    friendly_name, (int)friendly_name_size, NULL, NULL);
            }
            *if_idx = (int)addr->IfIndex;
            found = TRUE;
            traceEvent(TRACE_INFO, "GetAdapterInfoByLuid: matched by LUID, friendly='%s', if_idx=%d, type=%lu",
                       friendly_name, *if_idx, addr->IfType);
            break;
        }
    }

    if (!found) {
        traceEvent(TRACE_WARNING, "GetAdapterInfoByLuid: no adapter found with matching LUID");
    }

    free(addresses);
    return found;
}

/* ***************************************************** */

/**
 * Get friendly name (interface alias) for a wintun adapter by its wintun name.
 * Wintun adapters have the adapter name as their friendly name by default.
 * We use GetAdaptersAddresses to find the correct friendly name.
 *
 * NOTE: This function matches by FriendlyName which can be unreliable when
 * TAP-Win32 adapters share the same name. Prefer GetAdapterInfoByLuid instead.
 */
static BOOL GetAdapterFriendlyName(_In_ const WCHAR* wintun_adapter_name,
                                    _Out_ char* friendly_name,
                                    _In_ size_t friendly_name_size,
                                    _Out_ int* if_idx) {
    ULONG buffer_len = 0;
    PIP_ADAPTER_ADDRESSES addresses = NULL;
    PIP_ADAPTER_ADDRESSES addr;
    ULONG flags = 0;  /* Use minimal flags for safety - GAA_FLAG_INCLUDE_ALL_INTERFACES can cause issues */
    DWORD result;
    BOOL found = FALSE;
    int adapter_count = 0;

    *if_idx = -1;
    friendly_name[0] = '\0';

    /* First call to get buffer size - use AF_UNSPEC to include adapters without IPv4 yet */
    result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &buffer_len);
    if (result != ERROR_BUFFER_OVERFLOW) {
        traceEvent(TRACE_INFO, "GetAdaptersAddresses(1st call) returned %lu, expected BUFFER_OVERFLOW", result);
        return FALSE;
    }

    addresses = (PIP_ADAPTER_ADDRESSES)malloc(buffer_len);
    if (!addresses) return FALSE;

    result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addresses, &buffer_len);
    if (result != NO_ERROR) {
        traceEvent(TRACE_INFO, "GetAdaptersAddresses(2nd call) failed: %lu", result);
        free(addresses);
        return FALSE;
    }

    /* Search for adapter matching the wintun name.
     * When TAP-Win32 and wintun adapters share the same FriendlyName,
     * we must distinguish them by checking Description (wintun adapters
     * have Description containing "Wintun" or "WireGuard"). */
    PIP_ADAPTER_ADDRESSES best_match = NULL;
    PIP_ADAPTER_ADDRESSES name_match = NULL;  /* First adapter matching by name only */
    PIP_ADAPTER_ADDRESSES wintun_match = NULL; /* Adapter confirmed as wintun by Description */
    for (addr = addresses; addr; addr = addr->Next) {
        adapter_count++;

        /* Compare with the wintun adapter name */
        if (addr->FriendlyName && wcscmp(addr->FriendlyName, wintun_adapter_name) == 0) {
            char narrow_desc[256] = {0};
            if (addr->Description) {
                WideCharToMultiByte(CP_UTF8, 0, addr->Description, -1,
                                    narrow_desc, sizeof(narrow_desc), NULL, NULL);
            }

            traceEvent(TRACE_INFO, "GetAdapterFriendlyName: found adapter friendly='%ls', desc='%s', if_idx=%lu, type=%lu",
                       addr->FriendlyName, narrow_desc, addr->IfIndex, addr->IfType);

            /* Check if this looks like a wintun adapter by Description */
            BOOL is_wintun = FALSE;
            if (addr->Description) {
                if (wcsstr(addr->Description, L"Wintun") ||
                    wcsstr(addr->Description, L"wintun") ||
                    wcsstr(addr->Description, L"WireGuard")) {
                    is_wintun = TRUE;
                }
            }

            if (is_wintun) {
                /* Definitive wintun match - use this one */
                wintun_match = addr;
                break;  /* No need to keep looking */
            }

            /* Name matches but not confirmed wintun - save as candidate */
            if (!name_match) {
                name_match = addr;
            }
        }

        /* Also try matching by description (some wintun versions use this) */
        if (!name_match && !wintun_match && addr->Description) {
            char narrow_desc[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, addr->Description, -1,
                                narrow_desc, sizeof(narrow_desc), NULL, NULL);

            /* Convert wintun_adapter_name to narrow for comparison */
            char narrow_wintun_name[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, wintun_adapter_name, -1,
                                narrow_wintun_name, sizeof(narrow_wintun_name), NULL, NULL);

            if (strcmp(narrow_desc, narrow_wintun_name) == 0) {
                /* Description matches the adapter name - likely a wintun adapter */
                if (!name_match) {
                    name_match = addr;
                }
            }
        }
    }

    /* Prefer confirmed wintun match over name-only match */
    best_match = wintun_match ? wintun_match : name_match;

    /* Use the best match found */
    if (best_match) {
        if (best_match->FriendlyName) {
            WideCharToMultiByte(CP_UTF8, 0, best_match->FriendlyName, -1,
                                friendly_name, (int)friendly_name_size, NULL, NULL);
        }
        *if_idx = (int)best_match->IfIndex;
        found = TRUE;

        char best_desc[256] = {0};
        if (best_match->Description) {
            WideCharToMultiByte(CP_UTF8, 0, best_match->Description, -1,
                                best_desc, sizeof(best_desc), NULL, NULL);
        }
        traceEvent(TRACE_INFO, "GetAdapterFriendlyName: matched adapter friendly='%s', desc='%s', if_idx=%d, type=%lu",
                   friendly_name, best_desc, *if_idx, best_match->IfType);
    }

    traceEvent(TRACE_INFO, "GetAdapterFriendlyName: scanned %d adapters, found=%d", adapter_count, found);
    free(addresses);
    return found;
}

/* ***************************************************** */

/**
 * Verify that a specific IP address is configured on the adapter with given if_idx.
 * Returns TRUE if found, FALSE otherwise.
 */
static BOOL VerifyAdapterIP(_In_ int if_idx, _In_ uint32_t expected_ip) {
    ULONG buf_len = 0;
    PIP_ADAPTER_ADDRESSES addrs = NULL, a;
    ULONG flags = 0;
    DWORD res;
    BOOL found = FALSE;

    if (if_idx <= 0) {
        /* If no if_idx, search all adapters for the IP */
        res = GetAdaptersAddresses(AF_INET, flags, NULL, NULL, &buf_len);
        if (res == ERROR_BUFFER_OVERFLOW) {
            addrs = (PIP_ADAPTER_ADDRESSES)malloc(buf_len);
            if (addrs) {
                res = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &buf_len);
                if (res == NO_ERROR) {
                    for (a = addrs; a; a = a->Next) {
                        PIP_ADAPTER_UNICAST_ADDRESS ua;
                        for (ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                            struct sockaddr_in *sa = (struct sockaddr_in*)ua->Address.lpSockaddr;
                            if (sa->sin_family == AF_INET && sa->sin_addr.s_addr == expected_ip) {
                                found = TRUE;
                                traceEvent(TRACE_INFO, "VerifyAdapterIP: IP found on adapter if_idx=%lu name='%ls'",
                                           a->IfIndex, a->FriendlyName ? a->FriendlyName : L"(null)");
                                break;
                            }
                        }
                        if (found) break;
                    }
                }
                free(addrs);
            }
        }
    } else {
        /* Search specific adapter by if_idx */
        res = GetAdaptersAddresses(AF_INET, flags, NULL, NULL, &buf_len);
        if (res == ERROR_BUFFER_OVERFLOW) {
            addrs = (PIP_ADAPTER_ADDRESSES)malloc(buf_len);
            if (addrs) {
                res = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &buf_len);
                if (res == NO_ERROR) {
                    for (a = addrs; a; a = a->Next) {
                        if ((int)a->IfIndex == if_idx) {
                            PIP_ADAPTER_UNICAST_ADDRESS ua;
                            for (ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                                struct sockaddr_in *sa = (struct sockaddr_in*)ua->Address.lpSockaddr;
                                if (sa->sin_family == AF_INET && sa->sin_addr.s_addr == expected_ip) {
                                    found = TRUE;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                free(addrs);
            }
        }
    }

    if (!found) {
        traceEvent(TRACE_WARNING, "VerifyAdapterIP: IP not found on any matching adapter (if_idx=%d)", if_idx);
    }
    return found;
}

/**
 * Get interface index by friendly name using GetAdaptersAddresses.
 * Returns if_idx > 0 on success, -1 on failure.
 */
static int GetIfIndexByName(_In_ const char* friendly_name) {
    ULONG buf_len = 0;
    PIP_ADAPTER_ADDRESSES addrs = NULL, a;
    ULONG flags = 0;
    DWORD res;
    int if_idx = -1;
    wchar_t wname[256];

    if (!friendly_name || !friendly_name[0]) return -1;

    MultiByteToWideChar(CP_UTF8, 0, friendly_name, -1, wname, 256);

    res = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &buf_len);
    if (res == ERROR_BUFFER_OVERFLOW) {
        addrs = (PIP_ADAPTER_ADDRESSES)malloc(buf_len);
        if (addrs) {
            res = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addrs, &buf_len);
            if (res == NO_ERROR) {
                for (a = addrs; a; a = a->Next) {
                    if (a->FriendlyName && wcscmp(a->FriendlyName, wname) == 0) {
                        if_idx = (int)a->IfIndex;
                        break;
                    }
                }
            }
            free(addrs);
        }
    }

    return if_idx;
}

/* ***************************************************** */

/**
 * Configure IP address using Windows AddIPAddress API.
 * This is more reliable than netsh for newly created wintun adapters,
 * as netsh may fail when the adapter hasn't fully initialized.
 *
 * IMPORTANT: AddIPAddress only adds an IP, it doesn't disable DHCP.
 * We must also set the interface to static mode via netsh to prevent
 * APIPA (169.254.x.x) addresses from appearing.
 *
 * Returns 0 on success, -1 on failure.
 */
static int ConfigureIpAddressAPI(_In_ int if_idx,
                                  _In_ const char* ip_addr_str,
                                  _In_ const char* netmask_str) {
    UINT iaIP;
    UINT iaMask;
    ULONG ctx = 0;
    ULONG inst = 0;
    DWORD result;

    if (if_idx <= 0) {
        traceEvent(TRACE_WARNING, "ConfigureIpAddressAPI: invalid if_idx=%d, cannot use AddIPAddress API", if_idx);
        return -1;
    }

    iaIP = inet_addr(ip_addr_str);
    iaMask = inet_addr(netmask_str && netmask_str[0] ? netmask_str : "255.255.255.0");

    if (iaIP == INADDR_NONE || iaMask == INADDR_NONE) {
        traceEvent(TRACE_ERROR, "ConfigureIpAddressAPI: invalid IP or mask (ip=%s mask=%s)",
                   ip_addr_str, netmask_str ? netmask_str : "(null)");
        return -1;
    }

    traceEvent(TRACE_INFO, "ConfigureIpAddressAPI: AddIPAddress(ip=%s mask=%s if_idx=%d)",
               ip_addr_str, netmask_str ? netmask_str : "255.255.255.0", if_idx);

    result = AddIPAddress(iaIP, iaMask, (ULONG)if_idx, &ctx, &inst);
    if (result != NO_ERROR) {
        traceEvent(TRACE_WARNING, "ConfigureIpAddressAPI: AddIPAddress failed: error %lu (if_idx=%d)",
                   result, if_idx);
        return -1;
    }

    traceEvent(TRACE_INFO, "ConfigureIpAddressAPI: AddIPAddress succeeded (ctx=%lu inst=%lu)", ctx, inst);

    /* Also disable DHCP on this interface to prevent APIPA addresses.
     * netsh set address with "static" disables DHCP even if it doesn't
     * actually set the IP (because AddIPAddress already did).
     * The "source=static" tells Windows this is a statically configured interface. */
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "netsh interface ip set address interfaceIndex=%d static %s %s > nul 2>&1",
                 if_idx, ip_addr_str, netmask_str && netmask_str[0] ? netmask_str : "255.255.255.0");
        int sys_result = system(cmd);
        if (sys_result != 0) {
            traceEvent(TRACE_INFO, "netsh set static mode returned %d (may be OK if IP already set via API)", sys_result);
        } else {
            traceEvent(TRACE_INFO, "netsh set static mode succeeded - DHCP/APIPA disabled");
        }
    }

    return 0;
}

/**
 * Configure IP address using netsh.
 * Tries interfaceIndex first (most reliable), falls back to interface name.
 */
static int ConfigureIpAddressNetsh(_In_ const char* ifname, _In_ int if_idx,
                                    _In_ const char* ip_addr,
                                    _In_ const char* netmask, _In_ int mtu) {
    char cmd[512];
    int result;

    /* Set IP address - prefer interfaceIndex for reliability */
    if (if_idx > 0) {
        snprintf(cmd, sizeof(cmd),
                 "netsh interface ip set address interfaceIndex=%d static %s %s > nul 2>&1",
                 if_idx, ip_addr, netmask ? netmask : "255.255.255.0");
    } else if (netmask && netmask[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "netsh interface ip set address \"%s\" static %s %s > nul 2>&1",
                 ifname, ip_addr, netmask);
    } else {
        snprintf(cmd, sizeof(cmd), "netsh interface ip set address \"%s\" static %s 255.255.255.0 > nul 2>&1",
                 ifname, ip_addr);
    }
    result = system(cmd);
    if (result != 0) {
        traceEvent(TRACE_WARNING, "netsh set address failed for '%s' (if_idx=%d, error=%d)",
                   ifname, if_idx, result);
        /* If interfaceIndex failed, try with name as fallback */
        if (if_idx > 0) {
            traceEvent(TRACE_INFO, "Retrying with interface name '%s'...", ifname);
            if (netmask && netmask[0] != '\0') {
                snprintf(cmd, sizeof(cmd), "netsh interface ip set address \"%s\" static %s %s > nul 2>&1",
                         ifname, ip_addr, netmask);
            } else {
                snprintf(cmd, sizeof(cmd), "netsh interface ip set address \"%s\" static %s 255.255.255.0 > nul 2>&1",
                         ifname, ip_addr);
            }
            result = system(cmd);
        }
    } else {
        traceEvent(TRACE_INFO, "netsh set address succeeded for '%s' (if_idx=%d)", ifname, if_idx);
    }

    /* Set MTU */
    if (if_idx > 0) {
        snprintf(cmd, sizeof(cmd),
                 "netsh interface ipv4 set subinterface interfaceIndex=%d mtu=%d store=persistent > nul 2>&1",
                 if_idx, mtu);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "netsh interface ipv4 set subinterface \"%s\" mtu=%d store=persistent > nul 2>&1",
                 ifname, mtu);
    }
    result = system(cmd);
    if (result != 0) {
        traceEvent(TRACE_WARNING, "Failed to set MTU for '%s' (error=%d)", ifname, result);
    }

    return 0;
}

/* Forward declaration - defined after ConfigureIpAddress */
static void SetInterfaceMetric(int if_idx, int metric);

/**
 * Configure IP address on wintun adapter with retry logic.
 * Uses AddIPAddress API first (most reliable), falls back to netsh.
 * Retries up to 'max_retries' times with 'retry_delay_ms' between attempts,
 * as newly created wintun adapters may need time to become ready.
 */
static int ConfigureIpAddress(_In_ const char* ifname, _In_ int if_idx,
                               _In_ const char* ip_addr,
                               _In_ const char* netmask, _In_ int mtu, _In_ int metric) {
    int max_retries = 5;
    int retry_delay_ms = 500;
    int attempt;
    int api_result;
    BOOL ip_verified = FALSE;

    traceEvent(TRACE_INFO, "Configuring adapter '%s' (if_idx=%d): ip=%s mask=%s mtu=%d metric=%d",
               ifname, if_idx, ip_addr, netmask ? netmask : "(default)", mtu, metric);

    /* First, try AddIPAddress API - most reliable for wintun adapters */
    if (if_idx > 0) {
        for (attempt = 0; attempt < max_retries; attempt++) {
            api_result = ConfigureIpAddressAPI(if_idx, ip_addr, netmask);
            if (api_result == 0) {
                /* Verify IP was actually set */
                Sleep(200);
                ip_verified = VerifyAdapterIP(if_idx, inet_addr(ip_addr));
                if (ip_verified) {
                    traceEvent(TRACE_INFO, "IP %s verified on if_idx=%d via AddIPAddress API (attempt %d)",
                               ip_addr, if_idx, attempt + 1);
                    goto mtu_and_metric;
                }
                traceEvent(TRACE_WARNING, "AddIPAddress succeeded but IP not verified, retrying (%d/%d)...",
                           attempt + 1, max_retries);
            } else {
                traceEvent(TRACE_WARNING, "AddIPAddress failed (attempt %d/%d), if_idx may not be ready",
                           attempt + 1, max_retries);
            }

            /* Wait before retry - adapter may need time to initialize */
            if (attempt < max_retries - 1) {
                Sleep(retry_delay_ms);
                /* Re-query if_idx in case it changed */
                int new_if_idx = GetIfIndexByName(ifname);
                if (new_if_idx > 0 && new_if_idx != if_idx) {
                    traceEvent(TRACE_INFO, "if_idx changed from %d to %d during retry", if_idx, new_if_idx);
                    if_idx = new_if_idx;
                }
            }
        }
        traceEvent(TRACE_WARNING, "AddIPAddress API failed after %d attempts, falling back to netsh", max_retries);
    }

    /* Fallback: try netsh */
    for (attempt = 0; attempt < max_retries; attempt++) {
        ConfigureIpAddressNetsh(ifname, if_idx, ip_addr, netmask, mtu);

        /* Verify IP was set */
        Sleep(300);
        ip_verified = VerifyAdapterIP(if_idx > 0 ? if_idx : -1, inet_addr(ip_addr));
        if (ip_verified) {
            traceEvent(TRACE_INFO, "IP %s verified on '%s' via netsh (attempt %d)",
                       ip_addr, ifname, attempt + 1);
            goto mtu_and_metric;
        }

        traceEvent(TRACE_WARNING, "netsh configuration not verified (attempt %d/%d), retrying...",
                   attempt + 1, max_retries);

        if (attempt < max_retries - 1) {
            Sleep(retry_delay_ms);
            /* Re-query if_idx */
            if (if_idx <= 0) {
                int new_if_idx = GetIfIndexByName(ifname);
                if (new_if_idx > 0) {
                    if_idx = new_if_idx;
                }
            }
        }
    }

    traceEvent(TRACE_ERROR, "Failed to configure IP %s on '%s' after all attempts", ip_addr, ifname);

mtu_and_metric:
    /* Set metric if specified */
    if (metric > 0 && if_idx > 0) {
        SetInterfaceMetric(if_idx, metric);
    }

    return ip_verified ? 0 : -1;
}

/* ***************************************************** */

/**
 * Set interface metric on Windows
 */
static void SetInterfaceMetric(int if_idx, int metric) {
#ifdef _WIN64
    if (if_idx > 0 && metric > 0) {
        PMIB_IPINTERFACE_ROW row = (PMIB_IPINTERFACE_ROW)calloc(1, sizeof(MIB_IPINTERFACE_ROW));
        if (row) {
            InitializeIpInterfaceEntry(row);
            row->InterfaceIndex = if_idx;
            row->Family = AF_INET;
            if (GetIpInterfaceEntry(row) == NO_ERROR) {
                row->Metric = metric;
                row->SitePrefixLength = 0;
                if (SetIpInterfaceEntry(row) != NO_ERROR) {
                    traceEvent(TRACE_WARNING, "Failed to set interface metric to %d", metric);
                }
            }
            free(row);
        }
    }
#else
    (void)if_idx;
    (void)metric;
#endif
}

/* ***************************************************** */

/**
 * Wait for adapter to become available in the network stack.
 * After creating a new adapter, it may take a moment for it to appear.
 * We wait until the adapter has a valid IfIndex (> 0), not just a name.
 */
static BOOL WaitForAdapter(_In_ const WCHAR* adapter_name, _In_ int timeout_ms) {
    int elapsed = 0;
    char friendly_name[256];
    int if_idx;
    int attempt = 0;

    while (elapsed < timeout_ms) {
        attempt++;
        if (GetAdapterFriendlyName(adapter_name, friendly_name, sizeof(friendly_name), &if_idx)) {
            if (if_idx > 0) {
                traceEvent(TRACE_INFO, "Adapter found after %d ms (attempt %d), friendly='%s', if_idx=%d",
                           elapsed, attempt, friendly_name, if_idx);
                return TRUE;
            }
            traceEvent(TRACE_INFO, "Adapter found but if_idx=0 (attempt %d, %d ms), waiting for index assignment...",
                       attempt, elapsed);
        } else {
            if (attempt <= 3 || attempt % 10 == 0) {
                traceEvent(TRACE_INFO, "Adapter not yet visible (attempt %d, %d ms elapsed)...",
                           attempt, elapsed);
            }
        }
        Sleep(100);
        elapsed += 100;
    }
    traceEvent(TRACE_WARNING, "Adapter not found or no IfIndex after %d ms timeout", timeout_ms);
    return FALSE;
}

/* ***************************************************** */

/**
 * Open or create wintun adapter
 * If adapter doesn't exist and create_if_needed is TRUE, create it
 */
int wintun_open(_Out_ tuntap_dev* device,
                _In_ const char* devname,
                _In_ const char* address_mode,
                _In_ char* device_ip,
                _In_ char* device_mask,
                _In_ const char* device_mac,
                _In_ int mtu,
                _In_ int metric,
                _In_ BOOL create_if_needed) {

    N2N_WINTUN_CONTEXT* ctx;
    WINTUN_ADAPTER_HANDLE adapter = NULL;
    WINTUN_SESSION_HANDLE session = NULL;
    wchar_t* w_adapter_name = NULL;
    char* narrow_name = NULL;
    DWORD error = 0;
    int result = -1;
    BOOL adapter_created = FALSE;

    traceEvent(TRACE_INFO, "wintun_open: device=%s, create_if_needed=%d",
               devname ? devname : "(auto)", create_if_needed);

    /* Zero out device struct pointer fields to prevent free() on garbage pointers */
    device->device_name = NULL;
    device->ifName = NULL;
    device->device_handle = NULL;

    /* Initialize wintun if not already done */
    if (!g_WintunInitialized) {
        traceEvent(TRACE_NORMAL, "Loading wintun.dll...");
        if (!InitializeWintun(NULL, &g_WintunFunctions, &error)) {
            traceEvent(TRACE_ERROR, "Failed to initialize wintun: error %lu (wintun.dll not found?)", error);
            goto cleanup;
        }
        g_WintunInitialized = TRUE;

        /* Log driver version */
        SetLastError(0);
        DWORD driver_version = g_WintunFunctions.WintunGetRunningDriverVersion();
        DWORD ver_err = GetLastError();
        if (driver_version) {
            traceEvent(TRACE_NORMAL, "Wintun driver version: %lu.%lu",
                       driver_version >> 16, driver_version & 0xFFFF);
        } else {
            traceEvent(TRACE_INFO, "Wintun driver not yet installed (will install on first adapter creation), error=%lu", ver_err);
        }
    }

    /* Allocate context */
    ctx = (N2N_WINTUN_CONTEXT*)calloc(1, sizeof(N2N_WINTUN_CONTEXT));
    if (!ctx) {
        traceEvent(TRACE_ERROR, "Failed to allocate wintun context");
        goto cleanup;
    }

    /* Determine adapter name */
    if (devname && devname[0]) {
        w_adapter_name = utf8_to_wchar(devname);
    } else {
        /* Generate a default name */
        w_adapter_name = (wchar_t*)malloc(sizeof(wchar_t) * 64);
        if (w_adapter_name) {
            swprintf(w_adapter_name, 64, L"%s-%04x", N2N_WINTUN_ADAPTER_PREFIX, GetCurrentThreadId() & 0xFFFF);
        }
    }

    if (!w_adapter_name) {
        traceEvent(TRACE_ERROR, "Failed to create adapter name");
        goto cleanup;
    }

    /* Try to open existing adapter first */
    SetLastError(0);
    adapter = g_WintunFunctions.WintunOpenAdapter(w_adapter_name);
    error = GetLastError();

    if (!adapter) {
        traceEvent(TRACE_INFO, "WintunOpenAdapter('%ls') returned NULL, GetLastError=%lu",
                   w_adapter_name, error);
        if (create_if_needed) {
            /* Adapter doesn't exist, try to create it */
            traceEvent(TRACE_NORMAL, "Adapter '%ls' not found, creating new wintun adapter...",
                       w_adapter_name);

            adapter = CreateWintunAdapterInternal(w_adapter_name, &error);
            if (!adapter) {
                traceEvent(TRACE_ERROR, "Failed to create wintun adapter: error %lu", error);
                goto cleanup;
            }
            adapter_created = TRUE;
            traceEvent(TRACE_NORMAL, "Successfully created new wintun adapter '%ls'",
                       w_adapter_name);
        } else {
            traceEvent(TRACE_ERROR, "Failed to open wintun adapter '%ls': error %lu",
                       w_adapter_name, error);
            goto cleanup;
        }
    } else {
        traceEvent(TRACE_NORMAL, "Opened existing wintun adapter '%ls'", w_adapter_name);
    }

    traceEvent(TRACE_INFO, "Adapter handle = %p", (void*)adapter);

    ctx->adapter = adapter;
    wcscpy_s(ctx->adapter_name, MAX_PATH, w_adapter_name);

    /* If we just created the adapter, wait for it to appear in the network stack */
    if (adapter_created) {
        traceEvent(TRACE_INFO, "Waiting for new adapter to become available...");
        fflush(stdout);
        if (!WaitForAdapter(w_adapter_name, 5000)) {
            traceEvent(TRACE_WARNING, "New adapter not yet visible in network stack, proceeding anyway");
        }
        traceEvent(TRACE_INFO, "WaitForAdapter completed");
        fflush(stdout);
    }

    /* Get adapter LUID for reliable identification (avoids TAP name collisions) */
    traceEvent(TRACE_INFO, "Getting adapter LUID...");
    fflush(stdout);
    NET_LUID adapter_luid = {0};
    BOOL have_luid = FALSE;
    if (g_WintunFunctions.WintunGetAdapterLuid) {
        SetLastError(0);
        g_WintunFunctions.WintunGetAdapterLuid(adapter, &adapter_luid);
        DWORD luid_err = GetLastError();
        if (luid_err == 0) {
            have_luid = TRUE;
            traceEvent(TRACE_INFO, "Got adapter LUID: 0x%llx", (unsigned long long)adapter_luid.Value);
        } else {
            traceEvent(TRACE_WARNING, "WintunGetAdapterLuid failed: error %lu, falling back to name matching", luid_err);
        }
    } else {
        traceEvent(TRACE_WARNING, "WintunGetAdapterLuid not available, falling back to name matching");
    }
    fflush(stdout);

    traceEvent(TRACE_INFO, "Step: Getting friendly name for adapter '%ls'...", w_adapter_name);
    fflush(stdout);

    /* Get friendly name and interface index - prefer LUID-based lookup */
    {
        BOOL gfn_result = FALSE;
        if (have_luid) {
            /* Use LUID-based lookup - most reliable, avoids TAP adapter name collisions */
            gfn_result = GetAdapterInfoByLuid(&adapter_luid, ctx->friendly_name, sizeof(ctx->friendly_name), &ctx->if_idx);
            if (gfn_result) {
                traceEvent(TRACE_INFO, "Found adapter by LUID: friendly='%s', if_idx=%d", ctx->friendly_name, ctx->if_idx);
            } else {
                traceEvent(TRACE_WARNING, "LUID-based lookup failed, falling back to name matching");
            }
        }

        if (!gfn_result) {
            /* Fallback: match by FriendlyName (less reliable - may match TAP adapter) */
            gfn_result = GetAdapterFriendlyName(w_adapter_name, ctx->friendly_name, sizeof(ctx->friendly_name), &ctx->if_idx);
        }

        if (!gfn_result) {
            /* Last resort: use the adapter name as friendly name */
            traceEvent(TRACE_INFO, "All friendly name lookups failed, using fallback name");
            char* tmp = wchar_to_utf8(w_adapter_name);
            if (tmp) {
                strncpy(ctx->friendly_name, tmp, sizeof(ctx->friendly_name) - 1);
                ctx->friendly_name[sizeof(ctx->friendly_name) - 1] = '\0';
                free(tmp);
            }
            ctx->if_idx = -1;

            /* Try once more after a short delay if adapter was just created */
            if (adapter_created) {
                traceEvent(TRACE_INFO, "Retrying after 500ms delay...");
                Sleep(500);
                if (have_luid) {
                    gfn_result = GetAdapterInfoByLuid(&adapter_luid, ctx->friendly_name, sizeof(ctx->friendly_name), &ctx->if_idx);
                }
                if (!gfn_result) {
                    gfn_result = GetAdapterFriendlyName(w_adapter_name, ctx->friendly_name, sizeof(ctx->friendly_name), &ctx->if_idx);
                }
                if (gfn_result) {
                    traceEvent(TRACE_INFO, "Retry succeeded: friendly='%s', if_idx=%d", ctx->friendly_name, ctx->if_idx);
                }
            }
        } else {
            traceEvent(TRACE_INFO, "Got friendly name: '%s', if_idx=%d", ctx->friendly_name, ctx->if_idx);
        }
    }
    fflush(stdout);

    /* Set device_name (internal adapter name / GUID-like) */
    traceEvent(TRACE_INFO, "Setting device_name...");
    fflush(stdout);
    narrow_name = wchar_to_utf8(w_adapter_name);
    if (narrow_name) {
        if (device->device_name) { free(device->device_name); device->device_name = NULL; }
        device->device_name = _strdup(narrow_name);
        free(narrow_name);
    }

    /* Set ifName to the friendly name for netsh operations */
    traceEvent(TRACE_INFO, "Setting ifName...");
    fflush(stdout);
    if (device->ifName) { free(device->ifName); device->ifName = NULL; }
    device->ifName = _strdup(ctx->friendly_name);

    /* Start session for packet I/O */
    traceEvent(TRACE_INFO, "Starting wintun session (ring capacity=0x%x)...", WINTUN_RING_CAPACITY);
    fflush(stdout);
    SetLastError(0);
    traceEvent(TRACE_INFO, "About to call WintunStartSession(adapter=%p, capacity=0x%x)...", (void*)adapter, WINTUN_RING_CAPACITY);
    fflush(stdout);
    session = g_WintunFunctions.WintunStartSession(adapter, WINTUN_RING_CAPACITY);
    error = GetLastError();
    traceEvent(TRACE_INFO, "WintunStartSession returned session=%p, GetLastError=%lu", (void*)session, error);
    fflush(stdout);
    if (!session) {
        traceEvent(TRACE_ERROR, "Failed to start wintun session: error %lu", error);
        goto cleanup;
    }
    ctx->session = session;
    traceEvent(TRACE_INFO, "Wintun session started, handle=%p", (void*)session);
    fflush(stdout);

    /* Get read wait event */
    traceEvent(TRACE_INFO, "Getting read wait event...");
    fflush(stdout);
    SetLastError(0);
    ctx->read_event = g_WintunFunctions.WintunGetReadWaitEvent(session);
    error = GetLastError();
    traceEvent(TRACE_INFO, "Read wait event handle=%p, GetLastError=%lu", (void*)ctx->read_event, error);
    fflush(stdout);
    if (!ctx->read_event) {
        traceEvent(TRACE_ERROR, "Failed to get read wait event, error=%lu", error);
        goto cleanup;
    }

    /* Initialize critical section for thread-safe sending */
    InitializeCriticalSection(&ctx->send_cs);
    InitializeCriticalSection(&ctx->arp_cs);
    InitializeCriticalSection(&ctx->neighbor_cs);
    ctx->arp_reply_len = 0;
    ctx->neighbor_count = 0;

    /* Set default MAC address (wintun doesn't expose MAC directly) */
    if (device_mac && device_mac[0]) {
        str2mac(ctx->mac_addr, device_mac);
    } else {
        /* Generate random MAC */
        memrnd(ctx->mac_addr, N2N_MAC_SIZE);
        ctx->mac_addr[0] &= ~0x01;  /* Clear multicast bit */
        ctx->mac_addr[0] |= 0x02;   /* Set local assignment bit */
    }
    memcpy(device->mac_addr, ctx->mac_addr, N2N_MAC_SIZE);

    /* Set IP address and configure interface */
    ctx->ip_addr = inet_addr(device_ip);
    ctx->device_mask = inet_addr(device_mask);
    ctx->mtu = mtu;

    device->if_idx = ctx->if_idx;
    device->ip_addr = ctx->ip_addr;
    device->device_mask = ctx->device_mask;
    device->mtu = mtu;

    /* Enable the network interface (equivalent to TAP_IOCTL_SET_MEDIA_STATUS) */
    traceEvent(TRACE_INFO, "Enabling interface '%s'...", ctx->friendly_name);
    fflush(stdout);
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "netsh interface set interface \"%s\" enabled", ctx->friendly_name);
        int sys_ret = system(cmd);
        traceEvent(TRACE_INFO, "netsh enable interface returned %d", sys_ret);
    }
    fflush(stdout);

    /* Configure IP address - uses AddIPAddress API first, falls back to netsh,
     * with automatic retries and verification */
    traceEvent(TRACE_INFO, "Configuring IP address %s/%s on '%s' (if_idx=%d)...",
               device_ip, device_mask, ctx->friendly_name, ctx->if_idx);
    fflush(stdout);
    {
        int cfg_result = ConfigureIpAddress(ctx->friendly_name, ctx->if_idx, device_ip, device_mask, mtu, metric);
        if (cfg_result != 0) {
            traceEvent(TRACE_WARNING, "IP configuration may have failed, but continuing anyway");
        }

        /* Re-query if_idx in case it was updated during retry */
        if (ctx->if_idx <= 0) {
            int new_idx = GetIfIndexByName(ctx->friendly_name);
            if (new_idx > 0) {
                ctx->if_idx = new_idx;
                device->if_idx = new_idx;
                traceEvent(TRACE_INFO, "Updated if_idx to %d after IP configuration", new_idx);
            }
        }
    }

    traceEvent(TRACE_INFO, "IP configuration done");
    fflush(stdout);

    /* Store context in device */
    device->device_handle = (HANDLE)ctx;

    traceEvent(TRACE_NORMAL, "wintun adapter ready: name='%s', friendly='%s', ip=%s, mask=%s, mtu=%d, if_idx=%d",
               device->device_name ? device->device_name : "?",
               ctx->friendly_name, device_ip, device_mask, mtu, ctx->if_idx);
    fflush(stdout);

    result = 0;

cleanup:
    if (result != 0) {
        traceEvent(TRACE_ERROR, "wintun_open FAILED, cleaning up (result=%d)", result);
        fflush(stdout);
    }
    if (w_adapter_name) free(w_adapter_name);

    if (result != 0 && adapter) {
        if (session) g_WintunFunctions.WintunEndSession(session);
        g_WintunFunctions.WintunCloseAdapter(adapter);
        if (ctx) free(ctx);
    }

    return result;
}

/* ***************************************************** */

/**
 * Read packet from wintun adapter
 *
 * Wintun is a L3/TUN device - it returns raw IP packets (no Ethernet header).
 * n2n expects L2/TAP frames with Ethernet headers. We must prepend a fake
 * Ethernet header to the IP packet before returning it to n2n.
 *
 * Ethernet header: [dst_mac 6B][src_mac 6B][ethertype 2B] = 14 bytes total
 * EtherType: 0x0800 = IPv4, 0x86DD = IPv6
 */
#define N2N_ETH_HDR_SIZE 14
#define N2N_ETHERTYPE_IPV4 0x0800
#define N2N_ETHERTYPE_IPV6 0x86DD
#define N2N_ETHERTYPE_ARP  0x0806

/* ARP constants */
#define ARP_HTYPE_ETHERNET 0x0001
#define ARP_PTYPE_IPV4     0x0800
#define ARP_HLEN           6
#define ARP_PLEN           4
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2
#define ARP_PKT_SIZE       (N2N_ETH_HDR_SIZE + 8 + ARP_HLEN * 2 + ARP_PLEN * 2)  /* 42 bytes */

/* ***************************************************** */

/**
 * Learn a MAC-IP mapping from an incoming Ethernet frame.
 * Called from wintun_write when we receive a frame from a remote n2n peer.
 * The src MAC comes from the Ethernet header, the src IP from the IP header.
 */
static void neighbor_learn(N2N_WINTUN_CONTEXT* ctx,
                           const uint8_t* eth_src_mac,
                           const unsigned char* ip_pkt, int ip_pkt_len) {
    uint8_t ip_version;
    uint32_t src_ip;
    int i;

    if (!eth_src_mac || !ip_pkt || ip_pkt_len < 20) return;

    ip_version = ip_pkt[0] >> 4;
    if (ip_version != 4) return;  /* Only handle IPv4 for now */

    /* Extract source IP from IPv4 header (bytes 12-15) */
    memcpy(&src_ip, &ip_pkt[12], 4);
    if (src_ip == 0) return;

    /* Don't cache our own IP */
    if (src_ip == ctx->ip_addr) return;

    EnterCriticalSection(&ctx->neighbor_cs);

    /* Update existing entry or add new one */
    for (i = 0; i < ctx->neighbor_count; i++) {
        if (ctx->neighbor_cache[i].ip == src_ip) {
            memcpy(ctx->neighbor_cache[i].mac, eth_src_mac, N2N_MAC_SIZE);
            LeaveCriticalSection(&ctx->neighbor_cs);
            return;
        }
    }

    /* Add new entry */
    if (ctx->neighbor_count < N2N_NEIGHBOR_CACHE_SIZE) {
        ctx->neighbor_cache[ctx->neighbor_count].ip = src_ip;
        memcpy(ctx->neighbor_cache[ctx->neighbor_count].mac, eth_src_mac, N2N_MAC_SIZE);
        ctx->neighbor_count++;
    } else {
        /* Cache full, replace oldest (index 0) by shifting */
        memmove(&ctx->neighbor_cache[0], &ctx->neighbor_cache[1],
                (N2N_NEIGHBOR_CACHE_SIZE - 1) * sizeof(ctx->neighbor_cache[0]));
        ctx->neighbor_cache[N2N_NEIGHBOR_CACHE_SIZE - 1].ip = src_ip;
        memcpy(ctx->neighbor_cache[N2N_NEIGHBOR_CACHE_SIZE - 1].mac, eth_src_mac, N2N_MAC_SIZE);
    }

    LeaveCriticalSection(&ctx->neighbor_cs);
}

/**
 * Look up MAC address for a given IP address in the neighbor cache.
 * Returns TRUE if found, FALSE otherwise.
 */
static BOOL neighbor_lookup(N2N_WINTUN_CONTEXT* ctx,
                            uint32_t ip, uint8_t* out_mac) {
    int i;
    BOOL found = FALSE;

    if (ip == 0) return FALSE;

    EnterCriticalSection(&ctx->neighbor_cs);
    for (i = 0; i < ctx->neighbor_count; i++) {
        if (ctx->neighbor_cache[i].ip == ip) {
            memcpy(out_mac, ctx->neighbor_cache[i].mac, N2N_MAC_SIZE);
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&ctx->neighbor_cs);

    return found;
}

int wintun_read(_In_ tuntap_dev* device, _Out_ unsigned char* buf, _In_ int len) {
    N2N_WINTUN_CONTEXT* ctx = (N2N_WINTUN_CONTEXT*)device->device_handle;
    DWORD packet_size = 0;
    BYTE* packet;

    if (!ctx || !ctx->session) {
        traceEvent(TRACE_WARNING, "wintun_read: ctx=%p session=%p (invalid)",
                   (void*)ctx, ctx ? (void*)ctx->session : NULL);
        return -1;
    }

    /* Check buffer has room for Ethernet header + IP packet */
    if (len < N2N_ETH_HDR_SIZE + 1) {
        traceEvent(TRACE_WARNING, "wintun_read: buffer too small (%d)", len);
        return -1;
    }

    /* First, check if there's a cached ARP reply to return to n2n.
     * This is needed because wintun is L3-only and cannot handle ARP,
     * but n2n peers need ARP replies to resolve our MAC address. */
    EnterCriticalSection(&ctx->arp_cs);
    if (ctx->arp_reply_len > 0) {
        int reply_len = ctx->arp_reply_len;
        if (reply_len <= len) {
            memcpy(buf, ctx->arp_reply, reply_len);
        }
        ctx->arp_reply_len = 0;  /* Clear after consuming */
        LeaveCriticalSection(&ctx->arp_cs);
        traceEvent(TRACE_INFO, "wintun_read: returning cached ARP reply (%d bytes)", reply_len);
        return reply_len;
    }
    LeaveCriticalSection(&ctx->arp_cs);

    /* Try to receive a packet */
    SetLastError(0);
    packet = g_WintunFunctions.WintunReceivePacket(ctx->session, &packet_size);

    if (packet && packet_size > 0) {
        /* Got a raw IP packet from wintun - prepend Ethernet header */
        uint16_t ethertype;
        int total_len;

        /* Determine EtherType from IP version */
        if ((packet[0] >> 4) == 4) {
            ethertype = N2N_ETHERTYPE_IPV4;
        } else if ((packet[0] >> 4) == 6) {
            ethertype = N2N_ETHERTYPE_IPV6;
        } else {
            traceEvent(TRACE_WARNING, "wintun_read: unknown IP version %d, dropping", packet[0] >> 4);
            g_WintunFunctions.WintunReleaseReceivePacket(ctx->session, packet);
            return 0;
        }

        total_len = N2N_ETH_HDR_SIZE + (int)packet_size;
        if (total_len > len) {
            traceEvent(TRACE_WARNING, "wintun_read: packet too large (%d > %d)", total_len, len);
            g_WintunFunctions.WintunReleaseReceivePacket(ctx->session, packet);
            return 0;
        }

        /* Build Ethernet header */
        /* Look up destination MAC from neighbor cache for unicast.
         * For IPv4, dst IP is at offset 16-19 in the IP packet. */
        if (ethertype == N2N_ETHERTYPE_IPV4 && packet_size >= 20) {
            uint32_t dst_ip;
            memcpy(&dst_ip, &packet[16], 4);
            if (!neighbor_lookup(ctx, dst_ip, buf)) {
                /* Not found - use broadcast */
                memset(buf, 0xFF, N2N_MAC_SIZE);
            }
        } else {
            memset(buf, 0xFF, N2N_MAC_SIZE);                    /* dst MAC = broadcast */
        }
        memcpy(buf + N2N_MAC_SIZE, ctx->mac_addr, N2N_MAC_SIZE); /* src MAC = our MAC */
        buf[12] = (uint8_t)(ethertype >> 8);                    /* EtherType high byte */
        buf[13] = (uint8_t)(ethertype & 0xFF);                  /* EtherType low byte */

        /* Copy IP packet after Ethernet header */
        memcpy(buf + N2N_ETH_HDR_SIZE, packet, packet_size);

        g_WintunFunctions.WintunReleaseReceivePacket(ctx->session, packet);
        traceEvent(TRACE_DEBUG, "wintun_read: IP pkt %luB -> eth frame %dB (type=0x%04X)",
                   packet_size, total_len, ethertype);
        return total_len;
    }

    /* No packet available - check if it's an error or just no data */
    DWORD error = GetLastError();
    if (error == ERROR_NO_MORE_ITEMS || error == 0) {
        /* Normal: no packets available, wait for read event */
        DWORD wait_result = WaitForSingleObject(ctx->read_event, 100);  /* 100ms timeout */

        if (wait_result == WAIT_OBJECT_0) {
            /* Event signaled, try to receive again */
            SetLastError(0);
            packet = g_WintunFunctions.WintunReceivePacket(ctx->session, &packet_size);

            if (packet && packet_size > 0) {
                uint16_t ethertype;
                int total_len;

                if ((packet[0] >> 4) == 4) {
                    ethertype = N2N_ETHERTYPE_IPV4;
                } else if ((packet[0] >> 4) == 6) {
                    ethertype = N2N_ETHERTYPE_IPV6;
                } else {
                    g_WintunFunctions.WintunReleaseReceivePacket(ctx->session, packet);
                    return 0;
                }

                total_len = N2N_ETH_HDR_SIZE + (int)packet_size;
                if (total_len > len) {
                    g_WintunFunctions.WintunReleaseReceivePacket(ctx->session, packet);
                    return 0;
                }

                /* Look up destination MAC from neighbor cache for unicast */
                if (ethertype == N2N_ETHERTYPE_IPV4 && packet_size >= 20) {
                    uint32_t dst_ip;
                    memcpy(&dst_ip, &packet[16], 4);
                    if (!neighbor_lookup(ctx, dst_ip, buf)) {
                        memset(buf, 0xFF, N2N_MAC_SIZE);
                    }
                } else {
                    memset(buf, 0xFF, N2N_MAC_SIZE);
                }
                memcpy(buf + N2N_MAC_SIZE, ctx->mac_addr, N2N_MAC_SIZE);
                buf[12] = (uint8_t)(ethertype >> 8);
                buf[13] = (uint8_t)(ethertype & 0xFF);
                memcpy(buf + N2N_ETH_HDR_SIZE, packet, packet_size);

                g_WintunFunctions.WintunReleaseReceivePacket(ctx->session, packet);
                traceEvent(TRACE_DEBUG, "wintun_read: IP pkt %luB after wait -> eth frame %dB",
                           packet_size, total_len);
                return total_len;
            }
        } else if (wait_result == WAIT_TIMEOUT) {
            /* Normal timeout, no data */
        } else {
            traceEvent(TRACE_WARNING, "wintun_read: WaitForSingleObject returned %lu, error=%lu",
                       wait_result, GetLastError());
        }
        /* Timeout or no data after wait - normal, return 0 */
        return 0;
    }

    /* Real error */
    traceEvent(TRACE_WARNING, "wintun_read: WintunReceivePacket error=%lu", error);
    return -1;
}

/* ***************************************************** */

/**
 * Write packet to wintun adapter
 *
 * n2n sends L2/TAP Ethernet frames, but wintun expects raw L3/TUN IP packets.
 * We must strip the 14-byte Ethernet header before sending to wintun.
 */
int wintun_write(_In_ tuntap_dev* device, _In_ unsigned char* buf, _In_ int len) {
    N2N_WINTUN_CONTEXT* ctx = (N2N_WINTUN_CONTEXT*)device->device_handle;
    BYTE* packet;
    int ip_len;
    unsigned char* ip_data;

    if (!ctx || !ctx->session) {
        return -1;
    }

    /* Strip Ethernet header - wintun only accepts IP packets */
    if (len <= N2N_ETH_HDR_SIZE) {
        traceEvent(TRACE_WARNING, "wintun_write: frame too short (%d), dropping", len);
        return -1;
    }

    /* Validate EtherType */
    uint16_t ethertype = ((uint16_t)buf[12] << 8) | buf[13];

    /* Handle ARP: wintun is L3-only, cannot send ARP frames to the driver.
     * However, n2n peers need ARP to resolve our MAC address. When we receive
     * an ARP request targeting our IP, we generate a reply and cache it so
     * wintun_read can return it to n2n on the next read cycle. */
    if (ethertype == N2N_ETHERTYPE_ARP) {
        if (len >= ARP_PKT_SIZE) {
            /* Parse ARP header (after 14-byte Ethernet header):
             * [+0..1] htype (0x0001 = Ethernet)
             * [+2..3] ptype (0x0800 = IPv4)
             * [+4]    hlen  (6)
             * [+5]    plen  (4)
             * [+6..7] oper  (1=Request, 2=Reply)
             * [+8..13]  sender MAC
             * [+14..17] sender IP
             * [+18..23] target MAC
             * [+24..27] target IP */
            unsigned char *arp = buf + N2N_ETH_HDR_SIZE;
            uint16_t arp_oper = ((uint16_t)arp[6] << 8) | arp[7];
            /* target_ip is in network byte order (same as in the packet),
             * ctx->ip_addr from inet_addr() is also network byte order */
            uint32_t target_ip;
            memcpy(&target_ip, &arp[24], 4);

            if (arp_oper == ARP_OP_REQUEST && target_ip == ctx->ip_addr) {
                /* ARP request for our IP - generate a reply */
                uint8_t reply[64];
                unsigned char *reply_arp;

                /* Ethernet header: dst = sender's MAC, src = our MAC */
                memcpy(reply, &buf[6], N2N_MAC_SIZE);        /* dst = sender MAC */
                memcpy(reply + N2N_MAC_SIZE, ctx->mac_addr, N2N_MAC_SIZE); /* src = our MAC */
                reply[12] = (uint8_t)(N2N_ETHERTYPE_ARP >> 8);
                reply[13] = (uint8_t)(N2N_ETHERTYPE_ARP & 0xFF);

                /* ARP payload */
                reply_arp = reply + N2N_ETH_HDR_SIZE;
                reply_arp[0] = 0x00; reply_arp[1] = 0x01;  /* htype = Ethernet */
                reply_arp[2] = 0x08; reply_arp[3] = 0x00;  /* ptype = IPv4 */
                reply_arp[4] = ARP_HLEN;                    /* hlen = 6 */
                reply_arp[5] = ARP_PLEN;                    /* plen = 4 */
                reply_arp[6] = 0x00; reply_arp[7] = ARP_OP_REPLY; /* oper = Reply */
                memcpy(&reply_arp[8], ctx->mac_addr, N2N_MAC_SIZE);  /* sender MAC = us */
                memcpy(&reply_arp[14], &ctx->ip_addr, 4);            /* sender IP = our IP */
                memcpy(&reply_arp[18], &arp[8], N2N_MAC_SIZE);       /* target MAC = requester */
                memcpy(&reply_arp[24], &arp[14], 4);                 /* target IP = requester IP */

                /* Cache the ARP reply for wintun_read */
                EnterCriticalSection(&ctx->arp_cs);
                memcpy(ctx->arp_reply, reply, ARP_PKT_SIZE);
                ctx->arp_reply_len = ARP_PKT_SIZE;
                LeaveCriticalSection(&ctx->arp_cs);

                traceEvent(TRACE_INFO, "wintun_write: generated ARP reply for %u.%u.%u.%u",
                           (unsigned)(ntohl(target_ip) >> 24) & 0xFF,
                           (unsigned)(ntohl(target_ip) >> 16) & 0xFF,
                           (unsigned)(ntohl(target_ip) >> 8) & 0xFF,
                           (unsigned)ntohl(target_ip) & 0xFF);
            }
        }
        /* ARP frames cannot be sent to wintun driver, return success */
        return len;
    }

    if (ethertype != N2N_ETHERTYPE_IPV4 && ethertype != N2N_ETHERTYPE_IPV6) {
        traceEvent(TRACE_DEBUG, "wintun_write: non-IP ethertype 0x%04X, dropping", ethertype);
        /* Still return len so n2n doesn't think it's an error */
        return len;
    }

    ip_data = buf + N2N_ETH_HDR_SIZE;
    ip_len = len - N2N_ETH_HDR_SIZE;

    if (ip_len <= 0) {
        return -1;
    }

    /* Learn MAC-IP mapping from incoming Ethernet frame for unicast routing.
     * src MAC is in the Ethernet header (bytes 6-11),
     * src IP is in the IPv4 header (bytes 12-15 of IP header). */
    if (ethertype == N2N_ETHERTYPE_IPV4) {
        neighbor_learn(ctx, &buf[6], ip_data, ip_len);
    }

    EnterCriticalSection(&ctx->send_cs);

    /* Allocate send buffer for IP packet only */
    SetLastError(0);
    packet = g_WintunFunctions.WintunAllocateSendPacket(ctx->session, (DWORD)ip_len);
    DWORD error = GetLastError();

    if (packet) {
        memcpy(packet, ip_data, ip_len);
        g_WintunFunctions.WintunSendPacket(ctx->session, packet);
        LeaveCriticalSection(&ctx->send_cs);
        traceEvent(TRACE_DEBUG, "wintun_write: eth frame %dB -> IP pkt %dB (type=0x%04X)",
                   len, ip_len, ethertype);
        return len;
    } else if (error == ERROR_BUFFER_OVERFLOW) {
        /* Ring buffer full, try again later */
        LeaveCriticalSection(&ctx->send_cs);
        return 0;
    } else {
        LeaveCriticalSection(&ctx->send_cs);
        traceEvent(TRACE_WARNING, "wintun_write: allocate failed, error=%lu", error);
    }

    return -1;
}

/* ***************************************************** */

/**
 * Close wintun adapter
 */
void wintun_close(_In_ tuntap_dev* device) {
    N2N_WINTUN_CONTEXT* ctx = (N2N_WINTUN_CONTEXT*)device->device_handle;

    if (!ctx) {
        return;
    }

    traceEvent(TRACE_INFO, "Closing wintun adapter '%s'", ctx->friendly_name);

    /* End session */
    if (ctx->session) {
        g_WintunFunctions.WintunEndSession(ctx->session);
    }

    /* Close adapter */
    if (ctx->adapter) {
        g_WintunFunctions.WintunCloseAdapter(ctx->adapter);
    }

    /* Cleanup */
    DeleteCriticalSection(&ctx->send_cs);
    DeleteCriticalSection(&ctx->arp_cs);
    DeleteCriticalSection(&ctx->neighbor_cs);
    free(ctx);

    device->device_handle = NULL;
}

/* ***************************************************** */

/**
 * Get address (placeholder for wintun - not applicable)
 */
void wintun_get_address(_In_ tuntap_dev* device) {
    /* Wintun adapters don't support dynamic address queries like TAP */
}

/* ***************************************************** */

/**
 * Check if wintun is available on this system
 * This only checks if wintun.dll can be loaded - the driver will be
 * automatically installed when the first adapter is created.
 */
BOOL wintun_available(void) {
    DWORD error;
    return IsWintunInstalled(&error);
}

#endif /* _WIN32 */
