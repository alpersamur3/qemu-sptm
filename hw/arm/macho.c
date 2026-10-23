#include "qemu/osdep.h"
#include "xnu/macho.h"
#include "exec/memattrs.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "cpu.h"

#define NEXT(c) (( (lc_t*) (((u8*)((c))) + (((c))->cmdsize)) ))

// We assume all files are img4 unwrapped; this will tell us if the user
// accidentally forgot to unwrap something. We do not want any im4ps.
bool is_im4p(u8 *macho) {
    for (size_t i = 0; i < 0x30; i++) {
        if (0 == memcmp((char*)(&macho[i]), "IM4P", 4)) {
            return true;
        }
    }

    return false;
}

void macho_verify_header(u8 *macho, bool is_kc) {
    mh_t *header = (mh_t*)macho;
    if (MH_MAGIC_64 != header->magic) {
        fprintf(stderr, "error: macho header magic mismatch\n");
        exit(1);
    }

    if (CPU_TYPE_ARM64 != header->cputype) {
        fprintf(stderr, "error: not an aarch64 macho\n");
        exit(1);
    }

    if (is_kc && MH_FILESET != header->filetype) {
        fprintf(stderr, "warning: not an MH_FILESET kernelcache\n");
    }

    if (!is_kc && MH_EXECUTE != header->filetype) {
        fprintf(stderr, "warning: not an MH_EXECUTE macho\n");
    }
}

macho_info_t macho_get_info(u8 *macho) {
    mh_t *header = (mh_t*)macho;
    lc_t *cursor;
    hwaddr virtlo = (hwaddr)-1;
    hwaddr virthi = 0;
    hwaddr entrypoint = (hwaddr)-1;

    cursor = (lc_t*)(header+1);
    for (usize i = 0; i < header->ncmds; i++) {
        if (LC_SEGMENT_64 == cursor->cmd) {
            seg_t *seg = (seg_t*)(cursor);
            if (virtlo > seg->vmaddr) virtlo = seg->vmaddr;
            if (virthi < seg->vmaddr + seg->vmsize) virthi = seg->vmaddr + seg->vmsize;
        }

        if (LC_UNIXTHREAD == cursor->cmd) {
            assert(-1 == entrypoint);
            struct aarch64_thread_state *t = (struct aarch64_thread_state*)(cursor+1);
            entrypoint = t->pc;
        }

        if (LC_MAIN == cursor->cmd) {
            assert(-1 == entrypoint);
            struct entry_point_command *cmd = (struct entry_point_command*)(cursor);
            entrypoint = cmd->entryoff;
        }

        cursor = NEXT(cursor);
    }

    return (macho_info_t) {
        .macho  = macho,
        .physlo = 0, // we don't know this until the macho gets loaded
        .virtlo = virtlo,
        .virthi = virthi,
        .entrypoint = entrypoint,
    };
}

void macho_load_seg_at(macho_info_t *mi, seg_t *lc, hwaddr paddr) {
    address_space_write(
        &address_space_memory,
        paddr,
        MEMTXATTRS_UNSPECIFIED,
        &mi->macho[lc->fileoff],
        lc->filesize
    );

    if (lc->vmsize > lc->filesize) {
        address_space_set(
            &address_space_memory,
            paddr + lc->filesize,
            0,
            lc->vmsize - lc->filesize,
            MEMTXATTRS_UNSPECIFIED
        );
    }
}

void macho_load(macho_info_t *mi, hwaddr phys_base) {
    mh_t *header = (mh_t*)mi->macho;
    lc_t *cursor = (lc_t*)(header+1);

    mi->physlo = phys_base;
    for (usize i = 0; i < header->ncmds; i++) {
        if (LC_SEGMENT_64 == cursor->cmd) {
            seg_t *lc = (seg_t*)(cursor);
            macho_load_seg_at(mi, lc, lc->vmaddr - mi->virtlo + phys_base);
        }

        cursor = NEXT(cursor);
    }
}

fse_t *macho_find_fileset_entry(u8 *macho, const char *name) {
    mh_t *header = (mh_t*)macho;
    lc_t *cursor = (lc_t*)(header+1);

    for (size_t i = 0; i < header->ncmds; i++) {
        if (LC_FILESET_ENTRY == cursor->cmd) {
            fse_t *f = (fse_t*)cursor;
            // entry_id.offset, not f+1: on LLP64 (Windows) union lc_str also
            // holds a pointer, so sizeof(fse_t) isn't the file layout's 32
            if (0 == strcmp(name, (char*)f + f->entry_id.offset)) return f;
        }
        cursor = NEXT(cursor);
    }

    return NULL;
}

seg_t *macho_find_seg(u8 *macho, const char *name) {
    mh_t *header = (mh_t*)macho;
    lc_t *cursor = (lc_t*)(header+1);

    for (size_t i = 0; i < header->ncmds; i++) {
        if (LC_SEGMENT_64 == cursor->cmd) {
            seg_t *s = (seg_t*)cursor;
            if (0 == strcmp(name, s->segname)) return s;
        }
        cursor = NEXT(cursor);
    }

    return NULL;
}

sect_t *macho_find_sect(u8 *macho, const char *segname, const char *sectname) {
    sect_t *sects = NULL;
    seg_t *seg = macho_find_seg(macho, segname);
    if (!seg) return NULL;

    sects = (sect_t*)(seg+1);
    for (size_t i = 0; i < seg->nsects; i++) {
        if (0 == strcmp(sectname, sects[i].sectname)) return &sects[i];
    }

    return NULL;
}
