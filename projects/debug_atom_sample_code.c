/* Sample implementation of debug atom registry with adaptive word size */

#include "sim_defs.h"
#include <string.h>
#include <stdlib.h>

/* ========================================================================
   Configuration: Adaptive word size based on HOST platform
   ======================================================================== */

/* Detect 64-bit host platform (not simulator word size!)
 *
 * NOTE: We use UINTPTR_MAX == UINT64_MAX as primary test because:
 *   - sizeof(unsigned int) is NOT reliable (can be 4 even on 64-bit)
 *   - UINTPTR_MAX matches pointer width, which is the real platform indicator
 *   - Fallback to common 64-bit platform macros for robustness
 */
#if UINTPTR_MAX == UINT64_MAX || defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    typedef uint64_t debug_word_t;
    #define DEBUG_WORD_BITS 64
    #define DEBUG_WORD_SHIFT 6      /* log2(64) */
    #define DEBUG_WORD_MASK 0x3F    /* 64 - 1 */
    #define DEBUG_WORD_FMT "0x%016" PRIx64
    #define DEBUG_PLATFORM_DESC "64-bit host"
#else
    typedef uint32_t debug_word_t;
    #define DEBUG_WORD_BITS 32
    #define DEBUG_WORD_SHIFT 5      /* log2(32) */
    #define DEBUG_WORD_MASK 0x1F    /* 32 - 1 */
    #define DEBUG_WORD_FMT "0x%08x"
    #define DEBUG_PLATFORM_DESC "32-bit host"
#endif

#define DEBUG_TABLE_SIZE 256
#define DEBUG_TABLE_MASK 0xFF
#define DEBUG_BITMAP_WORDS (DEBUG_TABLE_SIZE / DEBUG_WORD_BITS)

/* On 64-bit: 256 / 64 = 4 words = 32 bytes per device
   On 32-bit: 256 / 32 = 8 words = 32 bytes per device */

/* ========================================================================
   Data Structures
   ======================================================================== */

/* Global hash table element - stores atom metadata and bitmap coordinates */
struct deb_hash_table_elem {
    char *debug_atom;           /* strdup()-ed copy of atom name */
    char *description;          /* strdup()-ed description */
    uint32_t word_index;        /* Which word in bitmap array */
    debug_word_t bit_mask;      /* Bit mask within that word */
    uint32_t ref_count;         /* Number of devices using this atom */
};

/* Global registry */
static struct deb_hash_table_elem global_hash_table[DEBUG_TABLE_SIZE];

/* Global allocator state */
static struct {
    uint32_t next_word;         /* Next bitmap word to allocate from */
    uint32_t next_bit;          /* Next bit within that word */
    uint32_t atoms_allocated;   /* Total atoms registered */
    bool initialized;           /* Registry initialized flag */
} atom_allocator = {0, 0, 0, false};

/* ========================================================================
   Hash Function (FNV-1a)
   ======================================================================== */

static uint32_t fnv1a_hash(const char *str)
{
    uint32_t hash = 2166136261u;  /* FNV offset basis */
    const uint32_t prime = 16777619u;  /* FNV prime */

    while (*str) {
        hash ^= (uint32_t)(unsigned char)(*str++);
        hash *= prime;
    }

    return hash;
}

/* ========================================================================
   Bit Allocation
   ======================================================================== */

static t_stat allocate_bit_position(uint32_t *word_idx, debug_word_t *bit_mask)
{
    if (atom_allocator.atoms_allocated >= DEBUG_TABLE_SIZE) {
        return SCPE_MEM;  /* Table full */
    }

    *word_idx = atom_allocator.next_word;
    *bit_mask = ((debug_word_t)1) << atom_allocator.next_bit;

    /* Advance allocator */
    atom_allocator.next_bit++;
    if (atom_allocator.next_bit >= DEBUG_WORD_BITS) {
        atom_allocator.next_bit = 0;
        atom_allocator.next_word++;
    }
    atom_allocator.atoms_allocated++;

    return SCPE_OK;
}

/* ========================================================================
   Atom Registration
   ======================================================================== */

uint32_t register_debug_atom(const char *name, const char *desc)
{
    uint32_t hash;
    uint32_t idx;
    uint32_t probe;
    uint32_t word_idx;
    debug_word_t bit_mask;
    t_stat stat;

    if (!name || !*name)
        return 0;

    hash = fnv1a_hash(name);
    idx = hash & DEBUG_TABLE_MASK;

    /* Check if already registered */
    if (global_hash_table[idx].debug_atom) {
        if (strcmp(global_hash_table[idx].debug_atom, name) == 0) {
            /* Same atom re-registered (idempotent) */
            global_hash_table[idx].ref_count++;
            return hash;
        }

        /* Collision - linear probing */
        probe = (idx + 1) & DEBUG_TABLE_MASK;
        while (probe != idx) {
            if (!global_hash_table[probe].debug_atom) {
                idx = probe;
                break;
            }
            if (strcmp(global_hash_table[probe].debug_atom, name) == 0) {
                /* Found in probe chain */
                global_hash_table[probe].ref_count++;
                return hash;
            }
            probe = (probe + 1) & DEBUG_TABLE_MASK;
        }

        if (probe == idx) {
            /* Table full */
            return 0;
        }
    }

    /* Allocate bit position */
    stat = allocate_bit_position(&word_idx, &bit_mask);
    if (stat != SCPE_OK) {
        return 0;
    }

    /* Register new atom */
    global_hash_table[idx].debug_atom = strdup(name);
    global_hash_table[idx].description = desc ? strdup(desc) : NULL;
    global_hash_table[idx].word_index = word_idx;
    global_hash_table[idx].bit_mask = bit_mask;
    global_hash_table[idx].ref_count = 1;

    return hash;
}

/* ========================================================================
   Device Operations
   ======================================================================== */

/* Enable debug atom for device */
t_stat enable_debug_atom(DEVICE *dptr, uint32_t atom_hash)
{
    uint32_t idx = atom_hash & DEBUG_TABLE_MASK;
    uint32_t probe = idx;

    if (!dptr)
        return SCPE_ARG;

    /* Find atom (handle probing for collisions) */
    do {
        if (global_hash_table[probe].debug_atom) {
            uint32_t word_idx = global_hash_table[probe].word_index;
            debug_word_t mask = global_hash_table[probe].bit_mask;

            /* Enable bit in device's bitmap */
            dptr->debug_bitmap[word_idx] |= mask;
            return SCPE_OK;
        }
        probe = (probe + 1) & DEBUG_TABLE_MASK;
    } while (probe != idx);

    return SCPE_ARG;  /* Atom not found */
}

/* Disable debug atom for device */
t_stat disable_debug_atom(DEVICE *dptr, uint32_t atom_hash)
{
    uint32_t idx = atom_hash & DEBUG_TABLE_MASK;
    uint32_t probe = idx;

    if (!dptr)
        return SCPE_ARG;

    do {
        if (global_hash_table[probe].debug_atom) {
            uint32_t word_idx = global_hash_table[probe].word_index;
            debug_word_t mask = global_hash_table[probe].bit_mask;

            /* Disable bit in device's bitmap */
            dptr->debug_bitmap[word_idx] &= ~mask;
            return SCPE_OK;
        }
        probe = (probe + 1) & DEBUG_TABLE_MASK;
    } while (probe != idx);

    return SCPE_ARG;
}

/* Check if debug atom is enabled (for internal use) */
bool is_debug_enabled(DEVICE *dptr, uint32_t atom_hash)
{
    uint32_t idx = atom_hash & DEBUG_TABLE_MASK;
    uint32_t word_idx = global_hash_table[idx].word_index;
    debug_word_t mask = global_hash_table[idx].bit_mask;

    return (dptr->debug_bitmap[word_idx] & mask) != 0;
}

/* Enable all atoms matching pattern */
t_stat enable_debug_pattern(DEVICE *dptr, const char *pattern)
{
    uint32_t i;
    int count = 0;

    if (!dptr || !pattern)
        return SCPE_ARG;

    for (i = 0; i < DEBUG_TABLE_SIZE; i++) {
        if (global_hash_table[i].debug_atom) {
            /* Simple wildcard matching: "tape.*" matches "tape.position" */
            if (match_pattern(global_hash_table[i].debug_atom, pattern)) {
                uint32_t word_idx = global_hash_table[i].word_index;
                debug_word_t mask = global_hash_table[i].bit_mask;
                dptr->debug_bitmap[word_idx] |= mask;
                count++;
            }
        }
    }

    return count > 0 ? SCPE_OK : SCPE_ARG;
}

/* Disable all debug atoms for device */
void disable_all_debug(DEVICE *dptr)
{
    if (dptr)
        memset(dptr->debug_bitmap, 0, sizeof(dptr->debug_bitmap));
}

/* ========================================================================
   Display/Introspection
   ======================================================================== */

/* Show all registered atoms */
void show_debug_atoms(FILE *st)
{
    uint32_t i;

    fprintf(st, "\nRegistered debug atoms (%u total):\n",
            atom_allocator.atoms_allocated);
    fprintf(st, "Platform: %s, %u-bit words, %u words per device (%u bytes)\n\n",
            DEBUG_PLATFORM_DESC,
            (unsigned)DEBUG_WORD_BITS,
            (unsigned)DEBUG_BITMAP_WORDS,
            (unsigned)(DEBUG_BITMAP_WORDS * sizeof(debug_word_t)));

    for (i = 0; i < DEBUG_TABLE_SIZE; i++) {
        if (global_hash_table[i].debug_atom) {
            fprintf(st, "  %-30s  word[%u] mask=" DEBUG_WORD_FMT " refs=%u\n",
                    global_hash_table[i].debug_atom,
                    global_hash_table[i].word_index,
                    global_hash_table[i].bit_mask,
                    global_hash_table[i].ref_count);
            if (global_hash_table[i].description) {
                fprintf(st, "      %s\n", global_hash_table[i].description);
            }
        }
    }
}

/* Show enabled atoms for device */
void show_device_debug(FILE *st, DEVICE *dptr)
{
    uint32_t i;
    int count = 0;

    if (!dptr) {
        fprintf(st, "Invalid device\n");
        return;
    }

    fprintf(st, "\n%s debug atoms enabled:\n", dptr->name);

    for (i = 0; i < DEBUG_TABLE_SIZE; i++) {
        if (global_hash_table[i].debug_atom) {
            uint32_t word_idx = global_hash_table[i].word_index;
            debug_word_t mask = global_hash_table[i].bit_mask;

            if (dptr->debug_bitmap[word_idx] & mask) {
                fprintf(st, "  %s\n", global_hash_table[i].debug_atom);
                count++;
            }
        }
    }

    if (count == 0) {
        fprintf(st, "  (none)\n");
    }
}

/* ========================================================================
   Pattern Matching (stub - needs full implementation)
   ======================================================================== */

static bool match_pattern(const char *str, const char *pattern)
{
    /* TODO: Implement proper wildcard matching
       For now, simple prefix match for "tape.*" style patterns */

    if (strcmp(pattern, "*") == 0)
        return true;

    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen-1] == '*') {
        /* Prefix match */
        return strncmp(str, pattern, plen-1) == 0;
    }

    /* Exact match */
    return strcmp(str, pattern) == 0;
}
