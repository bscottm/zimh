/* vax_mmu.h - VAX memory management (inlined)

   Copyright (c) 1998-2013, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   29-Nov-13    RMS     Reworked unaligned flows
   24-Oct-12    MB      Added support for KA620 virtual addressing
   21-Jul-08    RMS     Removed inlining support
   28-May-08    RMS     Inlined physical memory routines
   29-Apr-07    RMS     Added address masking for system page table reads
   22-Sep-05    RMS     Fixed declarations (Sterling Garwood)
   30-Sep-04    RMS     Comment and formating changes
   19-Sep-03    RMS     Fixed upper/lower case linkage problems on VMS
   01-Jun-03    RMS     Fixed compilation problem with USE_ADDR64

   This module contains the instruction simulators for

        Read            -       read virtual
        Write           -       write virtual
        ReadL(P)        -       read aligned physical longword (physical context)
        WriteL(P)       -       write aligned physical longword (physical context)
        ReadB(W)        -       read aligned physical byte (word)
        WriteB(W)       -       write aligned physical byte (word)
        Test            -       test acccess

*/

#ifndef VAX_MMU_H_
#define VAX_MMU_H_ 1

#include <stdbool.h>
#include <stdint.h>

#include "vax_defs.h"

typedef struct {
    uint32_t    tag;                                    /* tag */
    uint32_t    pte;                                    /* pte */
    } TLBENT;

extern uint32_t *M;
extern UNIT cpu_unit;
extern DEVICE cpu_dev;
extern int32_t mapen;                                   /* map enable */

extern uint32_t mchk_va, mchk_ref;                      /* for mcheck */
extern TLBENT stlb[VA_TBSIZE], ptlb[VA_TBSIZE];

static const uint32_t insert[4] = {
    0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF
    };

void zap_tb (int stb);
void zap_tb_ent (uint32_t va);
bool chk_tb_ent (uint32_t va);
void set_map_reg (void);
int32_t ReadIO (uint32_t pa, int32_t lnt);
void WriteIO (uint32_t pa, int32_t val, int32_t lnt);
int32_t ReadReg (uint32_t pa, int32_t lnt);
void WriteReg (uint32_t pa, int32_t val, int32_t lnt);
TLBENT fill (uint32_t va, int32_t lnt, int32_t acc, int32_t *stat);

static inline uint32_t ReadU (uint32_t pa, int32_t lnt);
static inline void WriteU (uint32_t pa, int32_t val, int32_t lnt);

static inline uint32_t ReadB (uint32_t pa);
static inline uint32_t ReadW (uint32_t pa);
static SIM_FORCE_INLINE uint32_t ReadL (uint32_t pa);
static inline uint32_t ReadLP (uint32_t pa);

static inline void WriteB (uint32_t pa, int32_t val);
static inline void WriteW (uint32_t pa, int32_t val);
static SIM_FORCE_INLINE void WriteL(uint32_t pa, int32_t val);
static inline void WriteLP(uint32_t pa, int32_t val);

/* Unaligned reads and writes take the slow path, hence, are externs: */
uint32_t Read_Unaligned(uint32_t va, int32_t lnt, int32_t acc, uint32_t pa, uint32_t off);
void Write_Unaligned(uint32_t va, int32_t val, int32_t lnt, int32_t acc, uint32_t pa, uint32_t off);

/* Read and write virtual

   These routines logically fall into three phases:

   1.   Look up the virtual address in the translation buffer, calling
        the fill routine on a tag mismatch or access mismatch (invalid
        tlb entries have access = 0 and thus always mismatch).  The
        fill routine handles all errors.  If the resulting physical
        address is aligned, do an aligned physical read or write.
   2.   Test for unaligned across page boundaries.  If cross page, look
        up the physical address of the second page.  If not cross page,
        the second physical address is the same as the first.
   3.   Using the two physical addresses, do an unaligned read or
        write, with three cases: unaligned long, unaligned word within
        a longword, unaligned word crossing a longword boundary.

   Note that these routines do not handle quad or octa references.
*/

/* Read virtual

   Inputs:
        va      =       virtual address
        lnt     =       length code (BWL)
        acc     =       access code (KESU)
   Output:
        returned data, right justified in 32b longword
*/

static SIM_FORCE_INLINE uint32_t Read(uint32_t va, int32_t lnt, int32_t acc)
{
    mchk_va = va;
    uint32_t pa, off = 0;

    if (SIM_LIKELY(mapen)) {
        uint32_t vpn = VA_GETVPN(va);
        off = VA_GETOFF(va);
        uint32_t tbi = VA_GETTBI(vpn);

        TLBENT xpte = (va & VA_S0) ? stlb[tbi] : ptlb[tbi];
        bool tlb_miss =
            ((xpte.pte & acc) == 0) | (xpte.tag != vpn) | (((acc & TLB_WACC) != 0) & ((xpte.pte & TLB_M) == 0));

        if (SIM_UNLIKELY(tlb_miss)) {
            xpte = fill(va, lnt, acc, NULL);
        }
        pa = (xpte.pte & TLB_PFN) | off;
    } else {
        pa = va & PAMASK;
    }

    // Hot path: Fast aligned access check
    if (SIM_LIKELY((pa & (lnt - 1)) == 0)) {
        return ((lnt >= L_LONG) ? ReadL(pa) : ((lnt == L_WORD) ? ReadW(pa) : ReadB(pa)));
    }

    // Cold path for misaligned memory
    return Read_Unaligned(va, lnt, acc, pa, off);
}

/* Write virtual

   Inputs:
        va      =       virtual address
        val     =       data to be written, right justified in 32b lw
        lnt     =       length code (BWL)
        acc     =       access code (KESU)
   Output:
        none
*/
static SIM_FORCE_INLINE void Write(uint32_t va, int32_t val, int32_t lnt, int32_t acc)
{
    mchk_va = va;
    uint32_t pa, off = 0;

    if (SIM_LIKELY(mapen)) {
        uint32_t vpn = VA_GETVPN(va);
        off = VA_GETOFF(va);
        uint32_t tbi = VA_GETTBI(vpn);

        TLBENT xpte = (va & VA_S0) ? stlb[tbi] : ptlb[tbi];

        bool tlb_miss = ((xpte.pte & acc) == 0) | (xpte.tag != vpn) | ((xpte.pte & TLB_M) == 0);

        if (SIM_UNLIKELY(tlb_miss)) {
            xpte = fill(va, lnt, acc, NULL);
        }
        pa = (xpte.pte & TLB_PFN) | off;
    } else {
        pa = va & PAMASK;
    }

    if (SIM_LIKELY((pa & (lnt - 1)) == 0)) {
        if (lnt >= L_LONG) {
            WriteL(pa, val);
        } else {
            (lnt == L_WORD) ? WriteW(pa, val) : WriteB(pa, val);
        }
        return;
    }

    Write_Unaligned(va, val, lnt, acc, pa, off);
}

/* Test access to a byte (VAX PROBEx) */

static inline int32_t Test (uint32_t va, int32_t acc, int32_t *status)
{
uint32_t vpn, off, tbi;
TLBENT xpte;

*status = PR_OK;                                        /* assume ok */
if (mapen) {                                            /* mapping on? */
    vpn = VA_GETVPN (va);                               /* get vpn, off */
    off = VA_GETOFF (va);
    tbi = VA_GETTBI (vpn);
    xpte = (va & VA_S0)? stlb[tbi]: ptlb[tbi];          /* access tlb */
    if ((xpte.pte & acc) && (xpte.tag == vpn))          /* TB hit, acc ok? */
        return (xpte.pte & TLB_PFN) | off;
    xpte = fill (va, L_BYTE, acc, status);              /* fill TB */
    if (*status == PR_OK)
        return (xpte.pte & TLB_PFN) | off;
    else
        return -1;
    }
return va & PAMASK;                                     /* ret phys addr */
}

/* Read aligned physical (in virtual context, unless indicated)

   Inputs:
        pa      =       physical address, naturally aligned
   Output:
        returned data, right justified in 32b longword
*/

uint32_t ReadB(uint32_t pa)
{
    uint32_t dat;

    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        dat = M[pa >> 2];
    } else {
        mchk_ref = REF_V;
        dat = SIM_LIKELY(ADDR_IS_IO(pa)) ? ReadIO(pa, L_BYTE) : ReadReg(pa, L_BYTE);
    }
    return (dat >> ((pa & 3) << 3)) & BMASK;
}

uint32_t ReadW(uint32_t pa)
{
    uint32_t dat;

    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        dat = M[pa >> 2];
    } else {
        mchk_ref = REF_V;
        dat = SIM_LIKELY(ADDR_IS_IO(pa)) ? ReadIO(pa, L_WORD) : ReadReg(pa, L_WORD);
    }
    return (dat >> ((pa & 2) << 3)) & WMASK;  // Branchless: (pa & 2) << 3 = 0 or 16
}

uint32_t ReadL(uint32_t pa)
{
    if (SIM_LIKELY(ADDR_IS_MEM(pa)))
        return M[pa >> 2];

    mchk_ref = REF_V;
    return SIM_LIKELY(ADDR_IS_IO(pa)) ? ReadIO(pa, L_LONG) : ReadReg(pa, L_LONG);
}

uint32_t ReadLP(uint32_t pa)
{
    if (SIM_LIKELY(ADDR_IS_MEM(pa)))
        return M[pa >> 2];

    mchk_va = pa;
    mchk_ref = REF_P;
    return SIM_LIKELY(ADDR_IS_IO(pa)) ? ReadIO(pa, L_LONG) : ReadReg(pa, L_LONG);
}

/* Read unaligned physical (in virtual context)

   Inputs:
        pa      =       physical address
        lnt     =       length in bytes (1, 2, or 3)
   Output:
        returned data
*/

uint32_t ReadU(uint32_t pa, int32_t lnt)
{
    uint32_t dat;
    uint32_t sc = (pa & 3) << 3;

    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        dat = M[pa >> 2];
    } else {
        mchk_ref = REF_V;
        dat = SIM_LIKELY(ADDR_IS_IO(pa)) ? ReadIOU(pa, lnt) : ReadRegU(pa, lnt);
    }
    return (dat >> sc) & insert[lnt];
}

/* Write aligned physical (in virtual context, unless indicated)

   Inputs:
        pa      =       physical address, naturally aligned
        val     =       data to be written, right justified in 32b longword
   Output:
        none
*/

void WriteB(uint32_t pa, int32_t val)
{
    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        uint32_t id = pa >> 2;
        uint32_t sc = (pa & 3) << 3;
        uint32_t mask = 0xFFu << sc;
        M[id] = (M[id] & ~mask) | (((uint32_t)val << sc) & mask);
    } else {
        mchk_ref = REF_V;
        SIM_LIKELY(ADDR_IS_IO(pa)) ? WriteIO(pa, val, L_BYTE) : WriteReg(pa, val, L_BYTE);
    }
}

void WriteW(uint32_t pa, int32_t val)
{
    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        uint32_t id = pa >> 2;
        uint32_t sc = (pa & 2) << 3;  // Branchless: 0 or 16
        uint32_t mask = WMASK << sc;
        M[id] = (M[id] & ~mask) | (((uint32_t)val & WMASK) << sc);
    } else {
        mchk_ref = REF_V;
        SIM_LIKELY(ADDR_IS_IO(pa)) ? WriteIO(pa, val, L_WORD) : WriteReg(pa, val, L_WORD);
    }
}

void WriteL(uint32_t pa, int32_t val)
{
    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        M[pa >> 2] = val;
    } else {
        mchk_ref = REF_V;
        SIM_LIKELY(ADDR_IS_IO(pa)) ? WriteIO(pa, val, L_LONG) : WriteReg(pa, val, L_LONG);
    }
}

void WriteLP(uint32_t pa, int32_t val)
{
    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        M[pa >> 2] = val;
    } else {
        mchk_va = pa;
        mchk_ref = REF_P;
        SIM_LIKELY(ADDR_IS_IO(pa)) ? WriteIO(pa, val, L_LONG) : WriteReg(pa, val, L_LONG);
    }
}

/* Write unaligned physical (in virtual context)

   Inputs:
        pa      =       physical address
        val     =       data to be written, right justified in 32b longword
        lnt     =       length (1, 2, or 3 bytes)
   Output:
        none
*/

void WriteU(uint32_t pa, int32_t val, int32_t lnt)
{
    if (SIM_LIKELY(ADDR_IS_MEM(pa))) {
        uint32_t bo = pa & 3;
        uint32_t sc = bo << 3;
        uint32_t mask = insert[lnt] << sc;
        M[pa >> 2] = (M[pa >> 2] & ~mask) | ((((uint32_t)val) & insert[lnt]) << sc);
    } else {
        mchk_ref = REF_V;
        SIM_LIKELY(ADDR_IS_IO(pa)) ? WriteIOU(pa, val, lnt) : WriteRegU(pa, val, lnt);
    }
}

#endif /* VAX_MMU_H_ */
