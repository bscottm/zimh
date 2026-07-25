/* sim_raw_osfile.h: "Raw" (low-level) operating system file primitives */
/* Declarations, types and prototypes */

// SPDX-FileCopyrightText: 2026 The ZIMH Project
// SPDX-License-Identifier: X11

#if !defined(SIM_RAW_OSFILE_H_)
#    define SIM_RAW_OSFILE_H_

/* Positioned file read */
size_t sim_disk_pread(sim_raw_osfile_t handle, void *buf, size_t bytes, t_offset offset);
/* Positioned file write */
size_t sim_disk_pwrite (sim_raw_osfile_t handle, const void *buf, size_t bytes, t_offset offset);

sim_raw_osfile_t sim_disk_open_handle (const char *file);
sim_raw_osfile_t sim_fopen_handle (const char *file, const char *mode);
int sim_fclose_handle (sim_raw_osfile_t handle);
void sim_fflush_handle (sim_raw_osfile_t handle);

t_offset sim_fsize_handle (sim_raw_osfile_t handle);

#endif
