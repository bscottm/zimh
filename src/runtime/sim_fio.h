/* sim_fio.h: simulator file I/O library headers */
// SPDX-FileCopyrightText: 1993-2008 Robert M Supnik
// SPDX-License-Identifier: X11

/*
   02-Feb-11    MP      Added sim_fsize_ex and sim_fsize_name_ex returning t_addr
                        Added export of sim_buf_copy_swapped and sim_buf_swap_data
   15-May-06    RMS     Added sim_fsize_name
   16-Aug-05    RMS     Fixed C++ declaration and cast problems
   02-Jan-04    RMS     Split out from SCP
*/

#ifndef SIM_FIO_H_
#    define SIM_FIO_H_ 1

/* t_stat's declaration is in scp.h */
#    include "scp.h"

#    include <sys/stat.h>
#    include <time.h>

#    define FLIP_SIZE (1 << 16) /* flip buf size */
#    define fxread(a, b, c, d) sim_fread(a, b, c, d)
#    define fxwrite(a, b, c, d) sim_fwrite(a, b, c, d)

/* t_offset -> sim_off_t (sim_platform.h) */

int32_t sim_finit(void);
FILE *sim_fopen(const char *file, const char *mode);
int sim_fseek(FILE *st, sim_off_t offset, int whence);
int sim_fseeko(FILE *st, sim_off_t offset, int whence);
bool sim_can_seek(FILE *st);
int sim_set_fsize(FILE *fptr, sim_off_t size);
t_stat sim_set_file_times(const char *file_name, time_t access_time, time_t write_time);
int sim_set_fifo_nonblock(FILE *fptr);
size_t sim_fread(void *bptr, size_t size, size_t count, FILE *fptr);
size_t sim_fwrite(const void *bptr, size_t size, size_t count, FILE *fptr);
sim_off_t sim_ftell(FILE *st);
sim_off_t sim_fsize_ex(FILE *fptr);
sim_off_t sim_fsize_name(const char *fname);
int sim_stat(const char *fname, struct stat *stat_str);
int sim_chdir(const char *path);
int sim_mkdir(const char *path);
int sim_rmdir(const char *path);
t_stat sim_copyfile(const char *source_file, const char *dest_file, bool overwrite_existing);
char *sim_filepath_parts(const char *pathname, const char *parts);
char *sim_getcwd(char *buf, size_t buf_size);
typedef void (*DIR_ENTRY_CALLBACK)(const char *directory, const char *filename, sim_off_t FileSize,
                                   const struct stat *filestat, void *context);
t_stat sim_dir_scan(const char *cptr, DIR_ENTRY_CALLBACK entry, void *context);
char **sim_get_filelist(const char *filename);
void sim_free_filelist(char ***pfilelist);
void sim_print_filelist(char **filelist);

void sim_buf_swap_data(void *bptr, size_t size, size_t count);
void sim_byte_swap_data(void *bptr, size_t size, size_t count);
void sim_buf_copy_swapped(void *dptr, const void *bptr, size_t size, size_t count);
const char *sim_get_os_error_text(int error);
typedef struct SHMEM SHMEM;
t_stat sim_shmem_open(const char *name, size_t size, SHMEM **shmem, void **addr);
void sim_shmem_close(SHMEM *shmem);
int32_t sim_shmem_atomic_add(int32_t *ptr, int32_t val);
bool sim_shmem_atomic_cas(int32_t *ptr, int32_t oldv, int32_t newv);

extern bool sim_taddr_64;   /* t_addr is > 32b and Large File Support available */
extern const bool sim_toffset_64; /* Large File (>2GB) file I/O support */
extern bool sim_end;        /* true = little endian, false = big endian */

char *sim_trim_endspc(char *cptr);

int sim_strwhitecasecmp(const char *string1, const char *string2, bool casecmp);

#endif
