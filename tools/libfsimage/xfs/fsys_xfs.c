/* fsys_xfs.c - an implementation for the SGI XFS file system */
/*  
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2001,2002,2004  Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <xenfsimage_grub.h>
#include <xen-tools/libs.h>
#include "xfs.h"

#define MAX_LINK_COUNT	8

typedef struct xad {
	xfs_fileoff_t offset;
	xfs_fsblock_t start;
	xfs_filblks_t len;
} xad_t;

struct xfs_info {
	int bsize;
	int dirbsize;
	int isize;
	unsigned int agblocks;
	int bdlog;
	int blklog;
	int inopblog;
	int agblklog;
	unsigned int nextents;
	xfs_daddr_t next;
	xfs_daddr_t daddr;
	xfs_dablk_t forw;
	xfs_dablk_t dablk;
	xfs_bmbt_rec_32_t *xt;
	xfs_bmbt_ptr_t ptr0;
	int btnode_ptr0_off;
	int i8param;
	int dirpos;
	int dirmax;
	int blkoff;
	int fpos;
	xfs_ino_t rootino;
};

static struct xfs_info xfs;

#define dirbuf		((char *)FSYS_BUF)
#define filebuf		((char *)FSYS_BUF + 4096)
#define inode		((xfs_dinode_t *)((char *)FSYS_BUF + 8192))
#define icore		(inode->di_core)

#define	mask32lo(n)	((xfs_uint32_t)((1ull << (n)) - 1))

#define	XFS_INO_MASK(k)		((xfs_uint32_t)((1ULL << (k)) - 1))
#define	XFS_INO_OFFSET_BITS	xfs.inopblog
#define	XFS_INO_AGINO_BITS	(xfs.agblklog + xfs.inopblog)

static inline xfs_agblock_t
agino2agbno (xfs_agino_t agino)
{
	return agino >> XFS_INO_OFFSET_BITS;
}

static inline xfs_agnumber_t
ino2agno (xfs_ino_t ino)
{
	return ino >> XFS_INO_AGINO_BITS;
}

static inline xfs_agino_t
ino2agino (xfs_ino_t ino)
{
	return ino & XFS_INO_MASK(XFS_INO_AGINO_BITS);
}

static inline int
ino2offset (xfs_ino_t ino)
{
	return ino & XFS_INO_MASK(XFS_INO_OFFSET_BITS);
}

static inline xfs_uint16_t
le16 (xfs_uint16_t x)
{
	__asm__("xchgb %b0,%h0"	\
		: "=Q" (x) \
		:  "0" (x)); \
		return x;
}

static inline xfs_uint32_t
le32 (xfs_uint32_t x)
{
#if 0
        /* 386 doesn't have bswap.  */
	__asm__("bswap %0" : "=r" (x) : "0" (x));
#else
	/* This is slower but this works on all x86 architectures.  */
	__asm__("xchgb %b0, %h0" \
		"\n\troll $16, %0" \
		"\n\txchgb %b0, %h0" \
		: "=Q" (x) : "0" (x));
#endif
	return x;
}

static inline xfs_uint64_t
le64 (xfs_uint64_t x)
{
	xfs_uint32_t h = x >> 32;
        xfs_uint32_t l = x & ((1ULL<<32)-1);
        return (((xfs_uint64_t)le32(l)) << 32) | ((xfs_uint64_t)(le32(h)));
}


static xfs_fsblock_t
xt_start (xfs_bmbt_rec_32_t *r)
{
	return (((xfs_fsblock_t)(le32 (r->l1) & mask32lo(9))) << 43) | 
	       (((xfs_fsblock_t)le32 (r->l2)) << 11) |
	       (((xfs_fsblock_t)le32 (r->l3)) >> 21);
}

static xfs_fileoff_t
xt_offset (xfs_bmbt_rec_32_t *r)
{
	return (((xfs_fileoff_t)le32 (r->l0) &
		mask32lo(31)) << 23) |
		(((xfs_fileoff_t)le32 (r->l1)) >> 9);
}

static xfs_filblks_t
xt_len (xfs_bmbt_rec_32_t *r)
{
	return le32(r->l3) & mask32lo(21);
}

static int
isinxt (xfs_fileoff_t key, xfs_fileoff_t offset, xfs_filblks_t len)
{
	return (key >= offset) ? (key < offset + len ? 1 : 0) : 0;
}

static xfs_daddr_t
agb2daddr (xfs_agnumber_t agno, xfs_agblock_t agbno)
{
	return ((xfs_fsblock_t)agno*xfs.agblocks + agbno) << xfs.bdlog;
}

static xfs_daddr_t
fsb2daddr (xfs_fsblock_t fsbno)
{
	return agb2daddr ((xfs_agnumber_t)(fsbno >> xfs.agblklog),
			 (xfs_agblock_t)(fsbno & mask32lo(xfs.agblklog)));
}

#undef offsetof
#define offsetof(t,m)	((size_t)&(((t *)0)->m))

static inline int
btroot_maxrecs (fsi_file_t *ffi)
{
	int tmp = icore.di_forkoff ? (icore.di_forkoff << 3) : xfs.isize;

	return (tmp - sizeof(xfs_bmdr_block_t) - offsetof(xfs_dinode_t, di_u)) /
		(sizeof (xfs_bmbt_key_t) + sizeof (xfs_bmbt_ptr_t));
}

static int
di_read (fsi_file_t *ffi, xfs_ino_t ino)
{
	xfs_agino_t agino;
	xfs_agnumber_t agno;
	xfs_agblock_t agbno;
	xfs_daddr_t daddr;
	int offset;

	agno = ino2agno (ino);
	agino = ino2agino (ino);
	agbno = agino2agbno (agino);
	offset = ino2offset (ino);
	daddr = agb2daddr (agno, agbno);

	devread (ffi, daddr, offset*xfs.isize, xfs.isize, (char *)inode);

	xfs.ptr0 = *(xfs_bmbt_ptr_t *)
		    (inode->di_u.di_c + sizeof(xfs_bmdr_block_t)
		    + btroot_maxrecs (ffi)*sizeof(xfs_bmbt_key_t));

	return 1;
}

static void
init_extents (fsi_file_t *ffi)
{
	xfs_bmbt_ptr_t ptr0;
	xfs_btree_lblock_t h;

	switch (icore.di_format) {
	case XFS_DINODE_FMT_EXTENTS:
		xfs.xt = inode->di_u.di_bmx;
		xfs.nextents = le32 (icore.di_nextents);
		break;
	case XFS_DINODE_FMT_BTREE:
		ptr0 = xfs.ptr0;
		for (;;) {
			xfs.daddr = fsb2daddr (le64(ptr0));
			devread (ffi, xfs.daddr, 0,
				 sizeof(xfs_btree_lblock_t), (char *)&h);
			if (!h.bb_level) {
				xfs.nextents = le16(h.bb_numrecs);
				xfs.next = fsb2daddr (le64(h.bb_rightsib));
				xfs.fpos = sizeof(xfs_btree_block_t);
				return;
			}
			devread (ffi, xfs.daddr, xfs.btnode_ptr0_off,
				 sizeof(xfs_bmbt_ptr_t), (char *)&ptr0);
		}
	}
}

static xad_t *
next_extent (fsi_file_t *ffi)
{
	static xad_t xad;

	switch (icore.di_format) {
	case XFS_DINODE_FMT_EXTENTS:
		if (xfs.nextents == 0)
			return NULL;
		break;
	case XFS_DINODE_FMT_BTREE:
		if (xfs.nextents == 0) {
			xfs_btree_lblock_t h;
			if (xfs.next == 0)
				return NULL;
			xfs.daddr = xfs.next;
			devread (ffi, xfs.daddr, 0, sizeof(xfs_btree_lblock_t), (char *)&h);
			xfs.nextents = le16(h.bb_numrecs);
			xfs.next = fsb2daddr (le64(h.bb_rightsib));
			xfs.fpos = sizeof(xfs_btree_block_t);
		}
		/* Yeah, I know that's slow, but I really don't care */
		devread (ffi, xfs.daddr, xfs.fpos, sizeof(xfs_bmbt_rec_t), filebuf);
		xfs.xt = (xfs_bmbt_rec_32_t *)filebuf;
		xfs.fpos += sizeof(xfs_bmbt_rec_32_t);
	}
	xad.offset = xt_offset (xfs.xt);
	xad.start = xt_start (xfs.xt);
	xad.len = xt_len (xfs.xt);
	++xfs.xt;
	--xfs.nextents;

	return &xad;
}

/*
 * Name lies - the function reads only first 100 bytes
 */
static void
xfs_dabread (fsi_file_t *ffi)
{
	xad_t *xad;
	xfs_fileoff_t offset;;

	init_extents (ffi);
	while ((xad = next_extent (ffi))) {
		offset = xad->offset;
		if (isinxt (xfs.dablk, offset, xad->len)) {
			devread (ffi, fsb2daddr (xad->start + xfs.dablk - offset),
				 0, 100, dirbuf);
			break;
		}
	}
}

static inline xfs_ino_t
sf_ino (char *sfe, int namelen)
{
	void *p = sfe + namelen + 3;

	return (xfs.i8param == 0)
		? le64(*(xfs_ino_t *)p) : le32(*(xfs_uint32_t *)p);
}

static inline xfs_ino_t
sf_parent_ino (fsi_file_t *ffi)
{
	return (xfs.i8param == 0)
		? le64(*(xfs_ino_t *)(&inode->di_u.di_dir2sf.hdr.parent))
		: le32(*(xfs_uint32_t *)(&inode->di_u.di_dir2sf.hdr.parent));
}

static inline int
roundup8 (int n)
{
	return ((n+7)&~7);
}

static int
xfs_read (fsi_file_t *ffi, char *buf, int len);

static char *
next_dentry (fsi_file_t *ffi, xfs_ino_t *ino)
{
	int namelen = 1;
	int toread;
	static char usual[2][3] = {".", ".."};
	static xfs_dir2_sf_entry_t *sfe;
	char *name = usual[0];

	if (xfs.dirpos >= xfs.dirmax) {
		if (xfs.forw == 0)
			return NULL;
		xfs.dablk = xfs.forw;
		xfs_dabread (ffi);
#define h	((xfs_dir2_leaf_hdr_t *)dirbuf)
		xfs.dirmax = le16 (h->count) - le16 (h->stale);
		xfs.forw = le32 (h->info.forw);
#undef h
		xfs.dirpos = 0;
	}

	switch (icore.di_format) {
	case XFS_DINODE_FMT_LOCAL:
		switch (xfs.dirpos) {
		case -2:
			*ino = 0;
			break;
		case -1:
			*ino = sf_parent_ino (ffi);
			++name;
			++namelen;
			sfe = (xfs_dir2_sf_entry_t *)
				(inode->di_u.di_c 
				 + sizeof(xfs_dir2_sf_hdr_t)
				 - xfs.i8param);
			break;
		default:
			namelen = sfe->namelen;
			*ino = sf_ino ((char *)sfe, namelen);
			name = (char *)sfe->name;
			sfe = (xfs_dir2_sf_entry_t *)
				  ((char *)sfe + namelen + 11 - xfs.i8param);
		}
		break;
	case XFS_DINODE_FMT_BTREE:
	case XFS_DINODE_FMT_EXTENTS:
#define dau	((xfs_dir2_data_union_t *)dirbuf)
		for (;;) {
			if (xfs.blkoff >= xfs.dirbsize) {
				xfs.blkoff = sizeof(xfs_dir2_data_hdr_t);
				filepos &= ~(xfs.dirbsize - 1);
				filepos |= xfs.blkoff;
			}
			xfs_read (ffi, dirbuf, 4);
			xfs.blkoff += 4;
			if (dau->unused.freetag == XFS_DIR2_DATA_FREE_TAG) {
				toread = roundup8 (le16(dau->unused.length)) - 4;
				xfs.blkoff += toread;
				filepos += toread;
				continue;
			}
			break;
		}
		xfs_read (ffi, (char *)dirbuf + 4, 5);
		*ino = le64 (dau->entry.inumber);
		namelen = dau->entry.namelen;
#undef dau
		toread = roundup8 (namelen + 11) - 9;
		xfs_read (ffi, dirbuf, toread);
		name = (char *)dirbuf;
		xfs.blkoff += toread + 5;
	}
	++xfs.dirpos;
	name[namelen] = 0;

	return name;
}

static char *
first_dentry (fsi_file_t *ffi, xfs_ino_t *ino)
{
	xfs.forw = 0;
	switch (icore.di_format) {
	case XFS_DINODE_FMT_LOCAL:
		xfs.dirmax = inode->di_u.di_dir2sf.hdr.count;
		xfs.i8param = inode->di_u.di_dir2sf.hdr.i8count ? 0 : 4;
		xfs.dirpos = -2;
		break;
	case XFS_DINODE_FMT_EXTENTS:
	case XFS_DINODE_FMT_BTREE:
		filepos = 0;
		xfs_read (ffi, dirbuf, sizeof(xfs_dir2_data_hdr_t));
		if (((xfs_dir2_data_hdr_t *)dirbuf)->magic == le32(XFS_DIR2_BLOCK_MAGIC)) {
#define tail		((xfs_dir2_block_tail_t *)dirbuf)
			filepos = xfs.dirbsize - sizeof(*tail);
			xfs_read (ffi, dirbuf, sizeof(*tail));
			xfs.dirmax = le32 (tail->count) - le32 (tail->stale);
#undef tail
		} else {
			xfs.dablk = (1ULL << 35) >> xfs.blklog;
#define h		((xfs_dir2_leaf_hdr_t *)dirbuf)
#define n		((xfs_da_intnode_t *)dirbuf)
			for (;;) {
				xfs_dabread (ffi);
				if ((n->hdr.info.magic == le16(XFS_DIR2_LEAFN_MAGIC))
				    || (n->hdr.info.magic == le16(XFS_DIR2_LEAF1_MAGIC))) {
					xfs.dirmax = le16 (h->count) - le16 (h->stale);
					xfs.forw = le32 (h->info.forw);
					break;
				}
				xfs.dablk = le32 (n->btree[0].before);
			}
#undef n
#undef h
		}
		xfs.blkoff = sizeof(xfs_dir2_data_hdr_t);
		filepos = xfs.blkoff;
		xfs.dirpos = 0;
	}
	return next_dentry (ffi, ino);
}

static bool
xfs_sb_is_invalid (const xfs_sb_t *super)
{
	return (le32(super->sb_magicnum) != XFS_SB_MAGIC)
	    || ((le16(super->sb_versionnum) & XFS_SB_VERSION_NUMBITS) !=
	        XFS_SB_VERSION_4)
	    || (super->sb_inodelog < XFS_SB_INODELOG_MIN)
	    || (super->sb_inodelog > XFS_SB_INODELOG_MAX)
	    || (super->sb_blocklog < XFS_SB_BLOCKLOG_MIN)
	    || (super->sb_blocklog > XFS_SB_BLOCKLOG_MAX)
	    || (super->sb_blocklog < super->sb_inodelog)
	    || (super->sb_agblklog > XFS_SB_AGBLKLOG_MAX)
	    || ((1ull << super->sb_agblklog) < le32(super->sb_agblocks))
	    || (((1ull << super->sb_agblklog) >> 1) >=
	        le32(super->sb_agblocks))
	    || ((super->sb_blocklog + super->sb_dirblklog) >=
	        XFS_SB_DIRBLK_NUMBITS);
}

static int
xfs_mount (fsi_file_t *ffi, const char *options)
{
	xfs_sb_t super;

	if (!devread (ffi, 0, 0, sizeof(super), (char *)&super)
	    || xfs_sb_is_invalid(&super)) {
		return 0;
	}

	/*
	 * Not sanitized. It's exclusively used to generate disk addresses,
	 * so it's not important from a security standpoint.
	 */
	xfs.rootino = le64 (super.sb_rootino);

	/*
	 * Sanitized to be consistent with each other, only used to
	 * generate disk addresses, so it's safe
	 */
	xfs.agblocks = le32 (super.sb_agblocks);
	xfs.agblklog = super.sb_agblklog;

	/* Derived from sanitized parameters */
	BUILD_BUG_ON(XFS_SB_BLOCKLOG_MIN < SECTOR_BITS);
	xfs.bdlog = super.sb_blocklog - SECTOR_BITS;
	xfs.bsize = 1 << super.sb_blocklog;
	xfs.blklog = super.sb_blocklog;
	xfs.isize = 1 << super.sb_inodelog;
	xfs.dirbsize = 1 << (super.sb_blocklog + super.sb_dirblklog);
	xfs.inopblog = super.sb_blocklog - super.sb_inodelog;

	xfs.btnode_ptr0_off =
		((xfs.bsize - sizeof(xfs_btree_block_t)) /
		(sizeof (xfs_bmbt_key_t) + sizeof (xfs_bmbt_ptr_t)))
		 * sizeof(xfs_bmbt_key_t) + sizeof(xfs_btree_block_t);

	return 1;
}

static int
xfs_read (fsi_file_t *ffi, char *buf, int len)
{
	xad_t *xad;
	xfs_fileoff_t endofprev, endofcur, offset;
	xfs_filblks_t xadlen;
	int toread, startpos, endpos;

	if (icore.di_format == XFS_DINODE_FMT_LOCAL) {
		grub_memmove (buf, inode->di_u.di_c + filepos, len);
		filepos += len;
		return len;
	}

	startpos = filepos;
	endpos = filepos + len;
	endofprev = (xfs_fileoff_t)-1;
	init_extents (ffi);
	while (len > 0 && (xad = next_extent (ffi))) {
		offset = xad->offset;
		xadlen = xad->len;
		if (isinxt (filepos >> xfs.blklog, offset, xadlen)) {
			endofcur = (offset + xadlen) << xfs.blklog; 
			toread = (endofcur >= endpos)
				  ? len : (endofcur - filepos);

			disk_read_func = disk_read_hook;
			devread (ffi, fsb2daddr (xad->start),
				 filepos - (offset << xfs.blklog), toread, buf);
			disk_read_func = NULL;

			buf += toread;
			len -= toread;
			filepos += toread;
		} else if (offset > endofprev) {
			toread = ((offset << xfs.blklog) >= endpos)
				  ? len : ((offset - endofprev) << xfs.blklog);
			len -= toread;
			filepos += toread;
			for (; toread; toread--) {
				*buf++ = 0;
			}
			continue;
		}
		endofprev = offset + xadlen; 
	}

	return filepos - startpos;
}

static int
xfs_dir (fsi_file_t *ffi, char *dirname)
{
	xfs_ino_t ino, parent_ino, new_ino;
	xfs_fsize_t di_size;
	int di_mode;
	int cmp, n, link_count;
	char linkbuf[xfs.bsize];
	char *rest, *name, ch;

	parent_ino = ino = xfs.rootino;
	link_count = 0;
	for (;;) {
		di_read (ffi, ino);
		di_size = le64 (icore.di_size);
		di_mode = le16 (icore.di_mode);

		if ((di_mode & IFMT) == IFLNK) {
			if (++link_count > MAX_LINK_COUNT) {
				errnum = ERR_SYMLINK_LOOP;
				return 0;
			}
			if (di_size < xfs.bsize - 1) {
				filepos = 0;
				filemax = di_size;
				n = xfs_read (ffi, linkbuf, filemax);
			} else {
				errnum = ERR_FILELENGTH;
				return 0;
			}

			ino = (linkbuf[0] == '/') ? xfs.rootino : parent_ino;
			while (n < (xfs.bsize - 1) && (linkbuf[n++] = *dirname++));
			linkbuf[n] = 0;
			dirname = linkbuf;
			continue;
		}

		if (!*dirname || isspace ((uint8_t)*dirname)) {
			if ((di_mode & IFMT) != IFREG) {
				errnum = ERR_BAD_FILETYPE;
				return 0;
			}
			filepos = 0;
			filemax = di_size;
			return 1;
		}

		if ((di_mode & IFMT) != IFDIR) {
			errnum = ERR_BAD_FILETYPE;
			return 0;
		}

		for (; *dirname == '/'; dirname++);

		for (rest = dirname; (ch = *rest)
                   && !isspace ((uint8_t)ch) && ch != '/'; rest++);
		*rest = 0;

		name = first_dentry (ffi, &new_ino);
		for (;;) {
			cmp = (!*dirname) ? -1 : substring (dirname, name);
#ifndef STAGE1_5
			if (print_possibilities && ch != '/' && cmp <= 0) {
				if (print_possibilities > 0)
					print_possibilities = -print_possibilities;
				print_a_completion (name);
			} else
#endif
			if (cmp == 0) {
				parent_ino = ino;
				if (new_ino)
					ino = new_ino;
		        	*(dirname = rest) = ch;
				break;
			}
			name = next_dentry (ffi, &new_ino);
			if (name == NULL) {
				if (print_possibilities < 0)
					return 1;

				errnum = ERR_FILE_NOT_FOUND;
				*rest = ch;
				return 0;
			}
		}
	}
}

fsi_plugin_ops_t *
fsi_init_plugin(int version, fsi_plugin_t *fp, const char **name)
{
	static fsig_plugin_ops_t ops = {
		FSIMAGE_PLUGIN_VERSION,
		.fpo_mount = xfs_mount,
		.fpo_dir = xfs_dir,
		.fpo_read = xfs_read
	};

	*name = "xfs";
	return (fsig_init(fp, &ops));
}
