/* sim_disk.h: simulator disk support library definitions */
// SPDX-FileCopyrightText: 2011 Mark Pizzolato
// SPDX-License-Identifier: MIT

/*
   Except as contained in this notice, the names of Robert M Supnik and
   Mark Pizzolato shall not be used in advertising or otherwise to promote
   the sale, use or other dealings in this Software without prior written
   authorization from Robert M Supnik and Mark Pizzolato.

   25-Jan-11    MP      Initial Implementation
*/

#ifndef SIM_DISK_H_
#define SIM_DISK_H_    1

#include <stdbool.h>
#include <stdint.h>

#include "sim_disk_ramdisk.h"

/* SIMH/Disk format */

typedef uint32_t        t_seccnt;                       /* disk sector count */
typedef uint32_t        t_lba;                          /* disk logical block address */

/* Unit flags */

#define DKUF_V_FMT      (UNIT_V_UF + 0)                 /* disk file format */
#define DKUF_W_FMT      2                               /* 2b of formats */
#define DKUF_M_FMT      ((1u << DKUF_W_FMT) - 1)
#define DKUF_F_AUTO      0                              /* Auto detect format format */
#define DKUF_F_STD       1                              /* SIMH format */
#define DKUF_F_RAW       2                              /* Raw Physical Disk Access */
#define DKUF_F_VHD       3                              /* VHD format */
#define DKUF_V_NOAUTOSIZE (DKUF_V_FMT + DKUF_W_FMT)     /* Don't Autosize disk option */
#define DKUF_V_UF       (DKUF_V_NOAUTOSIZE + 1)
#define DKUF_WLK        UNIT_WLK
#define DKUF_FMT        (DKUF_M_FMT << DKUF_V_FMT)
#define DKUF_WRP        (DKUF_WLK | UNIT_RO)
#define DKUF_NOAUTOSIZE (1 << DKUF_V_NOAUTOSIZE)

#define DK_F_STD        (DKUF_F_STD << DKUF_V_FMT)
#define DK_F_RAW        (DKUF_F_RAW << DKUF_V_FMT)
#define DK_F_VHD        (DKUF_F_VHD << DKUF_V_FMT)

#define DK_GET_FMT(u)   (((u)->flags >> DKUF_V_FMT) & DKUF_M_FMT)

/* Return status codes */

#define DKSE_OK         0                               /* no error */

typedef void (*DISK_PCALLBACK)(UNIT *unit, t_stat status);

/* Disk I/O context: */

typedef enum sim_disk_op_e {
    DOP_DONE,             /* close */
    DOP_RSEC,             /* sim_disk_rdsect_a */
    DOP_WSEC,             /* sim_disk_wrsect_a */
    DOP_IAVL,             /* sim_disk_isavailable_a */
    DOP_FLUSH,            /* Flush pending writes to the disk */
    DOP_IDLE,             /* Idle state, no operation pending */
} sim_disk_op_t;

struct disk_context {
    sim_off_t            container_size;     /* Size of the data portion (of the pseudo disk) */
    sim_off_t            highwater;          /* Furthest written sector in the disk */
    DEVICE              *dptr;              /* Device for unit (access to debug flags) */
    uint32_t            dbit;               /* debugging bit */
    uint32_t            sector_size;        /* Disk Sector Size (of the pseudo disk) */
    uint32_t            capac_factor;       /* Units of Capacity (8 = quadword, 2 = word, 1 = byte) */
    uint32_t            xfer_element_size;  /* Disk Bus Transfer size (1 - byte, 2 - word, 4 - longword) */
    uint32_t            storage_sector_size;/* Sector size of the containing storage */

    uint32_t            removable;          /* Removable device flag */
    uint32_t            is_cdrom;           /* Host system CDROM Device */
    uint32_t            media_removed;      /* Media not available flag */
    bool                auto_format;        /* Format determined dynamically */
    sim_disk_ramdisk    *ramdisk;           /* Volatile memory-backed disk */
    uint32_t            read_count;         /* Number of read operations performed */
    uint32_t            write_count;        /* Number of write operations performed */
    struct simh_disk_footer
                        *footer;
#if defined _WIN32
    HANDLE              disk_handle;        /* OS specific Raw device handle */
#endif
    bool                asynch_io;          /* Asynchronous Interrupt scheduling enabled */
    int                 asynch_io_latency;  /* instructions to delay pending interrupt */
    sim_mutex_t         lock;
    sim_thread_t        io_thread;          /* I/O Thread Id */
    sim_mutex_t         io_lock;
    sim_cond_t          io_cond;
    sim_cond_t          io_done;
    sim_cond_t          startup_cond;
    bool                io_thread_running;
    sim_disk_op_t       io_dop;
    uint8_t             *buf;
    t_seccnt            *rsects;
    t_seccnt            sects;
    t_lba               lba;
    DISK_PCALLBACK      callback;
    t_stat              io_status;
    };

#define disk_ctx up8                        /* Field in Unit structure which points to the disk_context */

/*
 * In-process test backend replacement hook.
 *
 * This is intended for controller and storage-layer tests that need to force
 * disk I/O behavior that is hard to create portably with host files, such as
 * read errors, write errors, or short transfers.
 *
 * This is not a stable storage plugin API. Overrides are process-local, are
 * not saved/restored, and are not safe to change while a UNIT has outstanding
 * asynchronous I/O. Any NULL operation falls through to the normal sim_disk
 * implementation. Tests should clear overrides during teardown.
 */
typedef struct sim_disk_test_backend {
    t_stat (*rdsect)(UNIT *uptr, t_lba lba, uint8_t *buf,
                     t_seccnt *sectsread, t_seccnt sects);
    t_stat (*wrsect)(UNIT *uptr, t_lba lba, uint8_t *buf,
                     t_seccnt *sectswritten, t_seccnt sects);
} SIM_DISK_TEST_BACKEND;

/* Prototypes */

t_stat sim_disk_init (void);
t_stat sim_disk_attach (UNIT *uptr,
                        const char *cptr,
                        size_t memory_sector_size,  /* memory footprint of sector data */
                        size_t xfer_element_size,
                        bool dontchangecapac,       /* if false just change uptr->capac as needed */
                        uint32_t debugbit,          /* debug bit */
                        const char *drivetype,      /* drive type */
                        uint32_t pdp11_tracksize,   /* BAD144 track */
                        int completion_delay);      /* Minimum Delay for asynch I/O completion */
t_stat sim_disk_attach_ex (UNIT *uptr,
                           const char *cptr,
                           size_t memory_sector_size,   /* memory footprint of sector data */
                           size_t xfer_element_size,
                           bool dontchangecapac,        /* if false just change uptr->capac as needed */
                           uint32_t dbit,               /* debug bit */
                           const char *dtype,           /* drive type */
                           uint32_t pdp11tracksize,     /* BAD144 track */
                           int completion_delay,        /* Minimum Delay for asynch I/O completion */
                           const char **drivetypes);    /* list of drive types (from smallest to largest) */
                                                        /* to try and fit the container/file system into */
t_stat sim_disk_attach_ex2 (UNIT *uptr,
                            const char *cptr,
                            size_t memory_sector_size,  /* memory footprint of sector data */
                            size_t xfer_element_size,
                            bool dontchangecapac,       /* if false just change uptr->capac as needed */
                            uint32_t dbit,              /* debug bit */
                            const char *dtype,          /* drive type */
                            uint32_t pdp11tracksize,    /* BAD144 track */
                            int completion_delay,       /* Minimum Delay for asynch I/O completion */
                            const char **drivetypes,    /* list of drive types (from smallest to largest) */
                                                        /* to try and fit the container/file system into */
                             size_t reserved_sectors);  /* Unused sectors beyond the file system */
t_stat sim_disk_set_test_backend (UNIT *uptr,
                                  const SIM_DISK_TEST_BACKEND *backend);
void sim_disk_clear_test_backend (UNIT *uptr);
void sim_disk_clear_all_test_backends (void);
t_stat sim_disk_detach (UNIT *uptr);
/* Persist ramdisk contents to its SAVE= image when this unit is a ramdisk. */
t_stat sim_disk_save_if_ramdisk (UNIT *uptr);
t_stat sim_disk_attach_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32_t flag, const char *cptr);
t_stat sim_disk_rdsect (UNIT *uptr, t_lba lba, uint8_t *buf, t_seccnt *sectsread, t_seccnt sects);
t_stat sim_disk_rdsect_a (UNIT *uptr, t_lba lba, uint8_t *buf, t_seccnt *sectsread, t_seccnt sects, DISK_PCALLBACK callback);
t_stat sim_disk_wrsect (UNIT *uptr, t_lba lba, uint8_t *buf, t_seccnt *sectswritten, t_seccnt sects);
t_stat sim_disk_wrsect_a (UNIT *uptr, t_lba lba, uint8_t *buf, t_seccnt *sectswritten, t_seccnt sects, DISK_PCALLBACK callback);
t_stat sim_disk_unload (UNIT *uptr);
t_stat sim_disk_erase (UNIT *uptr);
t_stat sim_disk_set_fmt (UNIT *uptr, int32_t val, const char *cptr, void *desc);
t_stat sim_disk_show_fmt (FILE *st, UNIT *uptr, int32_t val, const void *desc);
t_stat sim_disk_set_capac (UNIT *uptr, int32_t val, const char *cptr, void *desc);
t_stat sim_disk_show_capac (FILE *st, UNIT *uptr, int32_t val, const void *desc);
t_stat sim_disk_set_async (UNIT *uptr, int latency);
t_stat sim_disk_clr_async (UNIT *uptr);
t_stat sim_disk_reset (UNIT *uptr);
t_stat sim_disk_perror (UNIT *uptr, const char *msg);
t_stat sim_disk_clearerr (UNIT *uptr);
bool sim_disk_isavailable (UNIT *uptr);
bool sim_disk_isavailable_a (UNIT *uptr, DISK_PCALLBACK callback);
bool sim_disk_wrp (UNIT *uptr);
t_stat sim_disk_pdp11_bad_block (UNIT *uptr, int32_t sec, int32_t wds);
sim_off_t sim_disk_size (UNIT *uptr);
bool sim_disk_vhd_support (void);
bool sim_disk_raw_support (void);
void sim_disk_data_trace (UNIT *uptr, const uint8_t *data, size_t lba, size_t len, const char* txt, int detail, uint32_t reason);
t_stat sim_disk_info_cmd (int32_t flag, const char *ptr);
t_stat sim_disk_set_noautosize (int32_t flag, const char *cptr);
t_stat sim_disk_test (DEVICE *dptr, const char *cptr);

#endif
