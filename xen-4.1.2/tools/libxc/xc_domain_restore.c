/******************************************************************************
 * xc_domain_restore.c
 *
 * Restore the state of a guest session.
 *
 * Copyright (c) 2003, K A Fraser.
 * Copyright (c) 2006, Intel Corporation
 * Copyright (c) 2007, XenSource Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <malloc.h>

#include "xg_private.h"
#include "xg_save_restore.h"
#include "xc_dom.h"

#include <xen/hvm/ioreq.h>
#include <xen/hvm/params.h>
#include "../libxl/mc_migration_helper.h"
#include "./mc_ssl/cipher.h"
#include "./mc_ssl/mc_ssl.h"

/* 
 * Roger/classicsong
 * debug printf
 * hint printf
 */
#define dprintf(_f, _a...) \
    if (mc_migrate_debug == 1) fprintf(stderr, _f, ## _a)
#define hprintf(_f, _a...) \
    if (mc_migrate_hint == 1) ( fprintf(stderr, "Restore:" _f, ## _a), fflush(stderr) )
#define ffprintf(_file, _f, _a...) \
	( fprintf(_file, _f, ## _a), fflush(_file) )

uint32_t mc_dom = 0;
xc_interface *mc_xch = NULL;
struct restore_ctx *mc_ctx = NULL;
// End of Transfter String
//static char *mc_end_string = "End of Translation";

struct restore_ctx {
    unsigned long max_mfn; /* max mfn of the current host machine */
    unsigned long hvirt_start; /* virtual starting address of the hypervisor */
    unsigned int pt_levels; /* #levels of page tables used by the current guest */
    unsigned long nr_pfns; /* number of 'in use' pfns in the guest (i.e. #P2M entries with a valid mfn) */
    xen_pfn_t *live_p2m; /* Live mapping of the table mapping each PFN to its current MFN. */
    xen_pfn_t *p2m; /* A table mapping each PFN to its new MFN. */
    xen_pfn_t *p2m_batch; /* A table of P2M mappings in the current region.  */
    int completed; /* Set when a consistent image is available */
    int last_checkpoint; /* Set when we should commit to the current checkpoint when it completes. */
    struct domain_info_context dinfo;
};

#define HEARTBEAT_MS 1000

#ifndef __MINIOS__
static ssize_t rdexact(xc_interface *xch, struct restore_ctx *ctx,
                       int fd, void* buf, size_t size)
{
    size_t offset = 0;
    ssize_t len;
    struct timeval tv;
    fd_set rfds;

    while ( offset < size )
    {
        if ( ctx->completed ) {
            /* expect a heartbeat every HEARBEAT_MS ms maximum */
            tv.tv_sec = HEARTBEAT_MS / 1000;
            tv.tv_usec = (HEARTBEAT_MS % 1000) * 1000;

            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            len = select(fd + 1, &rfds, NULL, NULL, &tv);
            if ( len == -1 && errno == EINTR )
                continue;
            if ( !FD_ISSET(fd, &rfds) ) {
                ERROR("read_exact_timed failed (select returned %zd)", len);
                errno = ETIMEDOUT;
                return -1;
            }
        }

        len = read(fd, buf + offset, size - offset);
        if ( (len == -1) && ((errno == EINTR) || (errno == EAGAIN)) )
            continue;
        if ( len == 0 ) {
            ERROR("0-length read");
            errno = 0;
        }
        if ( len <= 0 ) {
            ERROR("read_exact_timed failed (read rc: %d, errno: %d)", len, errno);
            return -1;
        }
        offset += len;
    }

    return 0;
}

static ssize_t mc_rdexact(xc_interface *xch, struct restore_ctx *ctx,
                       int fd, void* buf, size_t size)
{
    size_t offset = 0;
    ssize_t len;
    struct timeval tv;
    fd_set rfds;

    while ( offset < size )
    {
        if ( ctx->completed ) {
            /* expect a heartbeat every HEARBEAT_MS ms maximum */
            tv.tv_sec = HEARTBEAT_MS / 1000;
            tv.tv_usec = (HEARTBEAT_MS % 1000) * 1000;

            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            len = select(fd + 1, &rfds, NULL, NULL, &tv);
            if ( len == -1 && errno == EINTR )
                continue;
            if ( !FD_ISSET(fd, &rfds) ) {
                ERROR("read_exact_timed failed (select returned %zd)", len);
                errno = ETIMEDOUT;
                return -1;
            }
        }

        len = read(fd, buf + offset, size - offset);
        if ( (len == -1) && ((errno == EINTR) || (errno == EAGAIN)) )
            continue;
        /*if ( len == 0 ) {
            ERROR("0-length read");
            errno = 0;
        }
        if ( len <= 0 ) {
            ERROR("read_exact_timed failed (read rc: %d, errno: %d)", len, errno);
            return -1;
        }*/
        offset += len;
    }

    return 0;
}


#define RDEXACT(fd,buf,size) rdexact(xch, ctx, fd, buf, size)
#define MCRDEXACT(fd,buf,size) mc_rdexact(xch, ctx, fd, buf, size)
#else
#define RDEXACT read_exact
#endif
/*
** In the state file (or during transfer), all page-table pages are
** converted into a 'canonical' form where references to actual mfns
** are replaced with references to the corresponding pfns.
** This function inverts that operation, replacing the pfn values with
** the (now known) appropriate mfn values.
*/
static int uncanonicalize_pagetable(
    xc_interface *xch, uint32_t dom, struct restore_ctx *ctx, void *page)
{
    int i, pte_last, nr_mfns = 0;
    unsigned long pfn;
    uint64_t pte;
    struct domain_info_context *dinfo = &ctx->dinfo;

    pte_last = PAGE_SIZE / ((ctx->pt_levels == 2)? 4 : 8);

    /* First pass: work out how many (if any) MFNs we need to alloc */
    for ( i = 0; i < pte_last; i++ )
    {
        if ( ctx->pt_levels == 2 )
            pte = ((uint32_t *)page)[i];
        else
            pte = ((uint64_t *)page)[i];

        /* XXX SMH: below needs fixing for PROT_NONE etc */
        if ( !(pte & _PAGE_PRESENT) )
            continue;
        
        pfn = (pte >> PAGE_SHIFT) & MFN_MASK_X86;
        
        if ( pfn >= dinfo->p2m_size )
        {
            /* This "page table page" is probably not one; bail. */
            ERROR("Frame number in page table is out of range: "
                  "i=%d pfn=0x%lx p2m_size=%lu",
                  i, pfn, dinfo->p2m_size);
            return 0;
        }
        
        if ( ctx->p2m[pfn] == INVALID_P2M_ENTRY )
        {
            /* Have a 'valid' PFN without a matching MFN - need to alloc */
            ctx->p2m_batch[nr_mfns++] = pfn; 
            ctx->p2m[pfn]--;
        }
    }

    /* Allocate the requisite number of mfns. */
    if ( nr_mfns &&
         (xc_domain_populate_physmap_exact(xch, dom, nr_mfns, 0, 0,
                                            ctx->p2m_batch) != 0) )
    { 
        ERROR("Failed to allocate memory for batch.!\n"); 
        errno = ENOMEM;
        return 0; 
    }
    
    /* Second pass: uncanonicalize each present PTE */
    nr_mfns = 0;
    for ( i = 0; i < pte_last; i++ )
    {
        if ( ctx->pt_levels == 2 )
            pte = ((uint32_t *)page)[i];
        else
            pte = ((uint64_t *)page)[i];
        
        /* XXX SMH: below needs fixing for PROT_NONE etc */
        if ( !(pte & _PAGE_PRESENT) )
            continue;
        
        pfn = (pte >> PAGE_SHIFT) & MFN_MASK_X86;

        if ( ctx->p2m[pfn] == (INVALID_P2M_ENTRY-1) )
            ctx->p2m[pfn] = ctx->p2m_batch[nr_mfns++];

        pte &= ~MADDR_MASK_X86;
        pte |= (uint64_t)ctx->p2m[pfn] << PAGE_SHIFT;

        if ( ctx->pt_levels == 2 )
            ((uint32_t *)page)[i] = (uint32_t)pte;
        else
            ((uint64_t *)page)[i] = (uint64_t)pte;
    }

    return 1;
}


/* Load the p2m frame list, plus potential extended info chunk */
static xen_pfn_t *load_p2m_frame_list(
    xc_interface *xch, struct restore_ctx *ctx,
    int io_fd, int *pae_extended_cr3, int *ext_vcpucontext,
    int *vcpuextstate, uint32_t *vcpuextstate_size)
{
    xen_pfn_t *p2m_frame_list;
    vcpu_guest_context_any_t ctxt;
    xen_pfn_t p2m_fl_zero;
    struct domain_info_context *dinfo = &ctx->dinfo;

    /* Read first entry of P2M list, or extended-info signature (~0UL). */
    if ( RDEXACT(io_fd, &p2m_fl_zero, sizeof(long)) )
    {
        PERROR("read extended-info signature failed");
        return NULL;
    }
    
    if ( p2m_fl_zero == ~0UL )
    {
        uint32_t tot_bytes;
        
        /* Next 4 bytes: total size of following extended info. */
        if ( RDEXACT(io_fd, &tot_bytes, sizeof(tot_bytes)) )
        {
            PERROR("read extended-info size failed");
            return NULL;
        }
        
        while ( tot_bytes )
        {
            uint32_t chunk_bytes;
            char     chunk_sig[4];
            
            /* 4-character chunk signature + 4-byte remaining chunk size. */
            if ( RDEXACT(io_fd, chunk_sig, sizeof(chunk_sig)) ||
                 RDEXACT(io_fd, &chunk_bytes, sizeof(chunk_bytes)) ||
                 (tot_bytes < (chunk_bytes + 8)) )
            {
                PERROR("read extended-info chunk signature failed");
                return NULL;
            }
            tot_bytes -= 8;

            /* VCPU context structure? */
            if ( !strncmp(chunk_sig, "vcpu", 4) )
            {
                /* Pick a guest word-size and PT depth from the ctxt size */
                if ( chunk_bytes == sizeof (ctxt.x32) )
                {
                    dinfo->guest_width = 4;
                    if ( ctx->pt_levels > 2 ) 
                        ctx->pt_levels = 3; 
                }
                else if ( chunk_bytes == sizeof (ctxt.x64) )
                {
                    dinfo->guest_width = 8;
                    ctx->pt_levels = 4;
                }
                else 
                {
                    ERROR("bad extended-info context size %d", chunk_bytes);
                    return NULL;
                }

                if ( RDEXACT(io_fd, &ctxt, chunk_bytes) )
                {
                    PERROR("read extended-info vcpu context failed");
                    return NULL;
                }
                tot_bytes -= chunk_bytes;
                chunk_bytes = 0;

                if ( GET_FIELD(&ctxt, vm_assist) 
                     & (1UL << VMASST_TYPE_pae_extended_cr3) )
                    *pae_extended_cr3 = 1;
            }
            else if ( !strncmp(chunk_sig, "extv", 4) )
            {
                *ext_vcpucontext = 1;
            }
            else if ( !strncmp(chunk_sig, "xcnt", 4) )
            {
                *vcpuextstate = 1;
                RDEXACT(io_fd, vcpuextstate_size, sizeof(*vcpuextstate_size));
                tot_bytes -= chunk_bytes;
                chunk_bytes = 0;
            }
            
            /* Any remaining bytes of this chunk: read and discard. */
            while ( chunk_bytes )
            {
                unsigned long sz = MIN(chunk_bytes, sizeof(xen_pfn_t));
                if ( RDEXACT(io_fd, &p2m_fl_zero, sz) )
                {
                    PERROR("read-and-discard extended-info chunk bytes failed");
                    return NULL;
                }
                chunk_bytes -= sz;
                tot_bytes   -= sz;
            }
        }

        /* Now read the real first entry of P2M list. */
        if ( RDEXACT(io_fd, &p2m_fl_zero, sizeof(xen_pfn_t)) )
        {
            PERROR("read first entry of p2m_frame_list failed");
            return NULL;
        }
    }

    /* Now that we know the guest's word-size, can safely allocate 
     * the p2m frame list */
    if ( (p2m_frame_list = malloc(P2M_TOOLS_FL_SIZE)) == NULL )
    {
        ERROR("Couldn't allocate p2m_frame_list array");
        return NULL;
    }

    /* First entry has already been read. */
    p2m_frame_list[0] = p2m_fl_zero;
    if ( RDEXACT(io_fd, &p2m_frame_list[1], 
                 (P2M_FL_ENTRIES - 1) * sizeof(xen_pfn_t)) )
    {
        PERROR("read p2m_frame_list failed");
        return NULL;
    }
    
    return p2m_frame_list;
}

typedef struct {
    int ishvm;
    union {
        struct tailbuf_pv {
            unsigned int pfncount;
            unsigned long* pfntab;
            unsigned int vcpucount;
            unsigned char* vcpubuf;
            unsigned char shared_info_page[PAGE_SIZE];
        } pv;
        struct tailbuf_hvm {
            uint64_t magicpfns[3];
            uint32_t hvmbufsize, reclen;
            uint8_t* hvmbuf;
            struct {
                uint32_t magic;
                uint32_t version;
                uint64_t len;
            } qemuhdr;
            uint32_t qemubufsize;
            uint8_t* qemubuf;
        } hvm;
    } u;
} tailbuf_t;

/* read stream until EOF, growing buffer as necssary */
static int compat_buffer_qemu(xc_interface *xch, struct restore_ctx *ctx,
                              int fd, struct tailbuf_hvm *buf)
{
    uint8_t *qbuf, *tmp;
    int blen = 0, dlen = 0;
    int rc;

    /* currently save records tend to be about 7K */
    blen = 8192;
    if ( !(qbuf = malloc(blen)) ) {
        ERROR("Error allocating QEMU buffer");
        return -1;
    }

    while( (rc = read(fd, qbuf+dlen, blen-dlen)) > 0 ) {
        DPRINTF("Read %d bytes of QEMU data\n", rc);
        dlen += rc;

        if (dlen == blen) {
            DPRINTF("%d-byte QEMU buffer full, reallocating...\n", dlen);
            blen += 4096;
            tmp = realloc(qbuf, blen);
            if ( !tmp ) {
                ERROR("Error growing QEMU buffer to %d bytes", blen);
                free(qbuf);
                return -1;
            }
            qbuf = tmp;
        }
    }

    if ( rc < 0 ) {
        ERROR("Error reading QEMU data");
        free(qbuf);
        return -1;
    }

    if ( memcmp(qbuf, "QEVM", 4) ) {
        ERROR("Invalid QEMU magic: 0x%08x", *(unsigned long*)qbuf);
        free(qbuf);
        return -1;
    }

    buf->qemubuf = qbuf;
    buf->qemubufsize = dlen;

    return 0;
}

static int buffer_qemu(xc_interface *xch, struct restore_ctx *ctx,
                       int fd, struct tailbuf_hvm *buf)
{
    uint32_t qlen;
    uint8_t *tmp;

    if ( RDEXACT(fd, &qlen, sizeof(qlen)) ) {
        PERROR("Error reading QEMU header length");
        return -1;
    }

    if ( qlen > buf->qemubufsize ) {
        if ( buf->qemubuf) {
            tmp = realloc(buf->qemubuf, qlen);
            if ( tmp )
                buf->qemubuf = tmp;
            else {
                ERROR("Error reallocating QEMU state buffer");
                return -1;
            }
        } else {
            buf->qemubuf = malloc(qlen);
            if ( !buf->qemubuf ) {
                ERROR("Error allocating QEMU state buffer");
                return -1;
            }
        }
    }
    buf->qemubufsize = qlen;

    if ( RDEXACT(fd, buf->qemubuf, buf->qemubufsize) ) {
        PERROR("Error reading QEMU state");
        return -1;
    }

    return 0;
}

static int dump_qemu(xc_interface *xch, uint32_t dom, struct tailbuf_hvm *buf)
{
    int saved_errno;
    char path[256];
    FILE *fp;

    sprintf(path, XC_DEVICE_MODEL_RESTORE_FILE".%u", dom);
    fp = fopen(path, "wb");
    if ( !fp )
        return -1;

    DPRINTF("Writing %d bytes of QEMU data\n", buf->qemubufsize);
    if ( fwrite(buf->qemubuf, 1, buf->qemubufsize, fp) != buf->qemubufsize) {
        saved_errno = errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    fclose(fp);

    return 0;
}

static int buffer_tail_hvm(xc_interface *xch, struct restore_ctx *ctx,
                           struct tailbuf_hvm *buf, int fd,
                           unsigned int max_vcpu_id, uint64_t vcpumap,
                           int ext_vcpucontext,
                           int vcpuextstate, uint32_t vcpuextstate_size)
{
    uint8_t *tmp;
    unsigned char qemusig[21];

    if ( RDEXACT(fd, buf->magicpfns, sizeof(buf->magicpfns)) ) {
        PERROR("Error reading magic PFNs");
        return -1;
    }
	fprintf(stderr, "magicpfn is %ld, %ld, %ld\n", buf->magicpfns[0], buf->magicpfns[1], buf->magicpfns[2]);

	fprintf(stderr, "Read reclen\n");
    if ( RDEXACT(fd, &buf->reclen, sizeof(buf->reclen)) ) {
        PERROR("Error reading HVM params size");
        return -1;
    }

    if ( buf->reclen > buf->hvmbufsize ) {
        if ( buf->hvmbuf) {
            tmp = realloc(buf->hvmbuf, buf->reclen);
            if ( tmp ) {
                buf->hvmbuf = tmp;
                buf->hvmbufsize = buf->reclen;
            } else {
                ERROR("Error reallocating HVM param buffer");
                return -1;
            }
        } else {
            buf->hvmbuf = malloc(buf->reclen);
            if ( !buf->hvmbuf ) {
                ERROR("Error allocating HVM param buffer");
                return -1;
            }
            buf->hvmbufsize = buf->reclen;
        }
    }

	fprintf(stderr, "Read hvmbuf, buf->reclen = %d\n", buf->reclen);
    if ( RDEXACT(fd, buf->hvmbuf, buf->reclen) ) {
        PERROR("Error reading HVM params");
        return -1;
    }

	fprintf(stderr, "Read qemusig\n");
    if ( RDEXACT(fd, qemusig, sizeof(qemusig)) ) {
        PERROR("Error reading QEMU signature");
        return -1;
    }

	fprintf(stderr, "Qemusig: %s\n", qemusig);
    /* The legacy live-migration QEMU record has no length information.
     * Short of reimplementing the QEMU parser, we're forced to just read
     * until EOF.
     *
     * Gets around this by sending a different signatures for the new
     * live-migration QEMU record and Remus which includes a length
     * prefix
     */
    if ( !memcmp(qemusig, "QemuDeviceModelRecord", sizeof(qemusig)) )
        return compat_buffer_qemu(xch, ctx, fd, buf);
    else if ( !memcmp(qemusig, "DeviceModelRecord0002", sizeof(qemusig)) ||
              !memcmp(qemusig, "RemusDeviceModelState", sizeof(qemusig)) )
        return buffer_qemu(xch, ctx, fd, buf);

    qemusig[20] = '\0';
    ERROR("Invalid QEMU signature: %s", qemusig);
    return -1;
}

static int buffer_tail_pv(xc_interface *xch, struct restore_ctx *ctx,
                          struct tailbuf_pv *buf, int fd,
                          unsigned int max_vcpu_id, uint64_t vcpumap,
                          int ext_vcpucontext,
                          int vcpuextstate,
                          uint32_t vcpuextstate_size)
{
    unsigned int i;
    size_t pfnlen, vcpulen;
    struct domain_info_context *dinfo = &ctx->dinfo;

    /* TODO: handle changing pfntab and vcpu counts */
    /* PFN tab */
    if ( RDEXACT(fd, &buf->pfncount, sizeof(buf->pfncount)) ||
         (buf->pfncount > (1U << 28)) ) /* up to 1TB of address space */
    {
        PERROR("Error when reading pfn count");
        return -1;
    }
    pfnlen = sizeof(unsigned long) * buf->pfncount;
    if ( !(buf->pfntab) ) {
        if ( !(buf->pfntab = malloc(pfnlen)) ) {
            ERROR("Error allocating PFN tail buffer");
            return -1;
        }
    }
    // DPRINTF("Reading PFN tab: %d bytes\n", pfnlen);
    if ( RDEXACT(fd, buf->pfntab, pfnlen) ) {
        PERROR("Error when reading pfntab");
        goto free_pfntab;
    }

    /* VCPU contexts */
    buf->vcpucount = 0;
    for (i = 0; i <= max_vcpu_id; i++) {
        // DPRINTF("vcpumap: %llx, cpu: %d, bit: %llu\n", vcpumap, i, (vcpumap % (1ULL << i)));
        if ( (!(vcpumap & (1ULL << i))) )
            continue;
        buf->vcpucount++;
    }
    // DPRINTF("VCPU count: %d\n", buf->vcpucount);
    vcpulen = ((dinfo->guest_width == 8) ? sizeof(vcpu_guest_context_x86_64_t)
               : sizeof(vcpu_guest_context_x86_32_t)) * buf->vcpucount;
    if ( ext_vcpucontext )
        vcpulen += 128 * buf->vcpucount;
    if ( vcpuextstate ) {
        vcpulen += vcpuextstate_size * buf->vcpucount;
    }

    if ( !(buf->vcpubuf) ) {
        if ( !(buf->vcpubuf = malloc(vcpulen)) ) {
            ERROR("Error allocating VCPU ctxt tail buffer");
            goto free_pfntab;
        }
    }
    // DPRINTF("Reading VCPUS: %d bytes\n", vcpulen);
    if ( RDEXACT(fd, buf->vcpubuf, vcpulen) ) {
        PERROR("Error when reading ctxt");
        goto free_vcpus;
    }

    /* load shared_info_page */
    // DPRINTF("Reading shared info: %lu bytes\n", PAGE_SIZE);
    if ( RDEXACT(fd, buf->shared_info_page, PAGE_SIZE) ) {
        PERROR("Error when reading shared info page");
        goto free_vcpus;
    }

    return 0;

  free_vcpus:
    if (buf->vcpubuf) {
        free (buf->vcpubuf);
        buf->vcpubuf = NULL;
    }
  free_pfntab:
    if (buf->pfntab) {
        free (buf->pfntab);
        buf->pfntab = NULL;
    }

    return -1;
}

static int buffer_tail(xc_interface *xch, struct restore_ctx *ctx,
                       tailbuf_t *buf, int fd, unsigned int max_vcpu_id,
                       uint64_t vcpumap, int ext_vcpucontext,
                       int vcpuextstate, uint32_t vcpuextstate_size)
{
	fprintf(stderr, "In buffer_tail\n");
    if ( buf->ishvm ){
		fprintf(stderr, "buffer_tail_hvm\n");
        return buffer_tail_hvm(xch, ctx, &buf->u.hvm, fd, max_vcpu_id, vcpumap,
                               ext_vcpucontext, vcpuextstate,
                               vcpuextstate_size);
	}
    else {
		fprintf(stderr, "buffer_tail_pt\n");
        return buffer_tail_pv(xch, ctx, &buf->u.pv, fd, max_vcpu_id, vcpumap,
                              ext_vcpucontext, vcpuextstate,
                              vcpuextstate_size);
	}
}

static void tailbuf_free_hvm(struct tailbuf_hvm *buf)
{
    if ( buf->hvmbuf ) {
        free(buf->hvmbuf);
        buf->hvmbuf = NULL;
    }
    if ( buf->qemubuf ) {
        free(buf->qemubuf);
        buf->qemubuf = NULL;
    }
}

static void tailbuf_free_pv(struct tailbuf_pv *buf)
{
    if ( buf->vcpubuf ) {
        free(buf->vcpubuf);
        buf->vcpubuf = NULL;
    }
    if ( buf->pfntab ) {
        free(buf->pfntab);
        buf->pfntab = NULL;
    }
}

static void tailbuf_free(tailbuf_t *buf)
{
    if ( buf->ishvm )
        tailbuf_free_hvm(&buf->u.hvm);
    else
        tailbuf_free_pv(&buf->u.pv);
}


static int pagebuf_init(pagebuf_t* buf)
{
    memset(buf, 0, sizeof(*buf));
    return 0;
}

static void pagebuf_free(pagebuf_t* buf)
{
    if (buf->pages) {
        free(buf->pages);
        buf->pages = NULL;
    }
    if(buf->pfn_types) {
        free(buf->pfn_types);
        buf->pfn_types = NULL;
    }
}

static int ssl_rdexact(struct ssl_wrap *ssl, int fd, void* data, size_t size, 
		xc_interface *xch, struct restore_ctx *ctx)
{
	int new_size;
	if ((new_size = read_size_adjust(ssl, size)) < 0) {
		return -1;
	}
	RDEXACT(fd, ssl->ssl_buf, new_size);
	ssl_decrypt(ssl, data, new_size, size);
	return 0;
}

#define ssl_RDEXACT(_ssl, _fd, _data, _size) ssl_rdexact((_ssl), (_fd), (_data), (_size), xch, ctx)

static int slave_pagebuf_get_one(xc_interface *xch, struct restore_ctx *ctx,
                           pagebuf_t* buf, int fd, uint32_t dom, struct ssl_wrap* de_wrap)
{
    int count, countpages, oldcount, i;
    void* ptmp;

	hprintf("function pagebuf_get_one\n");
    if ( ssl_RDEXACT(de_wrap, fd, &count, sizeof(count)) )
    {
        PERROR("Error when reading batch size");
        return -1;
    }

    // DPRINTF("reading batch of %d pages\n", count);
	if (count > 0) {
		hprintf("Pagebuf count: %d\n", count);
	}

    switch ( count )
    {
    case 0:
        // DPRINTF("Last batch read\n");
        return 0;

	case XC_PARA_MIGR_END: 
		buf->nr_pages = 0;
		buf->nr_physpages = 1;
		return 1;

	case XC_LAST_ITER_FIRST: 
		buf->nr_pages = 1;
		buf->nr_physpages = 0;
		return 1;

	case XC_ITERATION_BARRIER:
		buf->nr_pages = 0;
		buf->nr_physpages = 0;
		return 1;

    case XC_SAVE_ID_ENABLE_VERIFY_MODE:
        DPRINTF("Entering page verify mode\n");
        buf->verify = 1;
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_VCPU_INFO:
		hprintf("Real VCPU_INFO\n");
        buf->new_ctxt_format = 1;
        if ( ssl_RDEXACT(de_wrap, fd, &buf->max_vcpu_id, sizeof(buf->max_vcpu_id)) ||
             buf->max_vcpu_id >= 64 || ssl_RDEXACT(de_wrap, fd, &buf->vcpumap,
                                               sizeof(uint64_t)) ) {
            PERROR("Error when reading max_vcpu_id");
            return -1;
        }
        // DPRINTF("Max VCPU ID: %d, vcpumap: %llx\n", buf->max_vcpu_id, buf->vcpumap);
		//return 0;
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_HVM_IDENT_PT:
        /* Skip padding 4 bytes then read the EPT identity PT location. */
        if ( ssl_RDEXACT(de_wrap, fd, &buf->identpt, sizeof(uint32_t)) ||
             ssl_RDEXACT(de_wrap, fd, &buf->identpt, sizeof(uint64_t)) )
        {
            PERROR("error read the address of the EPT identity map");
            return -1;
        }
        // DPRINTF("EPT identity map address: %llx\n", buf->identpt);
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_HVM_VM86_TSS:
        /* Skip padding 4 bytes then read the vm86 TSS location. */
        if ( ssl_RDEXACT(de_wrap, fd, &buf->vm86_tss, sizeof(uint32_t)) ||
             ssl_RDEXACT(de_wrap, fd, &buf->vm86_tss, sizeof(uint64_t)) )
        {
            PERROR("error read the address of the vm86 TSS");
            return -1;
        }
        // DPRINTF("VM86 TSS location: %llx\n", buf->vm86_tss);
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_TMEM:
        DPRINTF("xc_domain_restore start tmem\n");
		hprintf("Real TMEM\n");
        if ( xc_tmem_restore(xch, dom, fd) ) {
            PERROR("error reading/restoring tmem");
            return -1;
        }
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_TMEM_EXTRA:
		hprintf("Real TMEM Extra\n");
        if ( xc_tmem_restore_extra(xch, dom, fd) ) {
            PERROR("error reading/restoring tmem extra");
            return -1;
        }
		//return 0;
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_TSC_INFO:
    {
        uint32_t tsc_mode, khz, incarn;
        uint64_t nsec;
		hprintf("Real TSC info\n");
        if ( ssl_RDEXACT(de_wrap, fd, &tsc_mode, sizeof(uint32_t)) ||
             ssl_RDEXACT(de_wrap, fd, &nsec, sizeof(uint64_t)) ||
             ssl_RDEXACT(de_wrap, fd, &khz, sizeof(uint32_t)) ||
             ssl_RDEXACT(de_wrap, fd, &incarn, sizeof(uint32_t)) ||
             xc_domain_set_tsc_info(xch, dom, tsc_mode, nsec, khz, incarn) ) {
            PERROR("error reading/restoring tsc info");
            return -1;
        }
		return 0;
        //return pagebuf_get_one(xch, ctx, buf, fd, dom);
    }

    case XC_SAVE_ID_HVM_CONSOLE_PFN :
        /* Skip padding 4 bytes then read the console pfn location. */
        if ( ssl_RDEXACT(de_wrap, fd, &buf->console_pfn, sizeof(uint32_t)) ||
             ssl_RDEXACT(de_wrap, fd, &buf->console_pfn, sizeof(uint64_t)) )
        {
            PERROR("error read the address of the console pfn");
            return -1;
        }
        // DPRINTF("console pfn location: %llx\n", buf->console_pfn);
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_LAST_CHECKPOINT:
        ctx->last_checkpoint = 1;
        // DPRINTF("last checkpoint indication received");
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    case XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION:
        /* Skip padding 4 bytes then read the acpi ioport location. */
        if ( ssl_RDEXACT(de_wrap, fd, &buf->acpi_ioport_location, sizeof(uint32_t)) ||
             ssl_RDEXACT(de_wrap, fd, &buf->acpi_ioport_location, sizeof(uint64_t)) )
        {
            PERROR("error read the acpi ioport location");
            return -1;
        }
        return slave_pagebuf_get_one(xch, ctx, buf, fd, dom, de_wrap);

    default:
        if ( (count > MAX_BATCH_SIZE) || (count < 0) ) {
            ERROR("Max batch size exceeded (%d). Giving up.", count);
            errno = EMSGSIZE;
            return -1;
        }
        break;
    }

    oldcount = buf->nr_pages;
    buf->nr_pages += count;
    if (!buf->pfn_types) {
        if (!(buf->pfn_types = malloc(buf->nr_pages * sizeof(*(buf->pfn_types))))) {
            ERROR("Could not allocate PFN type buffer");
            return -1;
        }
    } else {
        if (!(ptmp = realloc(buf->pfn_types, buf->nr_pages * sizeof(*(buf->pfn_types))))) {
            ERROR("Could not reallocate PFN type buffer");
            return -1;
        }
        buf->pfn_types = ptmp;
    }
    if ( ssl_RDEXACT(de_wrap, fd, buf->pfn_types + oldcount, count * sizeof(*(buf->pfn_types)))) {
        PERROR("Error when reading region pfn types");
        return -1;
    }

    countpages = count;
    for (i = oldcount; i < buf->nr_pages; ++i)
        if ((buf->pfn_types[i] & XEN_DOMCTL_PFINFO_LTAB_MASK) == XEN_DOMCTL_PFINFO_XTAB)
            --countpages;

    if (!countpages)
        return count;

    oldcount = buf->nr_physpages;
    buf->nr_physpages += countpages;
    if (!buf->pages) {
        if (!(buf->pages = malloc(buf->nr_physpages * PAGE_SIZE))) {
            ERROR("Could not allocate page buffer");
            return -1;
        }
    } else {
        if (!(ptmp = realloc(buf->pages, buf->nr_physpages * PAGE_SIZE))) {
            ERROR("Could not reallocate page buffer");
            return -1;
        }
        buf->pages = ptmp;
    }
    if ( ssl_RDEXACT(de_wrap, fd, buf->pages + oldcount * PAGE_SIZE, countpages * PAGE_SIZE) ) {
        PERROR("Error when reading pages");
        return -1;
    }

    return count;
}

static int pagebuf_get_one(xc_interface *xch, struct restore_ctx *ctx,
                           pagebuf_t* buf, int fd, uint32_t dom)
{
    int count, countpages, oldcount, i;
    void* ptmp;

	hprintf("function pagebuf_get_one\n");
    if ( RDEXACT(fd, &count, sizeof(count)) )
    {
        PERROR("Error when reading batch size");
        return -1;
    }

    // DPRINTF("reading batch of %d pages\n", count);
	if (count > 0) {
		hprintf("Pagebuf count: %d\n", count);
	}

    switch ( count )
    {
    case 0:
        // DPRINTF("Last batch read\n");
        return 0;

	case XC_PARA_MIGR_END: 
		buf->nr_pages = 0;
		buf->nr_physpages = 1;
		return 1;

	case XC_LAST_ITER_FIRST: 
		buf->nr_pages = 1;
		buf->nr_physpages = 0;
		return 1;

	case XC_ITERATION_BARRIER:
		buf->nr_pages = 0;
		buf->nr_physpages = 0;
		return 1;

    case XC_SAVE_ID_ENABLE_VERIFY_MODE:
        DPRINTF("Entering page verify mode\n");
        buf->verify = 1;
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_VCPU_INFO:
		hprintf("Real VCPU_INFO\n");
        buf->new_ctxt_format = 1;
        if ( RDEXACT(fd, &buf->max_vcpu_id, sizeof(buf->max_vcpu_id)) ||
             buf->max_vcpu_id >= 64 || RDEXACT(fd, &buf->vcpumap,
                                               sizeof(uint64_t)) ) {
            PERROR("Error when reading max_vcpu_id");
            return -1;
        }
        // DPRINTF("Max VCPU ID: %d, vcpumap: %llx\n", buf->max_vcpu_id, buf->vcpumap);
		//return 0;
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_HVM_IDENT_PT:
        /* Skip padding 4 bytes then read the EPT identity PT location. */
        if ( RDEXACT(fd, &buf->identpt, sizeof(uint32_t)) ||
             RDEXACT(fd, &buf->identpt, sizeof(uint64_t)) )
        {
            PERROR("error read the address of the EPT identity map");
            return -1;
        }
        // DPRINTF("EPT identity map address: %llx\n", buf->identpt);
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_HVM_VM86_TSS:
        /* Skip padding 4 bytes then read the vm86 TSS location. */
        if ( RDEXACT(fd, &buf->vm86_tss, sizeof(uint32_t)) ||
             RDEXACT(fd, &buf->vm86_tss, sizeof(uint64_t)) )
        {
            PERROR("error read the address of the vm86 TSS");
            return -1;
        }
        // DPRINTF("VM86 TSS location: %llx\n", buf->vm86_tss);
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_TMEM:
        DPRINTF("xc_domain_restore start tmem\n");
		hprintf("Real TMEM\n");
        if ( xc_tmem_restore(xch, dom, fd) ) {
            PERROR("error reading/restoring tmem");
            return -1;
        }
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_TMEM_EXTRA:
		hprintf("Real TMEM Extra\n");
        if ( xc_tmem_restore_extra(xch, dom, fd) ) {
            PERROR("error reading/restoring tmem extra");
            return -1;
        }
		//return 0;
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_TSC_INFO:
    {
        uint32_t tsc_mode, khz, incarn;
        uint64_t nsec;
		hprintf("Real TSC info\n");
        if ( RDEXACT(fd, &tsc_mode, sizeof(uint32_t)) ||
             RDEXACT(fd, &nsec, sizeof(uint64_t)) ||
             RDEXACT(fd, &khz, sizeof(uint32_t)) ||
             RDEXACT(fd, &incarn, sizeof(uint32_t)) ||
             xc_domain_set_tsc_info(xch, dom, tsc_mode, nsec, khz, incarn) ) {
            PERROR("error reading/restoring tsc info");
            return -1;
        }
		return 0;
        //return pagebuf_get_one(xch, ctx, buf, fd, dom);
    }

    case XC_SAVE_ID_HVM_CONSOLE_PFN :
        /* Skip padding 4 bytes then read the console pfn location. */
        if ( RDEXACT(fd, &buf->console_pfn, sizeof(uint32_t)) ||
             RDEXACT(fd, &buf->console_pfn, sizeof(uint64_t)) )
        {
            PERROR("error read the address of the console pfn");
            return -1;
        }
        // DPRINTF("console pfn location: %llx\n", buf->console_pfn);
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_LAST_CHECKPOINT:
        ctx->last_checkpoint = 1;
        // DPRINTF("last checkpoint indication received");
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    case XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION:
        /* Skip padding 4 bytes then read the acpi ioport location. */
        if ( RDEXACT(fd, &buf->acpi_ioport_location, sizeof(uint32_t)) ||
             RDEXACT(fd, &buf->acpi_ioport_location, sizeof(uint64_t)) )
        {
            PERROR("error read the acpi ioport location");
            return -1;
        }
        return pagebuf_get_one(xch, ctx, buf, fd, dom);

    default:
        if ( (count > MAX_BATCH_SIZE) || (count < 0) ) {
            ERROR("Max batch size exceeded (%d). Giving up.", count);
            errno = EMSGSIZE;
            return -1;
        }
        break;
    }

    oldcount = buf->nr_pages;
    buf->nr_pages += count;
    if (!buf->pfn_types) {
        if (!(buf->pfn_types = malloc(buf->nr_pages * sizeof(*(buf->pfn_types))))) {
            ERROR("Could not allocate PFN type buffer");
            return -1;
        }
    } else {
        if (!(ptmp = realloc(buf->pfn_types, buf->nr_pages * sizeof(*(buf->pfn_types))))) {
            ERROR("Could not reallocate PFN type buffer");
            return -1;
        }
        buf->pfn_types = ptmp;
    }
    if ( RDEXACT(fd, buf->pfn_types + oldcount, count * sizeof(*(buf->pfn_types)))) {
        PERROR("Error when reading region pfn types");
        return -1;
    }

    countpages = count;
    for (i = oldcount; i < buf->nr_pages; ++i)
        if ((buf->pfn_types[i] & XEN_DOMCTL_PFINFO_LTAB_MASK) == XEN_DOMCTL_PFINFO_XTAB)
            --countpages;

    if (!countpages)
        return count;

    oldcount = buf->nr_physpages;
    buf->nr_physpages += countpages;
    if (!buf->pages) {
        if (!(buf->pages = malloc(buf->nr_physpages * PAGE_SIZE))) {
            ERROR("Could not allocate page buffer");
            return -1;
        }
    } else {
        if (!(ptmp = realloc(buf->pages, buf->nr_physpages * PAGE_SIZE))) {
            ERROR("Could not reallocate page buffer");
            return -1;
        }
        buf->pages = ptmp;
    }
    if ( RDEXACT(fd, buf->pages + oldcount * PAGE_SIZE, countpages * PAGE_SIZE) ) {
        PERROR("Error when reading pages");
        return -1;
    }

    return count;
}

static int pagebuf_get(xc_interface *xch, struct restore_ctx *ctx,
                       pagebuf_t* buf, int fd, uint32_t dom)
{
    int rc;

    buf->nr_physpages = buf->nr_pages = 0;

    do {
        rc = pagebuf_get_one(xch, ctx, buf, fd, dom);
    } while (rc > 0);

    if (rc < 0)
        pagebuf_free(buf);

    return rc;
}

/* Bottom Apply Flag */
int apply_bottom_end = 0;

/*
 * classicsong 
 * profiling params
 *
 * switch it off by defaut by seting PROFILE_MIGR_PRIMTIVE 0
 */
#define PROFILE_MIGR_PRIMTIVE 0

struct timeval recv_page_map[20];
struct timeval recv_page_map_end[20];
unsigned long long total_page_map[20];

struct timeval apply_time[20];
struct timeval apply_time_end[20];
unsigned long long total_apply_time[20];

extern __thread unsigned long long total_out_time;
extern __thread unsigned long long total_inner_time;
extern __thread char *local_malloc_buf;

#if PROFILE_MIGR_PRIMTIVE
static unsigned long
time_between(struct timeval begin, struct timeval end)
{
	    return (end.tv_sec - begin.tv_sec) * 1000000 + (end.tv_usec - begin.tv_usec);
}
#endif

static int apply_batch(xc_interface *xch, uint32_t dom, struct restore_ctx *ctx,
                       xen_pfn_t* region_mfn, xen_pfn_t* p2m_batch, unsigned long* pfn_type, int pae_extended_cr3,
                       unsigned int hvm, struct xc_mmu* mmu,
                       pagebuf_t* pagebuf, int curbatch, int id)
{
    int i, j, curpage, nr_mfns;
    /* used by debug verify code */
    unsigned long buf[PAGE_SIZE/sizeof(unsigned long)];
    /* Our mapping of the current region (batch) */
    char *region_base;
    /* A temporary mapping, and a copy, of one frame of guest memory. */
    unsigned long *page = NULL;
    int nraces = 0;
    struct domain_info_context *dinfo = &ctx->dinfo;
    int* pfn_err = NULL;
    int rc = -1;

    unsigned long mfn, pfn, pagetype;

    j = pagebuf->nr_pages - curbatch;
    if (j > MAX_BATCH_SIZE)
        j = MAX_BATCH_SIZE;

    /* First pass for this batch: work out how much memory to alloc */
    nr_mfns = 0; 
    for ( i = 0; i < j; i++ )
    {
        unsigned long pfn, pagetype;
        pfn      = pagebuf->pfn_types[i + curbatch] & ~XEN_DOMCTL_PFINFO_LTAB_MASK;
        pagetype = pagebuf->pfn_types[i + curbatch] &  XEN_DOMCTL_PFINFO_LTAB_MASK;
		hprintf("pfn = %ld\n", pfn);

        if ( (pagetype != XEN_DOMCTL_PFINFO_XTAB) && 
             (ctx->p2m[pfn] == INVALID_P2M_ENTRY) )
        {
            /* Have a live PFN which hasn't had an MFN allocated */
            p2m_batch[nr_mfns++] = pfn; 
            ctx->p2m[pfn]--;
        }
    } 

    /* Now allocate a bunch of mfns for this batch */
    if ( nr_mfns &&
         (mc_xc_domain_populate_physmap_exact(xch, dom, nr_mfns, 0,
                                            0, p2m_batch) != 0) )
    { 
        ERROR("Failed to allocate memory for batch.!\n"); 
        errno = ENOMEM;
        return -1;
    }

    /* Second pass for this batch: update p2m[] and region_mfn[] */
    nr_mfns = 0; 
    for ( i = 0; i < j; i++ )
    {
        unsigned long pfn, pagetype;
        pfn      = pagebuf->pfn_types[i + curbatch] & ~XEN_DOMCTL_PFINFO_LTAB_MASK;
        pagetype = pagebuf->pfn_types[i + curbatch] &  XEN_DOMCTL_PFINFO_LTAB_MASK;

        if ( pagetype == XEN_DOMCTL_PFINFO_XTAB )
            region_mfn[i] = ~0UL; /* map will fail but we don't care */
        else 
        {
            if ( ctx->p2m[pfn] == (INVALID_P2M_ENTRY-1) )
            {
                /* We just allocated a new mfn above; update p2m */
                ctx->p2m[pfn] = p2m_batch[nr_mfns++]; 
                ctx->nr_pfns++; 
            }

            /* setup region_mfn[] for batch map.
             * For HVM guests, this interface takes PFNs, not MFNs */
            region_mfn[i] = hvm ? pfn : ctx->p2m[pfn]; 
        }
    }

    /* Map relevant mfns */
    pfn_err = calloc(j, sizeof(*pfn_err));

#if PROFILE_MIGR_PRIMTIVE
	gettimeofday(&recv_page_map[id], NULL);
#endif
    region_base = xc_map_foreign_bulk(
        xch, dom, PROT_WRITE, region_mfn, pfn_err, j);
#if PROFILE_MIGR_PRIMTIVE
	gettimeofday(&recv_page_map_end[id], NULL);
	total_page_map[id] += time_between(recv_page_map[id], recv_page_map_end[id]);
#endif

    if ( region_base == NULL )
    {
        PERROR("map batch failed");
        free(pfn_err);
        return -1;
    }

    for ( i = 0, curpage = -1; i < j; i++ )
    {
        pfn      = pagebuf->pfn_types[i + curbatch] & ~XEN_DOMCTL_PFINFO_LTAB_MASK;
        pagetype = pagebuf->pfn_types[i + curbatch] &  XEN_DOMCTL_PFINFO_LTAB_MASK;

        if ( pagetype == XEN_DOMCTL_PFINFO_XTAB )
            /* a bogus/unmapped page: skip it */
            continue;

        if (pfn_err[i])
        {
            ERROR("unexpected PFN mapping failure");
            goto err_mapped;
        }

        ++curpage;

        if ( pfn > dinfo->p2m_size )
        {
            ERROR("pfn out of range");
            goto err_mapped;
        }

        pfn_type[pfn] = pagetype;

        mfn = ctx->p2m[pfn];

        /* In verify mode, we use a copy; otherwise we work in place */
        page = pagebuf->verify ? (void *)buf : (region_base + i*PAGE_SIZE);

        memcpy(page, pagebuf->pages + (curpage + curbatch) * PAGE_SIZE, PAGE_SIZE);

        pagetype &= XEN_DOMCTL_PFINFO_LTABTYPE_MASK;

        if ( (pagetype >= XEN_DOMCTL_PFINFO_L1TAB) &&
             (pagetype <= XEN_DOMCTL_PFINFO_L4TAB) )
        {
            /*
            ** A page table page - need to 'uncanonicalize' it, i.e.
            ** replace all the references to pfns with the corresponding
            ** mfns for the new domain.
            **
            ** On PAE we need to ensure that PGDs are in MFNs < 4G, and
            ** so we may need to update the p2m after the main loop.
            ** Hence we defer canonicalization of L1s until then.
            */
            if ((ctx->pt_levels != 3) ||
                pae_extended_cr3 ||
                (pagetype != XEN_DOMCTL_PFINFO_L1TAB)) {

                if (!uncanonicalize_pagetable(xch, dom, ctx, page)) {
                    /*
                    ** Failing to uncanonicalize a page table can be ok
                    ** under live migration since the pages type may have
                    ** changed by now (and we'll get an update later).
                    */
                    DPRINTF("PT L%ld race on pfn=%08lx mfn=%08lx\n",
                            pagetype >> 28, pfn, mfn);
                    nraces++;
                    continue;
                }
            }
        }
        else if ( pagetype != XEN_DOMCTL_PFINFO_NOTAB )
        {
            ERROR("Bogus page type %lx page table is out of range: "
                  "i=%d p2m_size=%lu", pagetype, i, dinfo->p2m_size);
            goto err_mapped;
        }

        if ( pagebuf->verify )
        {
            int res = memcmp(buf, (region_base + i*PAGE_SIZE), PAGE_SIZE);
            if ( res )
            {
                int v;

                DPRINTF("************** pfn=%lx type=%lx gotcs=%08lx "
                        "actualcs=%08lx\n", pfn, pagebuf->pfn_types[pfn],
                        csum_page(region_base + (i + curbatch)*PAGE_SIZE),
                        csum_page(buf));

                for ( v = 0; v < 4; v++ )
                {
                    unsigned long *p = (unsigned long *)
                        (region_base + i*PAGE_SIZE);
                    if ( buf[v] != p[v] )
                        DPRINTF("    %d: %08lx %08lx\n", v, buf[v], p[v]);
                }
            }
        }

        if ( !hvm &&
             xc_add_mmu_update(xch, mmu,
                               (((unsigned long long)mfn) << PAGE_SHIFT)
                               | MMU_MACHPHYS_UPDATE, pfn) )
        {
            PERROR("failed machpys update mfn=%lx pfn=%lx", mfn, pfn);
            goto err_mapped;
        }
    } /* end of 'batch' for loop */

    rc = nraces;

  err_mapped:
    munmap(region_base, j*PAGE_SIZE);
	free(pfn_err);
	pagebuf->nr_physpages = pagebuf->nr_pages = 0;
	pagebuf_pool_enqueue(pagebuf);

    return rc;
}

struct global_mc_apply_para {
	xc_interface *xch;
	uint32_t dom;
	struct restore_ctx *ctx;
    unsigned long *pfn_type;
	int pae_extended_cr3;
	unsigned int hvm;
    struct xc_mmu *mmu;
	int ready; // ready flag
} global_mc_top_apply;


/* Roger 
 * Migration slave thread enter point */
void* receive_patch(void* args)
{
	int conn, pagecount;
	send_slave_argu_t *argu = (send_slave_argu_t*)args;
	char* ip = argu->ip;
	char* port = argu->port;
	int id = argu->id;
    pagebuf_t* pagebuf;
    xen_pfn_t *region_mfn = NULL;
	xen_pfn_t *p2m_batch = NULL;

	struct global_mc_apply_para *apply = &global_mc_top_apply;

	/* Init SSL */
	struct ssl_wrap *wrap = (struct ssl_wrap*)malloc(sizeof(struct ssl_wrap));
	struct ssl_wrap *de_wrap = (struct ssl_wrap*)malloc(sizeof(struct ssl_wrap));
	wrap->ssl_buf_len = PAGE_SIZE;
	wrap->ssl_buf = (char*)malloc(wrap->ssl_buf_len);
	wrap->cc = init_ssl_byname("aes128-cbc", "123Roger", CIPHER_ENCRYPT);
	de_wrap->ssl_buf_len = PAGE_SIZE;
	de_wrap->ssl_buf = (char*)malloc(de_wrap->ssl_buf_len);
	de_wrap->cc = init_ssl_byname("aes128-cbc", "123Roger", CIPHER_DECRYPT);
	/* End SSL */

	local_malloc_buf = memalign(PAGE_SIZE, 10 * PAGE_SIZE);
	/* Profile */
	total_out_time = total_inner_time = 0;

	/* apply */
	p2m_batch = malloc(ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT));
    region_mfn = malloc(ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT));
    memset(region_mfn, 0,
           ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT)); 
    memset(p2m_batch, 0,
           ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT)); 

	hprintf("Slave start to connect\n");
	if ((conn = mc_net_server(ip, port)) < 0) {
		fprintf(stderr, "Net Server Error\n");
		exit(-1);
	}

	hprintf("Slave connect success, ip = %s\n", ip);
	while(mc_xch == NULL || mc_ctx == NULL || mc_dom == 0 || apply->ready == 0) {
		nanosleep(SLEEP_SHORT_TIME, NULL);
	}
	hprintf("Slave Ready\n");

    //get pagebuf from a pool
	while (pagebuf_pool_dequeue(&pagebuf) < 0) nanosleep(SLEEP_SHORT_TIME, NULL);

	while ( (pagecount = slave_pagebuf_get_one(mc_xch, mc_ctx, pagebuf, conn, mc_dom, de_wrap)) > 0 ) {
		hprintf("Slave Read Page, ip = %s, read %d pages\n", ip, pagecount);
		if (pagebuf->nr_pages == 0 && pagebuf->nr_physpages == 1) { // finish
			pthread_mutex_lock(&recv_finish_cnt_mutex);
			recv_finish_cnt++;
			pthread_mutex_unlock(&recv_finish_cnt_mutex);
			pagebuf->nr_pages = pagebuf->nr_physpages = 0;
			pagebuf_pool_enqueue(pagebuf);
			break;
		} else if (pagebuf->nr_pages == 0 && pagebuf->nr_physpages == 0) { // iteration barrier 

			char* return_val= "OK"; // This should be picked out
			pthread_barrier_wait(&recv_iter_barr);
			ssl_write(wrap, conn, return_val, strlen(return_val));
			continue;
		} else if (pagebuf->nr_pages == 1 && pagebuf->nr_physpages == 0) { // last iteration
			fprintf(stderr, "Slave inform Last Iteration\n");
			pthread_mutex_lock(&last_iteration_mutex); 
			if (!mc_last_iter)
				mc_last_iter = 1;
			pthread_mutex_unlock(&last_iteration_mutex); 
			pagebuf->nr_pages = pagebuf->nr_physpages = 0;
			continue;
		}

        /*
         * if nr_pages <= 0
         *    not memory data, let main process handle it
         * otherwise
         *    memory data, handle it
         */
		if (pagebuf->nr_pages <= 0) {
			recv_pagebuf_enqueue(pagebuf);
		} else {
			int j = pagebuf->nr_pages, curbatch = 0; 
#if PROFILE_MIGR_PRIMTIVE
			gettimeofday(&apply_time[id], NULL);
#endif
			while ( curbatch < j ) {
				int brc;
				brc = apply_batch(apply->xch/*global*/, apply->dom/*global*/, 
						apply->ctx/*global*/, region_mfn/*local*/, p2m_batch/*local*/,
						apply->pfn_type/*global share*/, apply->pae_extended_cr3/*global*/,
						apply->hvm/*global*/, apply->mmu/*global*/, 
						pagebuf/*local*/, curbatch/*local*/, id);
				if ( brc < 0 )
					break;
				curbatch += MAX_BATCH_SIZE;
			}
#if PROFILE_MIGR_PRIMTIVE
			gettimeofday(&apply_time_end[id], NULL);
			total_apply_time[id] += time_between(apply_time[id], apply_time_end[id]);
#endif
		}

		while (pagebuf_pool_dequeue(&pagebuf) < 0) nanosleep(SLEEP_SHORT_TIME, NULL);
	}
	hprintf("Slave Finish, ip = %s\n", ip);
	free(region_mfn);
	free(p2m_batch);
	free(local_malloc_buf);

#if PROFILE_MIGR_PRIMTIVE
	fprintf(stderr, "populate_map inner: %llu\n", total_inner_time);
	fprintf(stderr, "populate_map out: %llu\n", total_out_time);
#endif
	return NULL;
}

extern int is_migrate;

static void init_pagebuf_pool(int slave_cnt) {
	int i;
#define SLAVE_RATIO 10
	for ( i = 0; i < slave_cnt * SLAVE_RATIO; i++ ) {
		pagebuf_t *pagebuf = malloc(sizeof(*pagebuf));
		pagebuf_init(pagebuf);
		pagebuf_pool_enqueue(pagebuf);
	}
}

int xc_domain_restore(xc_interface *xch, int io_fd, uint32_t dom,
                      unsigned int store_evtchn, unsigned long *store_mfn,
                      unsigned int console_evtchn, unsigned long *console_mfn,
                      unsigned int hvm, unsigned int pae, int superpages)
{
    DECLARE_DOMCTL;
    int rc = 1, frc, i, j, n, m, pae_extended_cr3 = 0, ext_vcpucontext = 0;
    int vcpuextstate = 0;
    uint32_t vcpuextstate_size = 0;
    unsigned long mfn, pfn;
    unsigned int prev_pc;
    //int nraces = 0;
	unsigned long long t = 0;

	int ever_last_iter = 0;

    /* The new domain's shared-info frame number. */
    unsigned long shared_info_frame;
    unsigned char shared_info_page[PAGE_SIZE]; /* saved contents from file */
    shared_info_any_t *old_shared_info = 
        (shared_info_any_t *)shared_info_page;
    shared_info_any_t *new_shared_info;

    /* A copy of the CPU context of the guest. */
    DECLARE_HYPERCALL_BUFFER(vcpu_guest_context_any_t, ctxt);

    /* A copy of the CPU eXtended States of the guest. */
    DECLARE_HYPERCALL_BUFFER(void, buffer);

    /* A table containing the type of each PFN (/not/ MFN!). */
    unsigned long *pfn_type = NULL;

    /* A table of MFNs to map in the current region */
    xen_pfn_t *region_mfn = NULL;

    /* A copy of the pfn-to-mfn table frame list. */
    xen_pfn_t *p2m_frame_list = NULL;
    
    /* A temporary mapping of the guest's start_info page. */
    start_info_any_t *start_info;

    /* Our mapping of the current region (batch) */
    char *region_base;

    struct xc_mmu *mmu = NULL;

    struct mmuext_op pin[MAX_PIN_BATCH];
    unsigned int nr_pins;

    uint64_t vcpumap = 1ULL;
    unsigned int max_vcpu_id = 0;
    int new_ctxt_format = 0;

    pagebuf_t pagebuf;
    tailbuf_t tailbuf, tmptail;
    void* vcpup;
    uint64_t console_pfn = 0;

    int orig_io_fd_flags;

    static struct restore_ctx _ctx = {
        .live_p2m = NULL,
        .p2m = NULL,
    };
    static struct restore_ctx *ctx = &_ctx;
    struct domain_info_context *dinfo = &ctx->dinfo;


	is_migrate = 1;
	mc_dom = dom;
	mc_xch = xch;
	mc_ctx = ctx;

	hprintf("Enter Restore Doamin\n");
	bzero(total_apply_time, sizeof(unsigned long long) * 20);
	bzero(total_page_map, sizeof(unsigned long long) * 20);

    pagebuf_init(&pagebuf);
    memset(&tailbuf, 0, sizeof(tailbuf));
    tailbuf.ishvm = hvm;

    /* For info only */
    ctx->nr_pfns = 0;

    if ( superpages )
        return 1;

    ctxt = xc_hypercall_buffer_alloc(xch, ctxt, sizeof(*ctxt));

    if ( ctxt == NULL )
    {
        PERROR("Unable to allocate VCPU ctxt buffer");
        return 1;
    }


    if ( (orig_io_fd_flags = fcntl(io_fd, F_GETFL, 0)) < 0 ) {
        PERROR("unable to read IO FD flags");
        goto out;
    }

	hprintf("Read p2m size, io_fd = %d\n", io_fd);
    if ( MCRDEXACT(io_fd, &dinfo->p2m_size, sizeof(unsigned long)) )
    {
        PERROR("read: p2m_size");
        goto out;
    }
    //DPRINTF("xc_domain_restore start: p2m_size = %lx\n", dinfo->p2m_size);
    hprintf("xc_domain_restore start: p2m_size = %lx\n", dinfo->p2m_size);

    if ( !get_platform_info(xch, dom,
                            &ctx->max_mfn, &ctx->hvirt_start, &ctx->pt_levels, &dinfo->guest_width) )
    {
        ERROR("Unable to get platform info.");
        return 1;
    }
    
    /* The *current* word size of the guest isn't very interesting; for now
     * assume the guest will be the same as we are.  We'll fix that later
     * if we discover otherwise. */
    dinfo->guest_width = sizeof(unsigned long);
    ctx->pt_levels = (dinfo->guest_width == 8) ? 4 : (ctx->pt_levels == 2) ? 2 : 3; 
    
    if ( !hvm ) 
    {
        /* Load the p2m frame list, plus potential extended info chunk */
        p2m_frame_list = load_p2m_frame_list(xch, ctx,
            io_fd, &pae_extended_cr3, &ext_vcpucontext,
            &vcpuextstate, &vcpuextstate_size);

        if ( !p2m_frame_list )
            goto out;

        /* Now that we know the word size, tell Xen about it */
        memset(&domctl, 0, sizeof(domctl));
        domctl.domain = dom;
        domctl.cmd    = XEN_DOMCTL_set_address_size;
        domctl.u.address_size.size = dinfo->guest_width * 8;
        frc = do_domctl(xch, &domctl);
        if ( frc != 0 )
        {
            PERROR("Unable to set guest address size.");
            goto out;
        }
    }

    /* We want zeroed memory so use calloc rather than malloc. */
    ctx->p2m   = calloc(dinfo->p2m_size, sizeof(xen_pfn_t));
    pfn_type   = calloc(dinfo->p2m_size, sizeof(unsigned long));


    region_mfn = malloc(ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT));
    ctx->p2m_batch = malloc(ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT));

	hprintf("Alloc Some memory\n");
    if ( (ctx->p2m == NULL) || (pfn_type == NULL) ||
         (region_mfn == NULL) || (ctx->p2m_batch == NULL) )
    {
        ERROR("memory alloc failed");
        errno = ENOMEM;
        goto out;
    }

    memset(region_mfn, 0,
           ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT)); 
    memset(ctx->p2m_batch, 0,
           ROUNDUP(MAX_BATCH_SIZE * sizeof(xen_pfn_t), PAGE_SHIFT)); 

    /* Get the domain's shared-info frame. */
    domctl.cmd = XEN_DOMCTL_getdomaininfo;
    domctl.domain = (domid_t)dom;
    if ( xc_domctl(xch, &domctl) < 0 )
    {
        PERROR("Could not get information on new domain");
        goto out;
    }
    shared_info_frame = domctl.u.getdomaininfo.shared_info_frame;

    /* Mark all PFNs as invalid; we allocate on demand */
    for ( pfn = 0; pfn < dinfo->p2m_size; pfn++ )
        ctx->p2m[pfn] = INVALID_P2M_ENTRY;

    mmu = xc_alloc_mmu_updates(xch, dom);
    if ( mmu == NULL )
    {
        PERROR("Could not initialise for MMU updates");
        goto out;
    }

    xc_report_progress_start(xch, "Reloading memory pages", dinfo->p2m_size);

    /*
     * Now simply read each saved frame into its new machine frame.
     * We uncanonicalise page tables as we go.
     */
    prev_pc = 0;

    n = m = 0;

	/* Init Global Apply Parameter */
	global_mc_top_apply.xch = xch;
	global_mc_top_apply.dom = dom;
	global_mc_top_apply.ctx = ctx;
	global_mc_top_apply.pfn_type = pfn_type;
	global_mc_top_apply.pae_extended_cr3 = pae_extended_cr3;
	global_mc_top_apply.hvm = hvm;
	global_mc_top_apply.mmu = mmu;
	global_mc_top_apply.ready = 1;

	/* Pagebuf pool */
	init_pagebuf_pool(recv_slave_cnt);

 loadpages:
	
	hprintf("Before For Loop\n");

    /*
     * classicsong add comments
     *
     * In the receive side,
     * memory data is received, processed and restored into guest VM by slave threads
     * CPU and device states are received by the main process
     */
    for ( ; ; )
    {
        int j;
		pagebuf_t *pagebuf_p;

        xc_report_progress_step(xch, n, dinfo->p2m_size);

		// Roger
        if ( !ctx->completed ) {

			while (recv_pagebuf_dequeue(&pagebuf_p) < 0) {
				pthread_mutex_lock(&last_iteration_mutex); 
				if ( !ever_last_iter && mc_last_iter ) { // Last iteration
					fprintf(stderr, "Master Do last Iteration\n");
					ever_last_iter = 1;
					hprintf("Try to tsc_info\n");
					if ( pagebuf_get_one(xch, ctx, &pagebuf, io_fd, dom) < 0 ) {
						PERROR("Error when reading batch");
						goto out;
					}
				}
				pthread_mutex_unlock(&last_iteration_mutex); 

				pthread_mutex_lock(&recv_finish_cnt_mutex);
				if (recv_finish_cnt < recv_slave_cnt) {
					pthread_mutex_unlock(&recv_finish_cnt_mutex);
					nanosleep(SLEEP_LONG_TIME, NULL);
					continue;
				} else {
					pthread_mutex_unlock(&recv_finish_cnt_mutex);

					if (recv_pagebuf_dequeue(&pagebuf_p) >= 0)
						break;

					fprintf(stderr, "Try VCPU INFO");
					if ( pagebuf_get_one(xch, ctx, &pagebuf, io_fd, dom) < 0 ) {
						ERROR("Error when reading batch\n");
						goto out;
					}
					pagebuf.nr_physpages = pagebuf.nr_pages = 0;
					apply_bottom_end = 1;
					goto mc_end;
				}
			} 
			pthread_mutex_unlock(&recv_finish_cnt_mutex);

			pagebuf = *pagebuf_p;
			if (pagebuf_p)
				free(pagebuf_p);
			pagebuf_p = NULL;
        }

mc_end:
        j = pagebuf.nr_pages;

        DBGPRINTF("batch %d\n",j);

        if ( j == 0 ) {
			fprintf(stderr,"Final CPU Update\n");
            /* catch vcpu updates */
            if (pagebuf.new_ctxt_format) {
				fprintf(stderr,"Update 1\n");
                vcpumap = pagebuf.vcpumap;
                max_vcpu_id = pagebuf.max_vcpu_id;
            }
            /* should this be deferred? does it change? */
            if ( pagebuf.identpt ) {
				fprintf(stderr, "Update 2\n");
                xc_set_hvm_param(xch, dom, HVM_PARAM_IDENT_PT, pagebuf.identpt);
			}
            if ( pagebuf.vm86_tss ) {
				fprintf(stderr, "Update 3\n");
                xc_set_hvm_param(xch, dom, HVM_PARAM_VM86_TSS, pagebuf.vm86_tss);
			}
            if ( pagebuf.console_pfn ) {
				fprintf(stderr, "Update 4\n");
                console_pfn = pagebuf.console_pfn;
			}
            break;  /* our work here is done */
        }
    }

	discard_file_cache(xch, io_fd, 0 /* no flush */);
    /*
     * Ensure we flush all machphys updates before potential PAE-specific
     * reallocations below.
     */
    if ( !hvm && xc_flush_mmu_updates(xch, mmu) )
    {
        PERROR("Error doing flush_mmu_updates()");
        goto out;
    }

    // DPRINTF("Received all pages (%d races)\n", nraces);
	fprintf(stderr, "Received All the Pages\n");

    if ( !ctx->completed ) {

        if ( buffer_tail(xch, ctx, &tailbuf, io_fd, max_vcpu_id, vcpumap,
                         ext_vcpucontext, vcpuextstate, vcpuextstate_size) < 0 ) {
            ERROR ("error buffering image tail");
            goto out;
        }

		fprintf(stderr, "After buffer_tail\n");
        ctx->completed = 1;

        /*
         * If more checkpoints are expected then shift into
         * nonblocking mode for the remainder.
         */
        if ( !ctx->last_checkpoint )
            fcntl(io_fd, F_SETFL, orig_io_fd_flags | O_NONBLOCK);
		fprintf(stderr, "After Last Checkpoint\n");
    }

	fprintf(stderr, "Before ACPI IO port\n");
    if (pagebuf.acpi_ioport_location == 1) {
        DBGPRINTF("Use new firmware ioport from the checkpoint\n");
        xc_set_hvm_param(xch, dom, HVM_PARAM_ACPI_IOPORTS_LOCATION, 1);
    } else if (pagebuf.acpi_ioport_location == 0) {
        DBGPRINTF("Use old firmware ioport from the checkpoint\n");
    } else {
        ERROR("Error, unknow acpi ioport location (%i)", pagebuf.acpi_ioport_location);
    }
	fprintf(stderr, "After ACPI IO port\n");

    if ( ctx->last_checkpoint )
    {
        // DPRINTF("Last checkpoint, finishing\n");
		hprintf("Goto Finish\n");
        goto finish;
    }

    // DPRINTF("Buffered checkpoint\n");

	fprintf(stderr, "Before GetBuf Page\n");
    if ( pagebuf_get(xch, ctx, &pagebuf, io_fd, dom) ) {
        PERROR("error when buffering batch, finishing");
        goto finish;
    }
	fprintf(stderr, "After GetBuf Page\n");
    memset(&tmptail, 0, sizeof(tmptail));
    tmptail.ishvm = hvm;
    if ( buffer_tail(xch, ctx, &tmptail, io_fd, max_vcpu_id, vcpumap,
                     ext_vcpucontext, vcpuextstate, vcpuextstate_size) < 0 ) {
        ERROR ("error buffering image tail, finishing");
        goto finish;
    }
    tailbuf_free(&tailbuf);
    memcpy(&tailbuf, &tmptail, sizeof(tailbuf));

    goto loadpages;

  finish:
    if ( hvm )
        goto finish_hvm;

    if ( (ctx->pt_levels == 3) && !pae_extended_cr3 )
    {
        /*
        ** XXX SMH on PAE we need to ensure PGDs are in MFNs < 4G. This
        ** is a little awkward and involves (a) finding all such PGDs and
        ** replacing them with 'lowmem' versions; (b) upating the p2m[]
        ** with the new info; and (c) canonicalizing all the L1s using the
        ** (potentially updated) p2m[].
        **
        ** This is relatively slow (and currently involves two passes through
        ** the pfn_type[] array), but at least seems to be correct. May wish
        ** to consider more complex approaches to optimize this later.
        */

        int j, k;
        
        /* First pass: find all L3TABs current in > 4G mfns and get new mfns */
        for ( i = 0; i < dinfo->p2m_size; i++ )
        {
            if ( ((pfn_type[i] & XEN_DOMCTL_PFINFO_LTABTYPE_MASK) ==
                  XEN_DOMCTL_PFINFO_L3TAB) &&
                 (ctx->p2m[i] > 0xfffffUL) )
            {
                unsigned long new_mfn;
                uint64_t l3ptes[4];
                uint64_t *l3tab;

                l3tab = (uint64_t *)
                    xc_map_foreign_range(xch, dom, PAGE_SIZE,
                                         PROT_READ, ctx->p2m[i]);

                for ( j = 0; j < 4; j++ )
                    l3ptes[j] = l3tab[j];

                munmap(l3tab, PAGE_SIZE);

                new_mfn = xc_make_page_below_4G(xch, dom, ctx->p2m[i]);
                if ( !new_mfn )
                {
                    PERROR("Couldn't get a page below 4GB :-(");
                    goto out;
                }

                ctx->p2m[i] = new_mfn;
                if ( xc_add_mmu_update(xch, mmu,
                                       (((unsigned long long)new_mfn)
                                        << PAGE_SHIFT) |
                                       MMU_MACHPHYS_UPDATE, i) )
                {
                    PERROR("Couldn't m2p on PAE root pgdir");
                    goto out;
                }

                l3tab = (uint64_t *)
                    xc_map_foreign_range(xch, dom, PAGE_SIZE,
                                         PROT_READ | PROT_WRITE, ctx->p2m[i]);

                for ( j = 0; j < 4; j++ )
                    l3tab[j] = l3ptes[j];

                munmap(l3tab, PAGE_SIZE);
            }
        }

        /* Second pass: find all L1TABs and uncanonicalize them */
        j = 0;

        for ( i = 0; i < dinfo->p2m_size; i++ )
        {
            if ( ((pfn_type[i] & XEN_DOMCTL_PFINFO_LTABTYPE_MASK) ==
                  XEN_DOMCTL_PFINFO_L1TAB) )
            {
                region_mfn[j] = ctx->p2m[i];
                j++;
            }

            if ( (i == (dinfo->p2m_size-1)) || (j == MAX_BATCH_SIZE) )
            {
                region_base = xc_map_foreign_pages(
                    xch, dom, PROT_READ | PROT_WRITE, region_mfn, j);
                if ( region_base == NULL )
                {
                    PERROR("map batch failed");
                    goto out;
                }

                for ( k = 0; k < j; k++ )
                {
                    if ( !uncanonicalize_pagetable(
                        xch, dom, ctx,
                        region_base + k*PAGE_SIZE) )
                    {
                        ERROR("failed uncanonicalize pt!");
                        goto out;
                    }
                }

                munmap(region_base, j*PAGE_SIZE);
                j = 0;
            }
        }

        if ( xc_flush_mmu_updates(xch, mmu) )
        {
            PERROR("Error doing xc_flush_mmu_updates()");
            goto out;
        }
    }

    /*
     * Pin page tables. Do this after writing to them as otherwise Xen
     * will barf when doing the type-checking.
     */
    nr_pins = 0;
    for ( i = 0; i < dinfo->p2m_size; i++ )
    {
        if ( (pfn_type[i] & XEN_DOMCTL_PFINFO_LPINTAB) == 0 )
            continue;

        switch ( pfn_type[i] & XEN_DOMCTL_PFINFO_LTABTYPE_MASK )
        {
        case XEN_DOMCTL_PFINFO_L1TAB:
            pin[nr_pins].cmd = MMUEXT_PIN_L1_TABLE;
            break;

        case XEN_DOMCTL_PFINFO_L2TAB:
            pin[nr_pins].cmd = MMUEXT_PIN_L2_TABLE;
            break;

        case XEN_DOMCTL_PFINFO_L3TAB:
            pin[nr_pins].cmd = MMUEXT_PIN_L3_TABLE;
            break;

        case XEN_DOMCTL_PFINFO_L4TAB:
            pin[nr_pins].cmd = MMUEXT_PIN_L4_TABLE;
            break;

        default:
            continue;
        }

        pin[nr_pins].arg1.mfn = ctx->p2m[i];
        nr_pins++;

        /* Batch full? Then flush. */
        if ( nr_pins == MAX_PIN_BATCH )
        {
            if ( xc_mmuext_op(xch, pin, nr_pins, dom) < 0 )
            {
                PERROR("Failed to pin batch of %d page tables", nr_pins);
                goto out;
            }
            nr_pins = 0;
        }
    }

    /* Flush final partial batch. */
    if ( (nr_pins != 0) && (xc_mmuext_op(xch, pin, nr_pins, dom) < 0) )
    {
        PERROR("Failed to pin batch of %d page tables", nr_pins);
        goto out;
    }

    DPRINTF("Memory reloaded (%ld pages)\n", ctx->nr_pfns);

    /* Get the list of PFNs that are not in the psuedo-phys map */
    {
        int nr_frees = 0;

        for ( i = 0; i < tailbuf.u.pv.pfncount; i++ )
        {
            unsigned long pfn = tailbuf.u.pv.pfntab[i];

            if ( ctx->p2m[pfn] != INVALID_P2M_ENTRY )
            {
                /* pfn is not in physmap now, but was at some point during
                   the save/migration process - need to free it */
                tailbuf.u.pv.pfntab[nr_frees++] = ctx->p2m[pfn];
                ctx->p2m[pfn]  = INVALID_P2M_ENTRY; /* not in pseudo-physical map */
            }
        }

        if ( nr_frees > 0 )
        {
            if ( (frc = xc_domain_decrease_reservation(xch, dom, nr_frees, 0, tailbuf.u.pv.pfntab)) != nr_frees )
            {
                PERROR("Could not decrease reservation : %d", frc);
                goto out;
            }
            else
                DPRINTF("Decreased reservation by %d pages\n", tailbuf.u.pv.pfncount);
        }
    }

    vcpup = tailbuf.u.pv.vcpubuf;
    for ( i = 0; i <= max_vcpu_id; i++ )
    {
        if ( !(vcpumap & (1ULL << i)) )
            continue;

        memcpy(ctxt, vcpup, ((dinfo->guest_width == 8) ? sizeof(ctxt->x64)
                              : sizeof(ctxt->x32)));
        vcpup += (dinfo->guest_width == 8) ? sizeof(ctxt->x64) : sizeof(ctxt->x32);

        DPRINTF("read VCPU %d\n", i);

        if ( !new_ctxt_format )
            SET_FIELD(ctxt, flags, GET_FIELD(ctxt, flags) | VGCF_online);

        if ( i == 0 )
        {
            /*
             * Uncanonicalise the suspend-record frame number and poke
             * resume record.
             */
            pfn = GET_FIELD(ctxt, user_regs.edx);
            if ( (pfn >= dinfo->p2m_size) ||
                 (pfn_type[pfn] != XEN_DOMCTL_PFINFO_NOTAB) )
            {
                ERROR("Suspend record frame number is bad");
                goto out;
            }
            mfn = ctx->p2m[pfn];
            SET_FIELD(ctxt, user_regs.edx, mfn);
            start_info = xc_map_foreign_range(
                xch, dom, PAGE_SIZE, PROT_READ | PROT_WRITE, mfn);
            SET_FIELD(start_info, nr_pages, dinfo->p2m_size);
            SET_FIELD(start_info, shared_info, shared_info_frame<<PAGE_SHIFT);
            SET_FIELD(start_info, flags, 0);
            *store_mfn = ctx->p2m[GET_FIELD(start_info, store_mfn)];
            SET_FIELD(start_info, store_mfn, *store_mfn);
            SET_FIELD(start_info, store_evtchn, store_evtchn);
            *console_mfn = ctx->p2m[GET_FIELD(start_info, console.domU.mfn)];
            SET_FIELD(start_info, console.domU.mfn, *console_mfn);
            SET_FIELD(start_info, console.domU.evtchn, console_evtchn);
            munmap(start_info, PAGE_SIZE);
        }
        /* Uncanonicalise each GDT frame number. */
        if ( GET_FIELD(ctxt, gdt_ents) > 8192 )
        {
            ERROR("GDT entry count out of range");
            goto out;
        }

        for ( j = 0; (512*j) < GET_FIELD(ctxt, gdt_ents); j++ )
        {
            pfn = GET_FIELD(ctxt, gdt_frames[j]);
            if ( (pfn >= dinfo->p2m_size) ||
                 (pfn_type[pfn] != XEN_DOMCTL_PFINFO_NOTAB) )
            {
                ERROR("GDT frame number %i (0x%lx) is bad", 
                      j, (unsigned long)pfn);
                goto out;
            }
            SET_FIELD(ctxt, gdt_frames[j], ctx->p2m[pfn]);
        }
        /* Uncanonicalise the page table base pointer. */
        pfn = UNFOLD_CR3(GET_FIELD(ctxt, ctrlreg[3]));

        if ( pfn >= dinfo->p2m_size )
        {
            ERROR("PT base is bad: pfn=%lu p2m_size=%lu type=%08lx",
                  pfn, dinfo->p2m_size, pfn_type[pfn]);
            goto out;
        }

        if ( (pfn_type[pfn] & XEN_DOMCTL_PFINFO_LTABTYPE_MASK) !=
             ((unsigned long)ctx->pt_levels<<XEN_DOMCTL_PFINFO_LTAB_SHIFT) )
        {
            ERROR("PT base is bad. pfn=%lu nr=%lu type=%08lx %08lx",
                  pfn, dinfo->p2m_size, pfn_type[pfn],
                  (unsigned long)ctx->pt_levels<<XEN_DOMCTL_PFINFO_LTAB_SHIFT);
            goto out;
        }
        SET_FIELD(ctxt, ctrlreg[3], FOLD_CR3(ctx->p2m[pfn]));

        /* Guest pagetable (x86/64) stored in otherwise-unused CR1. */
        if ( (ctx->pt_levels == 4) && (ctxt->x64.ctrlreg[1] & 1) )
        {
            pfn = UNFOLD_CR3(ctxt->x64.ctrlreg[1] & ~1);
            if ( pfn >= dinfo->p2m_size )
            {
                ERROR("User PT base is bad: pfn=%lu p2m_size=%lu",
                      pfn, dinfo->p2m_size);
                goto out;
            }
            if ( (pfn_type[pfn] & XEN_DOMCTL_PFINFO_LTABTYPE_MASK) !=
                 ((unsigned long)ctx->pt_levels<<XEN_DOMCTL_PFINFO_LTAB_SHIFT) )
            {
                ERROR("User PT base is bad. pfn=%lu nr=%lu type=%08lx %08lx",
                      pfn, dinfo->p2m_size, pfn_type[pfn],
                      (unsigned long)ctx->pt_levels<<XEN_DOMCTL_PFINFO_LTAB_SHIFT);
                goto out;
            }
            ctxt->x64.ctrlreg[1] = FOLD_CR3(ctx->p2m[pfn]);
        }
        domctl.cmd = XEN_DOMCTL_setvcpucontext;
        domctl.domain = (domid_t)dom;
        domctl.u.vcpucontext.vcpu = i;
        set_xen_guest_handle(domctl.u.vcpucontext.ctxt, ctxt);
        frc = xc_domctl(xch, &domctl);
        if ( frc != 0 )
        {
            PERROR("Couldn't build vcpu%d", i);
            goto out;
        }

        if ( !ext_vcpucontext )
            goto vcpu_ext_state_restore;
        memcpy(&domctl.u.ext_vcpucontext, vcpup, 128);
        vcpup += 128;
        domctl.cmd = XEN_DOMCTL_set_ext_vcpucontext;
        domctl.domain = dom;
        frc = xc_domctl(xch, &domctl);
        if ( frc != 0 )
        {
            PERROR("Couldn't set extended vcpu%d info", i);
            goto out;
        }

 vcpu_ext_state_restore:
        if ( !vcpuextstate )
            continue;

        memcpy(&domctl.u.vcpuextstate.xfeature_mask, vcpup,
               sizeof(domctl.u.vcpuextstate.xfeature_mask));
        vcpup += sizeof(domctl.u.vcpuextstate.xfeature_mask);
        memcpy(&domctl.u.vcpuextstate.size, vcpup,
               sizeof(domctl.u.vcpuextstate.size));
        vcpup += sizeof(domctl.u.vcpuextstate.size);

        buffer = xc_hypercall_buffer_alloc(xch, buffer,
                                           domctl.u.vcpuextstate.size);
        if ( !buffer )
        {
            PERROR("Could not allocate buffer to restore eXtended States");
            goto out;
        }
        memcpy(buffer, vcpup, domctl.u.vcpuextstate.size);
        vcpup += domctl.u.vcpuextstate.size;

        domctl.cmd = XEN_DOMCTL_setvcpuextstate;
        domctl.domain = dom;
        domctl.u.vcpuextstate.vcpu = i;
        set_xen_guest_handle(domctl.u.vcpuextstate.buffer, buffer);
        frc = xc_domctl(xch, &domctl);
        if ( frc != 0 )
        {
            PERROR("Couldn't set eXtended States for vcpu%d", i);
            goto out;
        }
        xc_hypercall_buffer_free(xch, buffer);
    }

    memcpy(shared_info_page, tailbuf.u.pv.shared_info_page, PAGE_SIZE);

    DPRINTF("Completed checkpoint load\n");

    /* Restore contents of shared-info page. No checking needed. */
    new_shared_info = xc_map_foreign_range(
        xch, dom, PAGE_SIZE, PROT_WRITE, shared_info_frame);

    /* restore saved vcpu_info and arch specific info */
    MEMCPY_FIELD(new_shared_info, old_shared_info, vcpu_info);
    MEMCPY_FIELD(new_shared_info, old_shared_info, arch);

    /* clear any pending events and the selector */
    MEMSET_ARRAY_FIELD(new_shared_info, evtchn_pending, 0);
    for ( i = 0; i < XEN_LEGACY_MAX_VCPUS; i++ )
	    SET_FIELD(new_shared_info, vcpu_info[i].evtchn_pending_sel, 0);

    /* mask event channels */
    MEMSET_ARRAY_FIELD(new_shared_info, evtchn_mask, 0xff);

    /* leave wallclock time. set by hypervisor */
    munmap(new_shared_info, PAGE_SIZE);

    /* Uncanonicalise the pfn-to-mfn table frame-number list. */
    for ( i = 0; i < P2M_FL_ENTRIES; i++ )
    {
        pfn = p2m_frame_list[i];
        if ( (pfn >= dinfo->p2m_size) || (pfn_type[pfn] != XEN_DOMCTL_PFINFO_NOTAB) )
        {
            ERROR("PFN-to-MFN frame number %i (%#lx) is bad", i, pfn);
            goto out;
        }
        p2m_frame_list[i] = ctx->p2m[pfn];
    }

    /* Copy the P2M we've constructed to the 'live' P2M */
    if ( !(ctx->live_p2m = xc_map_foreign_pages(xch, dom, PROT_WRITE,
                                           p2m_frame_list, P2M_FL_ENTRIES)) )
    {
        PERROR("Couldn't map p2m table");
        goto out;
    }

    /* If the domain we're restoring has a different word size to ours,
     * we need to adjust the live_p2m assignment appropriately */
    if ( dinfo->guest_width > sizeof (xen_pfn_t) )
        for ( i = dinfo->p2m_size - 1; i >= 0; i-- )
            ((int64_t *)ctx->live_p2m)[i] = (long)ctx->p2m[i];
    else if ( dinfo->guest_width < sizeof (xen_pfn_t) )
        for ( i = 0; i < dinfo->p2m_size; i++ )   
            ((uint32_t *)ctx->live_p2m)[i] = ctx->p2m[i];
    else
        memcpy(ctx->live_p2m, ctx->p2m, dinfo->p2m_size * sizeof(xen_pfn_t));
    munmap(ctx->live_p2m, P2M_FL_ENTRIES * PAGE_SIZE);

    DPRINTF("Domain ready to be built.\n");
    rc = 0;
    goto out;

  finish_hvm:
	fprintf(stderr, "Enter finish_hvm\n");
    /* Dump the QEMU state to a state file for QEMU to load */
    if ( dump_qemu(xch, dom, &tailbuf.u.hvm) ) {
        PERROR("Error dumping QEMU state to file");
        goto out;
    }

	fprintf(stderr, "Zero Pages\n");
    /* These comms pages need to be zeroed at the start of day */
    if ( xc_clear_domain_page(xch, dom, tailbuf.u.hvm.magicpfns[0]) ||
         xc_clear_domain_page(xch, dom, tailbuf.u.hvm.magicpfns[1]) ||
         xc_clear_domain_page(xch, dom, tailbuf.u.hvm.magicpfns[2]) )
    {
        PERROR("error zeroing magic pages");
        goto out;
    }

	fprintf(stderr, "XC_hvm_param\n");
    if ( (frc = xc_set_hvm_param(xch, dom,
                                 HVM_PARAM_IOREQ_PFN, tailbuf.u.hvm.magicpfns[0]))
         || (frc = xc_set_hvm_param(xch, dom,
                                    HVM_PARAM_BUFIOREQ_PFN, tailbuf.u.hvm.magicpfns[1]))
         || (frc = xc_set_hvm_param(xch, dom,
                                    HVM_PARAM_STORE_PFN, tailbuf.u.hvm.magicpfns[2]))
         || (frc = xc_set_hvm_param(xch, dom,
                                    HVM_PARAM_PAE_ENABLED, pae))
         || (frc = xc_set_hvm_param(xch, dom,
                                    HVM_PARAM_STORE_EVTCHN,
                                    store_evtchn)) )
    {
        PERROR("error setting HVM params: %i", frc);
        goto out;
    }
    *store_mfn = tailbuf.u.hvm.magicpfns[2];

	fprintf(stderr, "console_pfn\n");
    if ( console_pfn ) {
        if ( xc_clear_domain_page(xch, dom, console_pfn) ) {
            PERROR("error zeroing console page");
            goto out;
        }
        if ( (frc = xc_set_hvm_param(xch, dom, 
                                    HVM_PARAM_CONSOLE_PFN, console_pfn)) ) {
            PERROR("error setting HVM param: %i", frc);
            goto out;
        }
        *console_mfn = console_pfn;
    }

	fprintf(stderr, "XC domain setcontext\n");
    frc = xc_domain_hvm_setcontext(xch, dom, tailbuf.u.hvm.hvmbuf,
                                   tailbuf.u.hvm.reclen);
    if ( frc )
    {
        PERROR("error setting the HVM context");
        goto out;
    }

	//fprintf(stderr, "Receive Map Time %llu\n", total_page_map);
	//fprintf(stderr, "Recieve Apply Page Time %llu\n", total_apply_time);
	//
	t = 0;
	fprintf(stderr, "Receive Map Time:");
	for (i = 0; ; i++) {
		if (total_page_map[i] != 0) {
			fprintf(stderr, "%llu\t", total_page_map[i]);
			t += total_page_map[i];
		}
		else {
			break;
		}
	}
	fprintf(stderr, "\nReceive Total Map Time: %llu\n", t);

	t = 0;
	fprintf(stderr, "Receive Top Apply Time:");
	for (i = 0; ; i++) {
		if (total_apply_time[i] != 0) {
			fprintf(stderr, "%llu\t", total_apply_time[i]);
			t += total_apply_time[i];
		}
		else {
			break;
		}
	}
	fprintf(stderr, "\nReceive Total Apply Time: %llu\n", t);
    /* HVM success! */
    rc = 0;

 out:
	fprintf(stderr, "rc = %d\n", rc);
    if ( (rc != 0) && (dom != 0) )
        xc_domain_destroy(xch, dom);
    xc_hypercall_buffer_free(xch, ctxt);
    free(mmu);
    free(ctx->p2m);
    //free(pfn_type);
    tailbuf_free(&tailbuf);

    /* discard cache for save file  */
    discard_file_cache(xch, io_fd, 1 /*flush*/);

    fcntl(io_fd, F_SETFL, orig_io_fd_flags);

    DPRINTF("Restore exit with rc=%d\n", rc);

    return rc;
}
/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
