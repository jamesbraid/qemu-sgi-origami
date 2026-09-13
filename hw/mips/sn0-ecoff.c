/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIPS ECOFF, the object format of SGI's pre-ELF toolchain, scoped to what
 * a firmware-style loader needs: a big-endian OMAGIC executable -- fixed
 * addresses, unrelocated -- whose sections load where their headers say.
 * The layout is documented publicly in binutils' COFF/ECOFF support.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/core/loader.h"
#include "sn0-ecoff.h"

#define ECOFF_MAGIC_MIPSEB        0x0163
#define ECOFF_FILEHDR_SIZE        20
#define ECOFF_FILEHDR_MAGIC_OFF   0x00   /* uint16 */
#define ECOFF_FILEHDR_NSCNS_OFF   0x02   /* uint16 */
#define ECOFF_FILEHDR_OPTHDR_OFF  0x10   /* uint16 */
#define ECOFF_AOUTHDR_OMAGIC      0407   /* fixed-address, unrelocated */
#define ECOFF_AOUTHDR_MAGIC_OFF   0x00   /* uint16, in the optional header */
#define ECOFF_AOUTHDR_ENTRY_OFF   0x10   /* uint32 */
#define ECOFF_AOUTHDR_GPVALUE_OFF 0x34   /* uint32, the o32 ABI gp base */
#define ECOFF_SCNHDR_SIZE         40
#define ECOFF_SCNHDR_VADDR_OFF    0x0c   /* uint32, in each section header */
#define ECOFF_SCNHDR_SIZE_OFF     0x10   /* uint32 */
#define ECOFF_SCNHDR_SCNPTR_OFF   0x14   /* uint32 */

const char *load_ecoff_strerror(ssize_t error)
{
    switch (error) {
    case ECOFF_LOAD_FAILED:
        return "failed to read the ECOFF image";
    case ECOFF_LOAD_NOT_ECOFF:
        return "the image is not big-endian MIPS ECOFF";
    case ECOFF_LOAD_BAD_HEADER:
        return "the ECOFF headers reach outside the file";
    case ECOFF_LOAD_NOT_OMAGIC:
        return "the ECOFF image is not an OMAGIC executable";
    default:
        return "unknown error";
    }
}

ssize_t load_ecoff(const char *filename, uint64_t *pentry, uint64_t *pgp,
                   uint64_t *lowaddr, uint64_t *highaddr, hwaddr addr_offset)
{
    g_autofree uint8_t *buf = NULL;
    int64_t file_size;
    unsigned int nscns, opthdr, i;
    uint64_t scnhdr_base;
    uint64_t low = UINT64_MAX, high = 0;
    ssize_t total = 0;

    file_size = get_image_size(filename, NULL);
    if (file_size < 0) {
        return ECOFF_LOAD_FAILED;
    }
    if (file_size < ECOFF_FILEHDR_SIZE) {
        return ECOFF_LOAD_NOT_ECOFF;
    }

    buf = g_malloc(file_size);
    if (load_image_size(filename, buf, file_size) != file_size) {
        return ECOFF_LOAD_FAILED;
    }

    if (lduw_be_p(buf + ECOFF_FILEHDR_MAGIC_OFF) != ECOFF_MAGIC_MIPSEB) {
        return ECOFF_LOAD_NOT_ECOFF;
    }

    /*
     * Every header offset is checked against the file before it is read: a
     * truncated or crafted image must fail here, not read past the buffer.
     */
    nscns = lduw_be_p(buf + ECOFF_FILEHDR_NSCNS_OFF);
    opthdr = lduw_be_p(buf + ECOFF_FILEHDR_OPTHDR_OFF);
    if (opthdr < ECOFF_AOUTHDR_GPVALUE_OFF + 4 ||
        (uint64_t)ECOFF_FILEHDR_SIZE + opthdr > (uint64_t)file_size) {
        return ECOFF_LOAD_BAD_HEADER;
    }
    scnhdr_base = ECOFF_FILEHDR_SIZE + opthdr;
    if (scnhdr_base + (uint64_t)nscns * ECOFF_SCNHDR_SIZE >
        (uint64_t)file_size) {
        return ECOFF_LOAD_BAD_HEADER;
    }

    if (lduw_be_p(buf + ECOFF_FILEHDR_SIZE + ECOFF_AOUTHDR_MAGIC_OFF) !=
        ECOFF_AOUTHDR_OMAGIC) {
        return ECOFF_LOAD_NOT_OMAGIC;
    }

    if (pentry) {
        *pentry = ldl_be_p(buf + ECOFF_FILEHDR_SIZE + ECOFF_AOUTHDR_ENTRY_OFF);
    }
    if (pgp) {
        *pgp = ldl_be_p(buf + ECOFF_FILEHDR_SIZE + ECOFF_AOUTHDR_GPVALUE_OFF);
    }

    for (i = 0; i < nscns; i++) {
        const uint8_t *scn = buf + scnhdr_base +
                             (uint64_t)i * ECOFF_SCNHDR_SIZE;
        uint32_t vaddr = ldl_be_p(scn + ECOFF_SCNHDR_VADDR_OFF);
        uint32_t size = ldl_be_p(scn + ECOFF_SCNHDR_SIZE_OFF);
        uint32_t scnptr = ldl_be_p(scn + ECOFF_SCNHDR_SCNPTR_OFF);
        g_autofree char *label = g_strdup_printf("ecoff #%u: %s", i, filename);

        if (size == 0) {
            continue;
        }
        if (scnptr != 0) {
            /* Real contents -- text, rdata, data. Copy as the header says. */
            if ((uint64_t)scnptr + size > (uint64_t)file_size) {
                return ECOFF_LOAD_BAD_HEADER;
            }
            rom_add_blob_fixed(label, buf + scnptr, size,
                               addr_offset + vaddr);
        } else {
            /* bss and its kin: zero-filled, nothing in the file to copy. */
            g_autofree uint8_t *zero = g_malloc0(size);

            rom_add_blob_fixed(label, zero, size, addr_offset + vaddr);
        }

        low = MIN(low, (uint64_t)vaddr);
        high = MAX(high, (uint64_t)vaddr + size);
        total += size;
    }

    if (high == 0) {
        return ECOFF_LOAD_BAD_HEADER;
    }
    if (lowaddr) {
        *lowaddr = low;
    }
    if (highaddr) {
        *highaddr = high;
    }
    return total;
}
