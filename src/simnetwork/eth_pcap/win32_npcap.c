// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: MIT

#define WINDOWS_LEAN_AND_MEAN

// winsock2.h MUST be included before windows.h. Don't ask why. It just has to be.
#include <winsock2.h>
#include <windows.h>
#include <pcap.h>

#include "scp.h"
#include "string_compat.h"

/* Dynamic DLL loading technique and modified source comes from
   Etherial/WireShark capture_pcap.c */

/* Dynamic DLL load variables */
static HINSTANCE hLib = NULL; /* handle to DLL */

static enum {
    NPCAP_NOT_LOADED,
    NPCAP_LOADED,
    NPCAP_NOT_FOUND,
    NPCAP_MISSING_FUNCTION
} lib_loaded = NPCAP_NOT_LOADED;

static const char *lib_name = "npcap.dll";

static char no_pcap[PCAP_ERRBUF_SIZE] =
    "wpcap.dll failed to load, install Npcap or a compatible pcap runtime to use pcap networking";

/* define pointers to pcap functions needed */
static void (*p_pcap_close)(pcap_t *);
static int (*p_pcap_compile)(pcap_t *, struct bpf_program *, const char *, int, bpf_u_int32);
static int (*p_pcap_datalink)(pcap_t *);
static int (*p_pcap_dispatch)(pcap_t *, int, pcap_handler, u_char *);
static int (*p_pcap_findalldevs)(pcap_if_t **, char *);
static void (*p_pcap_freealldevs)(pcap_if_t *);
static void (*p_pcap_freecode)(struct bpf_program *);
static char *(*p_pcap_geterr)(pcap_t *);
static char *(*p_pcap_lib_version)(void);
static int (*p_pcap_lookupnet)(const char *, bpf_u_int32 *, bpf_u_int32 *, char *);
static pcap_t *(*p_pcap_open_live)(const char *, int, int, int, char *);
static int (*p_pcap_setmintocopy)(pcap_t *handle, int);
static HANDLE (*p_pcap_getevent)(pcap_t *);
static int (*p_pcap_sendpacket)(pcap_t *handle, const u_char *msg, int len);
static int (*p_pcap_setfilter)(pcap_t *, struct bpf_program *);
static int (*p_pcap_setnonblock)(pcap_t *a, int nonblock, char *errbuf);

/* load function pointer from DLL */
typedef int (*_func)(void);

static void load_function(const char *function, _func *func_ptr)
{
    *func_ptr = (_func)((size_t)GetProcAddress(hLib, function));
    if (*func_ptr == 0) {
        sim_printf("Eth: Failed to find function '%s' in %s\n", function, lib_name);
        lib_loaded = NPCAP_MISSING_FUNCTION;
    }
}

/* load wpcap.dll as required */
static bool load_pcap(void)
{
    switch (lib_loaded) {
    case NPCAP_NOT_LOADED: /* not loaded */
        /* attempt to load DLL */
        BOOL(WINAPI * p_SetDllDirectory)(LPCTSTR);
        UINT(WINAPI * p_GetSystemDirectory)(LPTSTR lpBuffer, UINT uSize);

        p_SetDllDirectory =
            (BOOL(WINAPI *)(LPCTSTR))GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetDllDirectoryA");
        p_GetSystemDirectory =
            (UINT(WINAPI *)(LPTSTR, UINT))GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemDirectoryA");
        if (p_SetDllDirectory && p_GetSystemDirectory) {
            char npcap_path[512] = "";

            if (p_GetSystemDirectory(npcap_path, sizeof(npcap_path) - 7))
                strlcat(npcap_path, "\\Npcap", sizeof(npcap_path));
            if (p_SetDllDirectory(npcap_path))
                hLib = LoadLibraryA(lib_name);
            p_SetDllDirectory(NULL);
        }
        if (hLib == NULL)
            hLib = LoadLibraryA(lib_name);

        if (hLib == NULL) {
            /* failed to load DLL */
            lib_loaded = NPCAP_NOT_FOUND;
            break;
        }

        /* library loaded OK */
        lib_loaded = NPCAP_LOADED;

        /* load required functions; sets dll_load=3 on error */
        load_function("pcap_close", (_func *)&p_pcap_close);
        load_function("pcap_compile", (_func *)&p_pcap_compile);
        load_function("pcap_datalink", (_func *)&p_pcap_datalink);
        load_function("pcap_dispatch", (_func *)&p_pcap_dispatch);
        load_function("pcap_findalldevs", (_func *)&p_pcap_findalldevs);
        load_function("pcap_freealldevs", (_func *)&p_pcap_freealldevs);
        load_function("pcap_freecode", (_func *)&p_pcap_freecode);
        load_function("pcap_geterr", (_func *)&p_pcap_geterr);
        load_function("pcap_lookupnet", (_func *)&p_pcap_lookupnet);
        load_function("pcap_open_live", (_func *)&p_pcap_open_live);
        load_function("pcap_setmintocopy", (_func *)&p_pcap_setmintocopy);
        load_function("pcap_getevent", (_func *)&p_pcap_getevent);
        load_function("pcap_sendpacket", (_func *)&p_pcap_sendpacket);
        load_function("pcap_setfilter", (_func *)&p_pcap_setfilter);
        load_function("pcap_setnonblock", (_func *)&p_pcap_setnonblock);
        load_function("pcap_lib_version", (_func *)&p_pcap_lib_version);
        break;
    default: /* loaded or failed */
        break;
    }
    return (lib_loaded == NPCAP_LOADED);
}

/* define functions with dynamic revectoring */
void pcap_close(pcap_t *a)
{
    if (load_pcap()) {
        p_pcap_close(a);
    }
}

int pcap_compile(pcap_t *a, struct bpf_program *b, const char *c, int d, bpf_u_int32 e)
{
    if (load_pcap()) {
        return p_pcap_compile(a, b, c, d, e);
    } else {
        return 0;
    }
}

const char *pcap_lib_version(void)
{
    static char buf[256];

    if (load_pcap() && p_pcap_lib_version != NULL) {
        return p_pcap_lib_version();
    } else {
        snprintf(buf, sizeof(buf), "npcap or winpcap is not installed");
        return buf;
    }
}

int pcap_datalink(pcap_t *a)
{
    if (load_pcap()) {
        return p_pcap_datalink(a);
    } else {
        return 0;
    }
}

int pcap_dispatch(pcap_t *a, int b, pcap_handler c, u_char *d)
{
    if (load_pcap()) {
        return p_pcap_dispatch(a, b, c, d);
    } else {
        return 0;
    }
}

int pcap_findalldevs(pcap_if_t **a, char *b)
{
    if (load_pcap()) {
        return p_pcap_findalldevs(a, b);
    } else {
        *a = 0;
        strlcpy(b, no_pcap, PCAP_ERRBUF_SIZE);
        no_pcap[0] = '\0';
        return -1;
    }
}

void pcap_freealldevs(pcap_if_t *a)
{
    if (load_pcap()) {
        p_pcap_freealldevs(a);
    }
}

void pcap_freecode(struct bpf_program *a)
{
    if (load_pcap()) {
        p_pcap_freecode(a);
    }
}

char *pcap_geterr(pcap_t *a)
{
    if (load_pcap()) {
        return p_pcap_geterr(a);
    } else {
        return (char *)"";
    }
}

int pcap_lookupnet(const char *a, bpf_u_int32 *b, bpf_u_int32 *c, char *d)
{
    if (load_pcap()) {
        return p_pcap_lookupnet(a, b, c, d);
    } else {
        return 0;
    }
}

pcap_t *pcap_open_live(const char *a, int b, int c, int d, char *e)
{
    if (load_pcap()) {
        return p_pcap_open_live(a, b, c, d, e);
    } else {
        return (pcap_t *)0;
    }
}

int pcap_setmintocopy(pcap_t *a, int b)
{
    if (load_pcap()) {
        return p_pcap_setmintocopy(a, b);
    } else {
        return -1;
    }
}

HANDLE pcap_getevent(pcap_t *a)
{
    if (load_pcap()) {
        return p_pcap_getevent(a);
    } else {
        return (HANDLE)0;
    }
}

int pcap_sendpacket(pcap_t *a, const u_char *b, int c)
{
    if (load_pcap()) {
        return p_pcap_sendpacket(a, b, c);
    } else {
        return 0;
    }
}

int pcap_setfilter(pcap_t *a, struct bpf_program *b)
{
    if (load_pcap()) {
        return p_pcap_setfilter(a, b);
    } else {
        return 0;
    }
}

int pcap_setnonblock(pcap_t *a, int nonblock, char *errbuf)
{
    if (load_pcap()) {
        return p_pcap_setnonblock(a, nonblock, errbuf);
    } else {
        return 0;
    }
}
