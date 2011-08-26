/*
 * mdadm - Intel(R) Matrix Storage Manager Support
 *
 * Copyright (C) 2002-2008 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define HAVE_STDINT_H 1
#include "mdadm.h"
#include "mdmon.h"
#include "sha1.h"
#include "platform-intel.h"
#include <values.h>
#include <scsi/sg.h>
#include <ctype.h>
#include <dirent.h>

/* MPB == Metadata Parameter Block */
#define MPB_SIGNATURE "Intel Raid ISM Cfg Sig. "
#define MPB_SIG_LEN (strlen(MPB_SIGNATURE))
#define MPB_VERSION_RAID0 "1.0.00"
#define MPB_VERSION_RAID1 "1.1.00"
#define MPB_VERSION_MANY_VOLUMES_PER_ARRAY "1.2.00"
#define MPB_VERSION_3OR4_DISK_ARRAY "1.2.01"
#define MPB_VERSION_RAID5 "1.2.02"
#define MPB_VERSION_5OR6_DISK_ARRAY "1.2.04"
#define MPB_VERSION_CNG "1.2.06"
#define MPB_VERSION_ATTRIBS "1.3.00"
#define MAX_SIGNATURE_LENGTH  32
#define MAX_RAID_SERIAL_LEN   16

/* supports RAID0 */
#define MPB_ATTRIB_RAID0		__cpu_to_le32(0x00000001)
/* supports RAID1 */
#define MPB_ATTRIB_RAID1		__cpu_to_le32(0x00000002)
/* supports RAID10 */
#define MPB_ATTRIB_RAID10		__cpu_to_le32(0x00000004)
/* supports RAID1E */
#define MPB_ATTRIB_RAID1E		__cpu_to_le32(0x00000008)
/* supports RAID5 */
#define MPB_ATTRIB_RAID5		__cpu_to_le32(0x00000010)
/* supports RAID CNG */
#define MPB_ATTRIB_RAIDCNG		__cpu_to_le32(0x00000020)
/* supports expanded stripe sizes of  256K, 512K and 1MB */
#define MPB_ATTRIB_EXP_STRIPE_SIZE	__cpu_to_le32(0x00000040)

/* The OROM Support RST Caching of Volumes */
#define MPB_ATTRIB_NVM			__cpu_to_le32(0x02000000)
/* The OROM supports creating disks greater than 2TB */
#define MPB_ATTRIB_2TB_DISK		__cpu_to_le32(0x04000000)
/* The OROM supports Bad Block Management */
#define MPB_ATTRIB_BBM			__cpu_to_le32(0x08000000)

/* THe OROM Supports NVM Caching of Volumes */
#define MPB_ATTRIB_NEVER_USE2           __cpu_to_le32(0x10000000)
/* The OROM supports creating volumes greater than 2TB */
#define MPB_ATTRIB_2TB			__cpu_to_le32(0x20000000)
/* originally for PMP, now it's wasted b/c. Never use this bit! */
#define MPB_ATTRIB_NEVER_USE		__cpu_to_le32(0x40000000)
/* Verify MPB contents against checksum after reading MPB */
#define MPB_ATTRIB_CHECKSUM_VERIFY	__cpu_to_le32(0x80000000)

/* Define all supported attributes that have to be accepted by mdadm
 */
#define MPB_ATTRIB_SUPPORTED	       (MPB_ATTRIB_CHECKSUM_VERIFY | \
					MPB_ATTRIB_2TB             | \
					MPB_ATTRIB_2TB_DISK        | \
					MPB_ATTRIB_RAID0           | \
					MPB_ATTRIB_RAID1           | \
					MPB_ATTRIB_RAID10          | \
					MPB_ATTRIB_RAID5           | \
					MPB_ATTRIB_EXP_STRIPE_SIZE)

/* Define attributes that are unused but not harmful */
#define MPB_ATTRIB_IGNORED		(MPB_ATTRIB_NEVER_USE)

#define MPB_SECTOR_CNT 2210
#define IMSM_RESERVED_SECTORS 4096
#define SECT_PER_MB_SHIFT 11

/* Disk configuration info. */
#define IMSM_MAX_DEVICES 255
struct imsm_disk {
	__u8 serial[MAX_RAID_SERIAL_LEN];/* 0xD8 - 0xE7 ascii serial number */
	__u32 total_blocks;		 /* 0xE8 - 0xEB total blocks */
	__u32 scsi_id;			 /* 0xEC - 0xEF scsi ID */
#define SPARE_DISK      __cpu_to_le32(0x01)  /* Spare */
#define CONFIGURED_DISK __cpu_to_le32(0x02)  /* Member of some RaidDev */
#define FAILED_DISK     __cpu_to_le32(0x04)  /* Permanent failure */
	__u32 status;			 /* 0xF0 - 0xF3 */
	__u32 owner_cfg_num; /* which config 0,1,2... owns this disk */ 
#define	IMSM_DISK_FILLERS	4
	__u32 filler[IMSM_DISK_FILLERS]; /* 0xF4 - 0x107 MPB_DISK_FILLERS for future expansion */
};

/* RAID map configuration infos. */
struct imsm_map {
	__u32 pba_of_lba0;	/* start address of partition */
	__u32 blocks_per_member;/* blocks per member */
	__u32 num_data_stripes;	/* number of data stripes */
	__u16 blocks_per_strip;
	__u8  map_state;	/* Normal, Uninitialized, Degraded, Failed */
#define IMSM_T_STATE_NORMAL 0
#define IMSM_T_STATE_UNINITIALIZED 1
#define IMSM_T_STATE_DEGRADED 2
#define IMSM_T_STATE_FAILED 3
	__u8  raid_level;
#define IMSM_T_RAID0 0
#define IMSM_T_RAID1 1
#define IMSM_T_RAID5 5		/* since metadata version 1.2.02 ? */
	__u8  num_members;	/* number of member disks */
	__u8  num_domains;	/* number of parity domains */
	__u8  failed_disk_num;  /* valid only when state is degraded */
	__u8  ddf;
	__u32 filler[7];	/* expansion area */
#define IMSM_ORD_REBUILD (1 << 24)
	__u32 disk_ord_tbl[1];	/* disk_ord_tbl[num_members],
				 * top byte contains some flags
				 */
} __attribute__ ((packed));

struct imsm_vol {
	__u32 curr_migr_unit;
	__u32 checkpoint_id;	/* id to access curr_migr_unit */
	__u8  migr_state;	/* Normal or Migrating */
#define MIGR_INIT 0
#define MIGR_REBUILD 1
#define MIGR_VERIFY 2 /* analagous to echo check > sync_action */
#define MIGR_GEN_MIGR 3
#define MIGR_STATE_CHANGE 4
#define MIGR_REPAIR 5
	__u8  migr_type;	/* Initializing, Rebuilding, ... */
	__u8  dirty;
	__u8  fs_state;		/* fast-sync state for CnG (0xff == disabled) */
	__u16 verify_errors;	/* number of mismatches */
	__u16 bad_blocks;	/* number of bad blocks during verify */
	__u32 filler[4];
	struct imsm_map map[1];
	/* here comes another one if migr_state */
} __attribute__ ((packed));

struct imsm_dev {
	__u8  volume[MAX_RAID_SERIAL_LEN];
	__u32 size_low;
	__u32 size_high;
#define DEV_BOOTABLE		__cpu_to_le32(0x01)
#define DEV_BOOT_DEVICE		__cpu_to_le32(0x02)
#define DEV_READ_COALESCING	__cpu_to_le32(0x04)
#define DEV_WRITE_COALESCING	__cpu_to_le32(0x08)
#define DEV_LAST_SHUTDOWN_DIRTY	__cpu_to_le32(0x10)
#define DEV_HIDDEN_AT_BOOT	__cpu_to_le32(0x20)
#define DEV_CURRENTLY_HIDDEN	__cpu_to_le32(0x40)
#define DEV_VERIFY_AND_FIX	__cpu_to_le32(0x80)
#define DEV_MAP_STATE_UNINIT	__cpu_to_le32(0x100)
#define DEV_NO_AUTO_RECOVERY	__cpu_to_le32(0x200)
#define DEV_CLONE_N_GO		__cpu_to_le32(0x400)
#define DEV_CLONE_MAN_SYNC	__cpu_to_le32(0x800)
#define DEV_CNG_MASTER_DISK_NUM	__cpu_to_le32(0x1000)
	__u32 status;	/* Persistent RaidDev status */
	__u32 reserved_blocks; /* Reserved blocks at beginning of volume */
	__u8  migr_priority;
	__u8  num_sub_vols;
	__u8  tid;
	__u8  cng_master_disk;
	__u16 cache_policy;
	__u8  cng_state;
	__u8  cng_sub_state;
#define IMSM_DEV_FILLERS 10
	__u32 filler[IMSM_DEV_FILLERS];
	struct imsm_vol vol;
} __attribute__ ((packed));

struct imsm_super {
	__u8 sig[MAX_SIGNATURE_LENGTH];	/* 0x00 - 0x1F */
	__u32 check_sum;		/* 0x20 - 0x23 MPB Checksum */
	__u32 mpb_size;			/* 0x24 - 0x27 Size of MPB */
	__u32 family_num;		/* 0x28 - 0x2B Checksum from first time this config was written */
	__u32 generation_num;		/* 0x2C - 0x2F Incremented each time this array's MPB is written */
	__u32 error_log_size;		/* 0x30 - 0x33 in bytes */
	__u32 attributes;		/* 0x34 - 0x37 */
	__u8 num_disks;			/* 0x38 Number of configured disks */
	__u8 num_raid_devs;		/* 0x39 Number of configured volumes */
	__u8 error_log_pos;		/* 0x3A  */
	__u8 fill[1];			/* 0x3B */
	__u32 cache_size;		/* 0x3c - 0x40 in mb */
	__u32 orig_family_num;		/* 0x40 - 0x43 original family num */
	__u32 pwr_cycle_count;		/* 0x44 - 0x47 simulated power cycle count for array */
	__u32 bbm_log_size;		/* 0x48 - 0x4B - size of bad Block Mgmt Log in bytes */
#define IMSM_FILLERS 35
	__u32 filler[IMSM_FILLERS];	/* 0x4C - 0xD7 RAID_MPB_FILLERS */
	struct imsm_disk disk[1];	/* 0xD8 diskTbl[numDisks] */
	/* here comes imsm_dev[num_raid_devs] */
	/* here comes BBM logs */
} __attribute__ ((packed));

#define BBM_LOG_MAX_ENTRIES 254

struct bbm_log_entry {
	__u64 defective_block_start;
#define UNREADABLE 0xFFFFFFFF
	__u32 spare_block_offset;
	__u16 remapped_marked_count;
	__u16 disk_ordinal;
} __attribute__ ((__packed__));

struct bbm_log {
	__u32 signature; /* 0xABADB10C */
	__u32 entry_count;
	__u32 reserved_spare_block_count; /* 0 */
	__u32 reserved; /* 0xFFFF */
	__u64 first_spare_lba;
	struct bbm_log_entry mapped_block_entries[BBM_LOG_MAX_ENTRIES];
} __attribute__ ((__packed__));


#ifndef MDASSEMBLE
static char *map_state_str[] = { "normal", "uninitialized", "degraded", "failed" };
#endif

#define RAID_DISK_RESERVED_BLOCKS_IMSM_HI 2209

#define GEN_MIGR_AREA_SIZE 2048 /* General Migration Copy Area size in blocks */

#define UNIT_SRC_NORMAL     0   /* Source data for curr_migr_unit must
				 *  be recovered using srcMap */
#define UNIT_SRC_IN_CP_AREA 1   /* Source data for curr_migr_unit has
				 *  already been migrated and must
				 *  be recovered from checkpoint area */
struct migr_record {
	__u32 rec_status;	    /* Status used to determine how to restart
				     * migration in case it aborts
				     * in some fashion */
	__u32 curr_migr_unit;	    /* 0..numMigrUnits-1 */
	__u32 family_num;	    /* Family number of MPB
				     * containing the RaidDev
				     * that is migrating */
	__u32 ascending_migr;	    /* True if migrating in increasing
				     * order of lbas */
	__u32 blocks_per_unit;      /* Num disk blocks per unit of operation */
	__u32 dest_depth_per_unit;  /* Num member blocks each destMap
				     * member disk
				     * advances per unit-of-operation */
	__u32 ckpt_area_pba;	    /* Pba of first block of ckpt copy area */
	__u32 dest_1st_member_lba;  /* First member lba on first
				     * stripe of destination */
	__u32 num_migr_units;	    /* Total num migration units-of-op */
	__u32 post_migr_vol_cap;    /* Size of volume after
				     * migration completes */
	__u32 post_migr_vol_cap_hi; /* Expansion space for LBA64 */
	__u32 ckpt_read_disk_num;   /* Which member disk in destSubMap[0] the
				     * migration ckpt record was read from
				     * (for recovered migrations) */
} __attribute__ ((__packed__));

static __u8 migr_type(struct imsm_dev *dev)
{
	if (dev->vol.migr_type == MIGR_VERIFY &&
	    dev->status & DEV_VERIFY_AND_FIX)
		return MIGR_REPAIR;
	else
		return dev->vol.migr_type;
}

static void set_migr_type(struct imsm_dev *dev, __u8 migr_type)
{
	/* for compatibility with older oroms convert MIGR_REPAIR, into
	 * MIGR_VERIFY w/ DEV_VERIFY_AND_FIX status
	 */
	if (migr_type == MIGR_REPAIR) {
		dev->vol.migr_type = MIGR_VERIFY;
		dev->status |= DEV_VERIFY_AND_FIX;
	} else {
		dev->vol.migr_type = migr_type;
		dev->status &= ~DEV_VERIFY_AND_FIX;
	}
}

static unsigned int sector_count(__u32 bytes)
{
	return ((bytes + (512-1)) & (~(512-1))) / 512;
}

static unsigned int mpb_sectors(struct imsm_super *mpb)
{
	return sector_count(__le32_to_cpu(mpb->mpb_size));
}

struct intel_dev {
	struct imsm_dev *dev;
	struct intel_dev *next;
	unsigned index;
};

struct intel_hba {
	enum sys_dev_type type;
	char *path;
	char *pci_id;
	struct intel_hba *next;
};

enum action {
	DISK_REMOVE = 1,
	DISK_ADD
};
/* internal representation of IMSM metadata */
struct intel_super {
	union {
		void *buf; /* O_DIRECT buffer for reading/writing metadata */
		struct imsm_super *anchor; /* immovable parameters */
	};
	union {
		void *migr_rec_buf; /* buffer for I/O operations */
		struct migr_record *migr_rec; /* migration record */
	};
	size_t len; /* size of the 'buf' allocation */
	void *next_buf; /* for realloc'ing buf from the manager */
	size_t next_len;
	int updates_pending; /* count of pending updates for mdmon */
	int current_vol; /* index of raid device undergoing creation */
	__u32 create_offset; /* common start for 'current_vol' */
	__u32 random; /* random data for seeding new family numbers */
	struct intel_dev *devlist;
	struct dl {
		struct dl *next;
		int index;
		__u8 serial[MAX_RAID_SERIAL_LEN];
		int major, minor;
		char *devname;
		struct imsm_disk disk;
		int fd;
		int extent_cnt;
		struct extent *e; /* for determining freespace @ create */
		int raiddisk; /* slot to fill in autolayout */
		enum action action;
	} *disks, *current_disk;
	struct dl *disk_mgmt_list; /* list of disks to add/remove while mdmon
				      active */
	struct dl *missing; /* disks removed while we weren't looking */
	struct bbm_log *bbm_log;
	struct intel_hba *hba; /* device path of the raid controller for this metadata */
	const struct imsm_orom *orom; /* platform firmware support */
	struct intel_super *next; /* (temp) list for disambiguating family_num */
};

struct intel_disk {
	struct imsm_disk disk;
	#define IMSM_UNKNOWN_OWNER (-1)
	int owner;
	struct intel_disk *next;
};

struct extent {
	unsigned long long start, size;
};

/* definitions of reshape process types */
enum imsm_reshape_type {
	CH_TAKEOVER,
	CH_MIGRATION,
};

/* definition of messages passed to imsm_process_update */
enum imsm_update_type {
	update_activate_spare,
	update_create_array,
	update_kill_array,
	update_rename_array,
	update_add_remove_disk,
	update_reshape_container_disks,
	update_reshape_migration,
	update_takeover,
	update_general_migration_checkpoint,
};

struct imsm_update_activate_spare {
	enum imsm_update_type type;
	struct dl *dl;
	int slot;
	int array;
	struct imsm_update_activate_spare *next;
};

struct geo_params {
	int dev_id;
	char *dev_name;
	long long size;
	int level;
	int layout;
	int chunksize;
	int raid_disks;
};

enum takeover_direction {
	R10_TO_R0,
	R0_TO_R10
};
struct imsm_update_takeover {
	enum imsm_update_type type;
	int subarray;
	enum takeover_direction direction;
};

struct imsm_update_reshape {
	enum imsm_update_type type;
	int old_raid_disks;
	int new_raid_disks;

	int new_disks[1]; /* new_raid_disks - old_raid_disks makedev number */
};

struct imsm_update_reshape_migration {
	enum imsm_update_type type;
	int old_raid_disks;
	int new_raid_disks;
	/* fields for array migration changes
	 */
	int subdev;
	int new_level;
	int new_layout;
	int new_chunksize;

	int new_disks[1]; /* new_raid_disks - old_raid_disks makedev number */
};

struct imsm_update_general_migration_checkpoint {
	enum imsm_update_type type;
	__u32 curr_migr_unit;
};

struct disk_info {
	__u8 serial[MAX_RAID_SERIAL_LEN];
};

struct imsm_update_create_array {
	enum imsm_update_type type;
	int dev_idx;
	struct imsm_dev dev;
};

struct imsm_update_kill_array {
	enum imsm_update_type type;
	int dev_idx;
};

struct imsm_update_rename_array {
	enum imsm_update_type type;
	__u8 name[MAX_RAID_SERIAL_LEN];
	int dev_idx;
};

struct imsm_update_add_remove_disk {
	enum imsm_update_type type;
};


static const char *_sys_dev_type[] = {
	[SYS_DEV_UNKNOWN] = "Unknown",
	[SYS_DEV_SAS] = "SAS",
	[SYS_DEV_SATA] = "SATA"
};

const char *get_sys_dev_type(enum sys_dev_type type)
{
	if (type >= SYS_DEV_MAX)
		type = SYS_DEV_UNKNOWN;

	return _sys_dev_type[type];
}

static struct intel_hba * alloc_intel_hba(struct sys_dev *device)
{
	struct intel_hba *result = malloc(sizeof(*result));
	if (result) {
		result->type = device->type;
		result->path = strdup(device->path);
		result->next = NULL;
		if (result->path && (result->pci_id = strrchr(result->path, '/')) != NULL)
			result->pci_id++;
	}
	return result;
}

static struct intel_hba * find_intel_hba(struct intel_hba *hba, struct sys_dev *device)
{
	struct intel_hba *result=NULL;
	for (result = hba; result; result = result->next) {
		if (result->type == device->type && strcmp(result->path, device->path) == 0)
			break;
	}
	return result;
}

static int attach_hba_to_super(struct intel_super *super, struct sys_dev *device)
{
	struct intel_hba *hba;

	/* check if disk attached to Intel HBA */
	hba = find_intel_hba(super->hba, device);
	if (hba != NULL)
		return 1;
	/* Check if HBA is already attached to super */
	if (super->hba == NULL) {
		super->hba = alloc_intel_hba(device);
		return 1;
	}

	hba = super->hba;
	/* Intel metadata allows for all disks attached to the same type HBA.
	 * Do not sypport odf HBA types mixing
	 */
	if (device->type != hba->type)
		return 2;

	while (hba->next)
		hba = hba->next;

	hba->next = alloc_intel_hba(device);
	return 1;
}

static struct sys_dev* find_disk_attached_hba(int fd, const char *devname)
{
	struct sys_dev *list, *elem, *prev;
	char *disk_path;

	if ((list = find_intel_devices()) == NULL)
		return 0;

	if (fd < 0)
		disk_path  = (char *) devname;
	else
		disk_path = diskfd_to_devpath(fd);

	if (!disk_path) {
		free_sys_dev(&list);
		return 0;
	}

	for (prev = NULL, elem = list; elem; prev = elem, elem = elem->next) {
		if (path_attached_to_hba(disk_path, elem->path)) {
			if (prev == NULL)
				list = list->next;
			else
				prev->next = elem->next;
			elem->next = NULL;
			if (disk_path != devname)
				free(disk_path);
			free_sys_dev(&list);
			return elem;
		}
	}
	if (disk_path != devname)
		free(disk_path);
	free_sys_dev(&list);

	return NULL;
}


static int find_intel_hba_capability(int fd, struct intel_super *super,
				     char *devname);

static struct supertype *match_metadata_desc_imsm(char *arg)
{
	struct supertype *st;

	if (strcmp(arg, "imsm") != 0 &&
	    strcmp(arg, "default") != 0
		)
		return NULL;

	st = malloc(sizeof(*st));
	if (!st)
		return NULL;
	memset(st, 0, sizeof(*st));
	st->container_dev = NoMdDev;
	st->ss = &super_imsm;
	st->max_devs = IMSM_MAX_DEVICES;
	st->minor_version = 0;
	st->sb = NULL;
	return st;
}

#ifndef MDASSEMBLE
static __u8 *get_imsm_version(struct imsm_super *mpb)
{
	return &mpb->sig[MPB_SIG_LEN];
}
#endif

/* retrieve a disk directly from the anchor when the anchor is known to be
 * up-to-date, currently only at load time
 */
static struct imsm_disk *__get_imsm_disk(struct imsm_super *mpb, __u8 index)
{
	if (index >= mpb->num_disks)
		return NULL;
	return &mpb->disk[index];
}

/* retrieve the disk description based on a index of the disk
 * in the sub-array
 */
static struct dl *get_imsm_dl_disk(struct intel_super *super, __u8 index)
{
	struct dl *d;

	for (d = super->disks; d; d = d->next)
		if (d->index == index)
			return d;

	return NULL;
}
/* retrieve a disk from the parsed metadata */
static struct imsm_disk *get_imsm_disk(struct intel_super *super, __u8 index)
{
	struct dl *dl;

	dl = get_imsm_dl_disk(super, index);
	if (dl)
		return &dl->disk;

	return NULL;
}

/* generate a checksum directly from the anchor when the anchor is known to be
 * up-to-date, currently only at load or write_super after coalescing
 */
static __u32 __gen_imsm_checksum(struct imsm_super *mpb)
{
	__u32 end = mpb->mpb_size / sizeof(end);
	__u32 *p = (__u32 *) mpb;
	__u32 sum = 0;

        while (end--) {
                sum += __le32_to_cpu(*p);
		p++;
	}

        return sum - __le32_to_cpu(mpb->check_sum);
}

static size_t sizeof_imsm_map(struct imsm_map *map)
{
	return sizeof(struct imsm_map) + sizeof(__u32) * (map->num_members - 1);
}

struct imsm_map *get_imsm_map(struct imsm_dev *dev, int second_map)
{
	/* A device can have 2 maps if it is in the middle of a migration.
	 * If second_map is:
	 *    0   - we return the first map
	 *    1   - we return the second map if it exists, else NULL
	 *   -1   - we return the second map if it exists, else the first
	 */
	struct imsm_map *map = &dev->vol.map[0];

	if (second_map == 1 && !dev->vol.migr_state)
		return NULL;
	else if (second_map == 1 ||
		 (second_map < 0 && dev->vol.migr_state)) {
		void *ptr = map;

		return ptr + sizeof_imsm_map(map);
	} else
		return map;

}

/* return the size of the device.
 * migr_state increases the returned size if map[0] were to be duplicated
 */
static size_t sizeof_imsm_dev(struct imsm_dev *dev, int migr_state)
{
	size_t size = sizeof(*dev) - sizeof(struct imsm_map) +
		      sizeof_imsm_map(get_imsm_map(dev, 0));

	/* migrating means an additional map */
	if (dev->vol.migr_state)
		size += sizeof_imsm_map(get_imsm_map(dev, 1));
	else if (migr_state)
		size += sizeof_imsm_map(get_imsm_map(dev, 0));

	return size;
}

#ifndef MDASSEMBLE
/* retrieve disk serial number list from a metadata update */
static struct disk_info *get_disk_info(struct imsm_update_create_array *update)
{
	void *u = update;
	struct disk_info *inf;

	inf = u + sizeof(*update) - sizeof(struct imsm_dev) +
	      sizeof_imsm_dev(&update->dev, 0);

	return inf;
}
#endif

static struct imsm_dev *__get_imsm_dev(struct imsm_super *mpb, __u8 index)
{
	int offset;
	int i;
	void *_mpb = mpb;

	if (index >= mpb->num_raid_devs)
		return NULL;

	/* devices start after all disks */
	offset = ((void *) &mpb->disk[mpb->num_disks]) - _mpb;

	for (i = 0; i <= index; i++)
		if (i == index)
			return _mpb + offset;
		else
			offset += sizeof_imsm_dev(_mpb + offset, 0);

	return NULL;
}

static struct imsm_dev *get_imsm_dev(struct intel_super *super, __u8 index)
{
	struct intel_dev *dv;

	if (index >= super->anchor->num_raid_devs)
		return NULL;
	for (dv = super->devlist; dv; dv = dv->next)
		if (dv->index == index)
			return dv->dev;
	return NULL;
}

/*
 * for second_map:
 *  == 0 get first map
 *  == 1 get second map
 *  == -1 than get map according to the current migr_state
 */
static __u32 get_imsm_ord_tbl_ent(struct imsm_dev *dev,
				  int slot,
				  int second_map)
{
	struct imsm_map *map;

	map = get_imsm_map(dev, second_map);

	/* top byte identifies disk under rebuild */
	return __le32_to_cpu(map->disk_ord_tbl[slot]);
}

#define ord_to_idx(ord) (((ord) << 8) >> 8)
static __u32 get_imsm_disk_idx(struct imsm_dev *dev, int slot, int second_map)
{
	__u32 ord = get_imsm_ord_tbl_ent(dev, slot, second_map);

	return ord_to_idx(ord);
}

static void set_imsm_ord_tbl_ent(struct imsm_map *map, int slot, __u32 ord)
{
	map->disk_ord_tbl[slot] = __cpu_to_le32(ord);
}

static int get_imsm_disk_slot(struct imsm_map *map, unsigned idx)
{
	int slot;
	__u32 ord;

	for (slot = 0; slot < map->num_members; slot++) {
		ord = __le32_to_cpu(map->disk_ord_tbl[slot]);
		if (ord_to_idx(ord) == idx)
			return slot;
	}

	return -1;
}

static int get_imsm_raid_level(struct imsm_map *map)
{
	if (map->raid_level == 1) {
		if (map->num_members == 2)
			return 1;
		else
			return 10;
	}

	return map->raid_level;
}

static int cmp_extent(const void *av, const void *bv)
{
	const struct extent *a = av;
	const struct extent *b = bv;
	if (a->start < b->start)
		return -1;
	if (a->start > b->start)
		return 1;
	return 0;
}

static int count_memberships(struct dl *dl, struct intel_super *super)
{
	int memberships = 0;
	int i;

	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);
		struct imsm_map *map = get_imsm_map(dev, 0);

		if (get_imsm_disk_slot(map, dl->index) >= 0)
			memberships++;
	}

	return memberships;
}

static struct extent *get_extents(struct intel_super *super, struct dl *dl)
{
	/* find a list of used extents on the given physical device */
	struct extent *rv, *e;
	int i;
	int memberships = count_memberships(dl, super);
	__u32 reservation = MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS;

	rv = malloc(sizeof(struct extent) * (memberships + 1));
	if (!rv)
		return NULL;
	e = rv;

	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);
		struct imsm_map *map = get_imsm_map(dev, 0);

		if (get_imsm_disk_slot(map, dl->index) >= 0) {
			e->start = __le32_to_cpu(map->pba_of_lba0);
			e->size = __le32_to_cpu(map->blocks_per_member);
			e++;
		}
	}
	qsort(rv, memberships, sizeof(*rv), cmp_extent);

	/* determine the start of the metadata 
	 * when no raid devices are defined use the default
	 * ...otherwise allow the metadata to truncate the value
	 * as is the case with older versions of imsm
	 */
	if (memberships) {
		struct extent *last = &rv[memberships - 1];
		__u32 remainder;

		remainder = __le32_to_cpu(dl->disk.total_blocks) - 
			    (last->start + last->size);
		/* round down to 1k block to satisfy precision of the kernel
		 * 'size' interface
		 */
		remainder &= ~1UL;
		/* make sure remainder is still sane */
		if (remainder < (unsigned)ROUND_UP(super->len, 512) >> 9)
			remainder = ROUND_UP(super->len, 512) >> 9;
		if (reservation > remainder)
			reservation = remainder;
	}
	e->start = __le32_to_cpu(dl->disk.total_blocks) - reservation;
	e->size = 0;
	return rv;
}

/* try to determine how much space is reserved for metadata from
 * the last get_extents() entry, otherwise fallback to the
 * default
 */
static __u32 imsm_reserved_sectors(struct intel_super *super, struct dl *dl)
{
	struct extent *e;
	int i;
	__u32 rv;

	/* for spares just return a minimal reservation which will grow
	 * once the spare is picked up by an array
	 */
	if (dl->index == -1)
		return MPB_SECTOR_CNT;

	e = get_extents(super, dl);
	if (!e)
		return MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS;

	/* scroll to last entry */
	for (i = 0; e[i].size; i++)
		continue;

	rv = __le32_to_cpu(dl->disk.total_blocks) - e[i].start;

	free(e);

	return rv;
}

static int is_spare(struct imsm_disk *disk)
{
	return (disk->status & SPARE_DISK) == SPARE_DISK;
}

static int is_configured(struct imsm_disk *disk)
{
	return (disk->status & CONFIGURED_DISK) == CONFIGURED_DISK;
}

static int is_failed(struct imsm_disk *disk)
{
	return (disk->status & FAILED_DISK) == FAILED_DISK;
}

/* Return minimum size of a spare that can be used in this array*/
static unsigned long long min_acceptable_spare_size_imsm(struct supertype *st)
{
	struct intel_super *super = st->sb;
	struct dl *dl;
	struct extent *e;
	int i;
	unsigned long long rv = 0;

	if (!super)
		return rv;
	/* find first active disk in array */
	dl = super->disks;
	while (dl && (is_failed(&dl->disk) || dl->index == -1))
		dl = dl->next;
	if (!dl)
		return rv;
	/* find last lba used by subarrays */
	e = get_extents(super, dl);
	if (!e)
		return rv;
	for (i = 0; e[i].size; i++)
		continue;
	if (i > 0)
		rv = e[i-1].start + e[i-1].size;
	free(e);
	/* add the amount of space needed for metadata */
	rv = rv + MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS;
	return rv * 512;
}

#ifndef MDASSEMBLE
static __u64 blocks_per_migr_unit(struct intel_super *super,
				  struct imsm_dev *dev);

static void print_imsm_dev(struct intel_super *super,
			   struct imsm_dev *dev,
			   char *uuid,
			   int disk_idx)
{
	__u64 sz;
	int slot, i;
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct imsm_map *map2 = get_imsm_map(dev, 1);
	__u32 ord;

	printf("\n");
	printf("[%.16s]:\n", dev->volume);
	printf("           UUID : %s\n", uuid);
	printf("     RAID Level : %d", get_imsm_raid_level(map));
	if (map2)
		printf(" <-- %d", get_imsm_raid_level(map2));
	printf("\n");
	printf("        Members : %d", map->num_members);
	if (map2)
		printf(" <-- %d", map2->num_members);
	printf("\n");
	printf("          Slots : [");
	for (i = 0; i < map->num_members; i++) {
		ord = get_imsm_ord_tbl_ent(dev, i, 0);
		printf("%s", ord & IMSM_ORD_REBUILD ? "_" : "U");
	}
	printf("]");
	if (map2) {
		printf(" <-- [");
		for (i = 0; i < map2->num_members; i++) {
			ord = get_imsm_ord_tbl_ent(dev, i, 1);
			printf("%s", ord & IMSM_ORD_REBUILD ? "_" : "U");
		}
		printf("]");
	}
	printf("\n");
	printf("    Failed disk : ");
	if (map->failed_disk_num == 0xff)
		printf("none");
	else
		printf("%i", map->failed_disk_num);
	printf("\n");
	slot = get_imsm_disk_slot(map, disk_idx);
	if (slot >= 0) {
		ord = get_imsm_ord_tbl_ent(dev, slot, -1);
		printf("      This Slot : %d%s\n", slot,
		       ord & IMSM_ORD_REBUILD ? " (out-of-sync)" : "");
	} else
		printf("      This Slot : ?\n");
	sz = __le32_to_cpu(dev->size_high);
	sz <<= 32;
	sz += __le32_to_cpu(dev->size_low);
	printf("     Array Size : %llu%s\n", (unsigned long long)sz,
	       human_size(sz * 512));
	sz = __le32_to_cpu(map->blocks_per_member);
	printf("   Per Dev Size : %llu%s\n", (unsigned long long)sz,
	       human_size(sz * 512));
	printf("  Sector Offset : %u\n",
		__le32_to_cpu(map->pba_of_lba0));
	printf("    Num Stripes : %u\n",
		__le32_to_cpu(map->num_data_stripes));
	printf("     Chunk Size : %u KiB",
		__le16_to_cpu(map->blocks_per_strip) / 2);
	if (map2)
		printf(" <-- %u KiB",
			__le16_to_cpu(map2->blocks_per_strip) / 2);
	printf("\n");
	printf("       Reserved : %d\n", __le32_to_cpu(dev->reserved_blocks));
	printf("  Migrate State : ");
	if (dev->vol.migr_state) {
		if (migr_type(dev) == MIGR_INIT)
			printf("initialize\n");
		else if (migr_type(dev) == MIGR_REBUILD)
			printf("rebuild\n");
		else if (migr_type(dev) == MIGR_VERIFY)
			printf("check\n");
		else if (migr_type(dev) == MIGR_GEN_MIGR)
			printf("general migration\n");
		else if (migr_type(dev) == MIGR_STATE_CHANGE)
			printf("state change\n");
		else if (migr_type(dev) == MIGR_REPAIR)
			printf("repair\n");
		else
			printf("<unknown:%d>\n", migr_type(dev));
	} else
		printf("idle\n");
	printf("      Map State : %s", map_state_str[map->map_state]);
	if (dev->vol.migr_state) {
		struct imsm_map *map = get_imsm_map(dev, 1);

		printf(" <-- %s", map_state_str[map->map_state]);
		printf("\n     Checkpoint : %u (%llu)",
		       __le32_to_cpu(dev->vol.curr_migr_unit),
		       (unsigned long long)blocks_per_migr_unit(super, dev));
	}
	printf("\n");
	printf("    Dirty State : %s\n", dev->vol.dirty ? "dirty" : "clean");
}

static void print_imsm_disk(struct imsm_super *mpb, int index, __u32 reserved)
{
	struct imsm_disk *disk = __get_imsm_disk(mpb, index);
	char str[MAX_RAID_SERIAL_LEN + 1];
	__u64 sz;

	if (index < 0 || !disk)
		return;

	printf("\n");
	snprintf(str, MAX_RAID_SERIAL_LEN + 1, "%s", disk->serial);
	printf("  Disk%02d Serial : %s\n", index, str);
	printf("          State :%s%s%s\n", is_spare(disk) ? " spare" : "",
					    is_configured(disk) ? " active" : "",
					    is_failed(disk) ? " failed" : "");
	printf("             Id : %08x\n", __le32_to_cpu(disk->scsi_id));
	sz = __le32_to_cpu(disk->total_blocks) - reserved;
	printf("    Usable Size : %llu%s\n", (unsigned long long)sz,
	       human_size(sz * 512));
}

static int is_gen_migration(struct imsm_dev *dev);

void examine_migr_rec_imsm(struct intel_super *super)
{
	struct migr_record *migr_rec = super->migr_rec;
	struct imsm_super *mpb = super->anchor;
	int i;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev = __get_imsm_dev(mpb, i);
		if (is_gen_migration(dev) == 0)
				continue;

		printf("\nMigration Record Information:");
		if (super->disks->index > 1) {
			printf(" Empty\n                              ");
			printf("Examine one of first two disks in array\n");
			break;
		}
		printf("\n                     Status : ");
		if (__le32_to_cpu(migr_rec->rec_status) == UNIT_SRC_NORMAL)
			printf("Normal\n");
		else
			printf("Contains Data\n");
		printf("               Current Unit : %u\n",
		       __le32_to_cpu(migr_rec->curr_migr_unit));
		printf("                     Family : %u\n",
		       __le32_to_cpu(migr_rec->family_num));
		printf("                  Ascending : %u\n",
		       __le32_to_cpu(migr_rec->ascending_migr));
		printf("            Blocks Per Unit : %u\n",
		       __le32_to_cpu(migr_rec->blocks_per_unit));
		printf("       Dest. Depth Per Unit : %u\n",
		       __le32_to_cpu(migr_rec->dest_depth_per_unit));
		printf("        Checkpoint Area pba : %u\n",
		       __le32_to_cpu(migr_rec->ckpt_area_pba));
		printf("           First member lba : %u\n",
		       __le32_to_cpu(migr_rec->dest_1st_member_lba));
		printf("      Total Number of Units : %u\n",
		       __le32_to_cpu(migr_rec->num_migr_units));
		printf("             Size of volume : %u\n",
		       __le32_to_cpu(migr_rec->post_migr_vol_cap));
		printf("  Expansion space for LBA64 : %u\n",
		       __le32_to_cpu(migr_rec->post_migr_vol_cap_hi));
		printf("       Record was read from : %u\n",
		       __le32_to_cpu(migr_rec->ckpt_read_disk_num));

		break;
	}
}
#endif /* MDASSEMBLE */
/*******************************************************************************
 * function: imsm_check_attributes
 * Description: Function checks if features represented by attributes flags
 * 		are supported by mdadm.
 * Parameters:
 *		attributes - Attributes read from metadata
 * Returns:
 * 		0 - passed attributes contains unsupported features flags
 * 		1 - all features are supported
 ******************************************************************************/
static int imsm_check_attributes(__u32 attributes)
{
	int ret_val = 1;
	__u32 not_supported = MPB_ATTRIB_SUPPORTED^0xffffffff;

	not_supported &= ~MPB_ATTRIB_IGNORED;

	not_supported &= attributes;
	if (not_supported) {
		fprintf(stderr, Name "(IMSM): Unsupported attributes : %x\n",
			(unsigned)__le32_to_cpu(not_supported));
		if (not_supported & MPB_ATTRIB_CHECKSUM_VERIFY) {
			dprintf("\t\tMPB_ATTRIB_CHECKSUM_VERIFY \n");
			not_supported ^= MPB_ATTRIB_CHECKSUM_VERIFY;
		}
		if (not_supported & MPB_ATTRIB_2TB) {
			dprintf("\t\tMPB_ATTRIB_2TB\n");
			not_supported ^= MPB_ATTRIB_2TB;
		}
		if (not_supported & MPB_ATTRIB_RAID0) {
			dprintf("\t\tMPB_ATTRIB_RAID0\n");
			not_supported ^= MPB_ATTRIB_RAID0;
		}
		if (not_supported & MPB_ATTRIB_RAID1) {
			dprintf("\t\tMPB_ATTRIB_RAID1\n");
			not_supported ^= MPB_ATTRIB_RAID1;
		}
		if (not_supported & MPB_ATTRIB_RAID10) {
			dprintf("\t\tMPB_ATTRIB_RAID10\n");
			not_supported ^= MPB_ATTRIB_RAID10;
		}
		if (not_supported & MPB_ATTRIB_RAID1E) {
			dprintf("\t\tMPB_ATTRIB_RAID1E\n");
			not_supported ^= MPB_ATTRIB_RAID1E;
		}
		if (not_supported & MPB_ATTRIB_RAID5) {
		dprintf("\t\tMPB_ATTRIB_RAID5\n");
			not_supported ^= MPB_ATTRIB_RAID5;
		}
		if (not_supported & MPB_ATTRIB_RAIDCNG) {
			dprintf("\t\tMPB_ATTRIB_RAIDCNG\n");
			not_supported ^= MPB_ATTRIB_RAIDCNG;
		}
		if (not_supported & MPB_ATTRIB_BBM) {
			dprintf("\t\tMPB_ATTRIB_BBM\n");
		not_supported ^= MPB_ATTRIB_BBM;
		}
		if (not_supported & MPB_ATTRIB_CHECKSUM_VERIFY) {
			dprintf("\t\tMPB_ATTRIB_CHECKSUM_VERIFY (== MPB_ATTRIB_LEGACY)\n");
			not_supported ^= MPB_ATTRIB_CHECKSUM_VERIFY;
		}
		if (not_supported & MPB_ATTRIB_EXP_STRIPE_SIZE) {
			dprintf("\t\tMPB_ATTRIB_EXP_STRIP_SIZE\n");
			not_supported ^= MPB_ATTRIB_EXP_STRIPE_SIZE;
		}
		if (not_supported & MPB_ATTRIB_2TB_DISK) {
			dprintf("\t\tMPB_ATTRIB_2TB_DISK\n");
			not_supported ^= MPB_ATTRIB_2TB_DISK;
		}
		if (not_supported & MPB_ATTRIB_NEVER_USE2) {
			dprintf("\t\tMPB_ATTRIB_NEVER_USE2\n");
			not_supported ^= MPB_ATTRIB_NEVER_USE2;
		}
		if (not_supported & MPB_ATTRIB_NEVER_USE) {
			dprintf("\t\tMPB_ATTRIB_NEVER_USE\n");
			not_supported ^= MPB_ATTRIB_NEVER_USE;
		}

		if (not_supported)
			dprintf(Name "(IMSM): Unknown attributes : %x\n", not_supported);

		ret_val = 0;
	}

	return ret_val;
}

#ifndef MDASSEMBLE
static void getinfo_super_imsm(struct supertype *st, struct mdinfo *info, char *map);

static void examine_super_imsm(struct supertype *st, char *homehost)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	char str[MAX_SIGNATURE_LENGTH];
	int i;
	struct mdinfo info;
	char nbuf[64];
	__u32 sum;
	__u32 reserved = imsm_reserved_sectors(super, super->disks);
	struct dl *dl;

	snprintf(str, MPB_SIG_LEN, "%s", mpb->sig);
	printf("          Magic : %s\n", str);
	snprintf(str, strlen(MPB_VERSION_RAID0), "%s", get_imsm_version(mpb));
	printf("        Version : %s\n", get_imsm_version(mpb));
	printf("    Orig Family : %08x\n", __le32_to_cpu(mpb->orig_family_num));
	printf("         Family : %08x\n", __le32_to_cpu(mpb->family_num));
	printf("     Generation : %08x\n", __le32_to_cpu(mpb->generation_num));
	printf("     Attributes : ");
	if (imsm_check_attributes(mpb->attributes))
		printf("All supported\n");
	else
		printf("not supported\n");
	getinfo_super_imsm(st, &info, NULL);
	fname_from_uuid(st, &info, nbuf, ':');
	printf("           UUID : %s\n", nbuf + 5);
	sum = __le32_to_cpu(mpb->check_sum);
	printf("       Checksum : %08x %s\n", sum,
		__gen_imsm_checksum(mpb) == sum ? "correct" : "incorrect");
	printf("    MPB Sectors : %d\n", mpb_sectors(mpb));
	printf("          Disks : %d\n", mpb->num_disks);
	printf("   RAID Devices : %d\n", mpb->num_raid_devs);
	print_imsm_disk(mpb, super->disks->index, reserved);
	if (super->bbm_log) {
		struct bbm_log *log = super->bbm_log;

		printf("\n");
		printf("Bad Block Management Log:\n");
		printf("       Log Size : %d\n", __le32_to_cpu(mpb->bbm_log_size));
		printf("      Signature : %x\n", __le32_to_cpu(log->signature));
		printf("    Entry Count : %d\n", __le32_to_cpu(log->entry_count));
		printf("   Spare Blocks : %d\n",  __le32_to_cpu(log->reserved_spare_block_count));
		printf("    First Spare : %llx\n",
		       (unsigned long long) __le64_to_cpu(log->first_spare_lba));
	}
	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct mdinfo info;
		struct imsm_dev *dev = __get_imsm_dev(mpb, i);

		super->current_vol = i;
		getinfo_super_imsm(st, &info, NULL);
		fname_from_uuid(st, &info, nbuf, ':');
		print_imsm_dev(super, dev, nbuf + 5, super->disks->index);
	}
	for (i = 0; i < mpb->num_disks; i++) {
		if (i == super->disks->index)
			continue;
		print_imsm_disk(mpb, i, reserved);
	}
	for (dl = super->disks ; dl; dl = dl->next) {
		struct imsm_disk *disk;
		char str[MAX_RAID_SERIAL_LEN + 1];
		__u64 sz;

		if (dl->index >= 0)
			continue;

		disk = &dl->disk;
		printf("\n");
		snprintf(str, MAX_RAID_SERIAL_LEN + 1, "%s", disk->serial);
		printf("    Disk Serial : %s\n", str);
		printf("          State :%s%s%s\n", is_spare(disk) ? " spare" : "",
		       is_configured(disk) ? " active" : "",
		       is_failed(disk) ? " failed" : "");
		printf("             Id : %08x\n", __le32_to_cpu(disk->scsi_id));
		sz = __le32_to_cpu(disk->total_blocks) - reserved;
		printf("    Usable Size : %llu%s\n", (unsigned long long)sz,
		       human_size(sz * 512));
	}

	examine_migr_rec_imsm(super);
}

static void brief_examine_super_imsm(struct supertype *st, int verbose)
{
	/* We just write a generic IMSM ARRAY entry */
	struct mdinfo info;
	char nbuf[64];
	struct intel_super *super = st->sb;

	if (!super->anchor->num_raid_devs) {
		printf("ARRAY metadata=imsm\n");
		return;
	}

	getinfo_super_imsm(st, &info, NULL);
	fname_from_uuid(st, &info, nbuf, ':');
	printf("ARRAY metadata=imsm UUID=%s\n", nbuf + 5);
}

static void brief_examine_subarrays_imsm(struct supertype *st, int verbose)
{
	/* We just write a generic IMSM ARRAY entry */
	struct mdinfo info;
	char nbuf[64];
	char nbuf1[64];
	struct intel_super *super = st->sb;
	int i;

	if (!super->anchor->num_raid_devs)
		return;

	getinfo_super_imsm(st, &info, NULL);
	fname_from_uuid(st, &info, nbuf, ':');
	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);

		super->current_vol = i;
		getinfo_super_imsm(st, &info, NULL);
		fname_from_uuid(st, &info, nbuf1, ':');
		printf("ARRAY /dev/md/%.16s container=%s member=%d UUID=%s\n",
		       dev->volume, nbuf + 5, i, nbuf1 + 5);
	}
}

static void export_examine_super_imsm(struct supertype *st)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct mdinfo info;
	char nbuf[64];

	getinfo_super_imsm(st, &info, NULL);
	fname_from_uuid(st, &info, nbuf, ':');
	printf("MD_METADATA=imsm\n");
	printf("MD_LEVEL=container\n");
	printf("MD_UUID=%s\n", nbuf+5);
	printf("MD_DEVICES=%u\n", mpb->num_disks);
}

static void detail_super_imsm(struct supertype *st, char *homehost)
{
	struct mdinfo info;
	char nbuf[64];

	getinfo_super_imsm(st, &info, NULL);
	fname_from_uuid(st, &info, nbuf, ':');
	printf("\n           UUID : %s\n", nbuf + 5);
}

static void brief_detail_super_imsm(struct supertype *st)
{
	struct mdinfo info;
	char nbuf[64];
	getinfo_super_imsm(st, &info, NULL);
	fname_from_uuid(st, &info, nbuf, ':');
	printf(" UUID=%s", nbuf + 5);
}

static int imsm_read_serial(int fd, char *devname, __u8 *serial);
static void fd2devname(int fd, char *name);

static int ahci_enumerate_ports(const char *hba_path, int port_count, int host_base, int verbose)
{
	/* dump an unsorted list of devices attached to AHCI Intel storage
	 * controller, as well as non-connected ports
	 */
	int hba_len = strlen(hba_path) + 1;
	struct dirent *ent;
	DIR *dir;
	char *path = NULL;
	int err = 0;
	unsigned long port_mask = (1 << port_count) - 1;

	if (port_count > (int)sizeof(port_mask) * 8) {
		if (verbose)
			fprintf(stderr, Name ": port_count %d out of range\n", port_count);
		return 2;
	}

	/* scroll through /sys/dev/block looking for devices attached to
	 * this hba
	 */
	dir = opendir("/sys/dev/block");
	for (ent = dir ? readdir(dir) : NULL; ent; ent = readdir(dir)) {
		int fd;
		char model[64];
		char vendor[64];
		char buf[1024];
		int major, minor;
		char *device;
		char *c;
		int port;
		int type;

		if (sscanf(ent->d_name, "%d:%d", &major, &minor) != 2)
			continue;
		path = devt_to_devpath(makedev(major, minor));
		if (!path)
			continue;
		if (!path_attached_to_hba(path, hba_path)) {
			free(path);
			path = NULL;
			continue;
		}

		/* retrieve the scsi device type */
		if (asprintf(&device, "/sys/dev/block/%d:%d/device/xxxxxxx", major, minor) < 0) {
			if (verbose)
				fprintf(stderr, Name ": failed to allocate 'device'\n");
			err = 2;
			break;
		}
		sprintf(device, "/sys/dev/block/%d:%d/device/type", major, minor);
		if (load_sys(device, buf) != 0) {
			if (verbose)
				fprintf(stderr, Name ": failed to read device type for %s\n",
					path);
			err = 2;
			free(device);
			break;
		}
		type = strtoul(buf, NULL, 10);

		/* if it's not a disk print the vendor and model */
		if (!(type == 0 || type == 7 || type == 14)) {
			vendor[0] = '\0';
			model[0] = '\0';
			sprintf(device, "/sys/dev/block/%d:%d/device/vendor", major, minor);
			if (load_sys(device, buf) == 0) {
				strncpy(vendor, buf, sizeof(vendor));
				vendor[sizeof(vendor) - 1] = '\0';
				c = (char *) &vendor[sizeof(vendor) - 1];
				while (isspace(*c) || *c == '\0')
					*c-- = '\0';

			}
			sprintf(device, "/sys/dev/block/%d:%d/device/model", major, minor);
			if (load_sys(device, buf) == 0) {
				strncpy(model, buf, sizeof(model));
				model[sizeof(model) - 1] = '\0';
				c = (char *) &model[sizeof(model) - 1];
				while (isspace(*c) || *c == '\0')
					*c-- = '\0';
			}

			if (vendor[0] && model[0])
				sprintf(buf, "%.64s %.64s", vendor, model);
			else
				switch (type) { /* numbers from hald/linux/device.c */
				case 1: sprintf(buf, "tape"); break;
				case 2: sprintf(buf, "printer"); break;
				case 3: sprintf(buf, "processor"); break;
				case 4:
				case 5: sprintf(buf, "cdrom"); break;
				case 6: sprintf(buf, "scanner"); break;
				case 8: sprintf(buf, "media_changer"); break;
				case 9: sprintf(buf, "comm"); break;
				case 12: sprintf(buf, "raid"); break;
				default: sprintf(buf, "unknown");
				}
		} else
			buf[0] = '\0';
		free(device);

		/* chop device path to 'host%d' and calculate the port number */
		c = strchr(&path[hba_len], '/');
		if (!c) {
			if (verbose)
				fprintf(stderr, Name ": %s - invalid path name\n", path + hba_len);
			err = 2;
			break;
		}
		*c = '\0';
		if (sscanf(&path[hba_len], "host%d", &port) == 1)
			port -= host_base;
		else {
			if (verbose) {
				*c = '/'; /* repair the full string */
				fprintf(stderr, Name ": failed to determine port number for %s\n",
					path);
			}
			err = 2;
			break;
		}

		/* mark this port as used */
		port_mask &= ~(1 << port);

		/* print out the device information */
		if (buf[0]) {
			printf("          Port%d : - non-disk device (%s) -\n", port, buf);
			continue;
		}

		fd = dev_open(ent->d_name, O_RDONLY);
		if (fd < 0)
			printf("          Port%d : - disk info unavailable -\n", port);
		else {
			fd2devname(fd, buf);
			printf("          Port%d : %s", port, buf);
			if (imsm_read_serial(fd, NULL, (__u8 *) buf) == 0)
				printf(" (%.*s)\n", MAX_RAID_SERIAL_LEN, buf);
			else
				printf(" ()\n");
		}
		close(fd);
		free(path);
		path = NULL;
	}
	if (path)
		free(path);
	if (dir)
		closedir(dir);
	if (err == 0) {
		int i;

		for (i = 0; i < port_count; i++)
			if (port_mask & (1 << i))
				printf("          Port%d : - no device attached -\n", i);
	}

	return err;
}

static void print_found_intel_controllers(struct sys_dev *elem)
{
	for (; elem; elem = elem->next) {
		fprintf(stderr, Name ": found Intel(R) ");
		if (elem->type == SYS_DEV_SATA)
			fprintf(stderr, "SATA ");
		else if (elem->type == SYS_DEV_SAS)
			fprintf(stderr, "SAS ");
		fprintf(stderr, "RAID controller");
		if (elem->pci_id)
			fprintf(stderr, " at %s", elem->pci_id);
		fprintf(stderr, ".\n");
	}
	fflush(stderr);
}

static int ahci_get_port_count(const char *hba_path, int *port_count)
{
	struct dirent *ent;
	DIR *dir;
	int host_base = -1;

	*port_count = 0;
	if ((dir = opendir(hba_path)) == NULL)
		return -1;

	for (ent = readdir(dir); ent; ent = readdir(dir)) {
		int host;

		if (sscanf(ent->d_name, "host%d", &host) != 1)
			continue;
		if (*port_count == 0)
			host_base = host;
		else if (host < host_base)
			host_base = host;

		if (host + 1 > *port_count + host_base)
			*port_count = host + 1 - host_base;
	}
	closedir(dir);
	return host_base;
}

static void print_imsm_capability(const struct imsm_orom *orom)
{
	printf("       Platform : Intel(R) Matrix Storage Manager\n");
	printf("        Version : %d.%d.%d.%d\n", orom->major_ver, orom->minor_ver,
	       orom->hotfix_ver, orom->build);
	printf("    RAID Levels :%s%s%s%s%s\n",
	       imsm_orom_has_raid0(orom) ? " raid0" : "",
	       imsm_orom_has_raid1(orom) ? " raid1" : "",
	       imsm_orom_has_raid1e(orom) ? " raid1e" : "",
	       imsm_orom_has_raid10(orom) ? " raid10" : "",
	       imsm_orom_has_raid5(orom) ? " raid5" : "");
	printf("    Chunk Sizes :%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
	       imsm_orom_has_chunk(orom, 2) ? " 2k" : "",
	       imsm_orom_has_chunk(orom, 4) ? " 4k" : "",
	       imsm_orom_has_chunk(orom, 8) ? " 8k" : "",
	       imsm_orom_has_chunk(orom, 16) ? " 16k" : "",
	       imsm_orom_has_chunk(orom, 32) ? " 32k" : "",
	       imsm_orom_has_chunk(orom, 64) ? " 64k" : "",
	       imsm_orom_has_chunk(orom, 128) ? " 128k" : "",
	       imsm_orom_has_chunk(orom, 256) ? " 256k" : "",
	       imsm_orom_has_chunk(orom, 512) ? " 512k" : "",
	       imsm_orom_has_chunk(orom, 1024*1) ? " 1M" : "",
	       imsm_orom_has_chunk(orom, 1024*2) ? " 2M" : "",
	       imsm_orom_has_chunk(orom, 1024*4) ? " 4M" : "",
	       imsm_orom_has_chunk(orom, 1024*8) ? " 8M" : "",
	       imsm_orom_has_chunk(orom, 1024*16) ? " 16M" : "",
	       imsm_orom_has_chunk(orom, 1024*32) ? " 32M" : "",
	       imsm_orom_has_chunk(orom, 1024*64) ? " 64M" : "");
	printf("      Max Disks : %d\n", orom->tds);
	printf("    Max Volumes : %d\n", orom->vpa);
	return;
}

static int detail_platform_imsm(int verbose, int enumerate_only)
{
	/* There are two components to imsm platform support, the ahci SATA
	 * controller and the option-rom.  To find the SATA controller we
	 * simply look in /sys/bus/pci/drivers/ahci to see if an ahci
	 * controller with the Intel vendor id is present.  This approach
	 * allows mdadm to leverage the kernel's ahci detection logic, with the
	 * caveat that if ahci.ko is not loaded mdadm will not be able to
	 * detect platform raid capabilities.  The option-rom resides in a
	 * platform "Adapter ROM".  We scan for its signature to retrieve the
	 * platform capabilities.  If raid support is disabled in the BIOS the
	 * option-rom capability structure will not be available.
	 */
	const struct imsm_orom *orom;
	struct sys_dev *list, *hba;
	int host_base = 0;
	int port_count = 0;
	int result=0;

	if (enumerate_only) {
		if (check_env("IMSM_NO_PLATFORM"))
			return 0;
		list = find_intel_devices();
		if (!list)
			return 2;
		for (hba = list; hba; hba = hba->next) {
			orom = find_imsm_capability(hba->type);
			if (!orom) {
				result = 2;
				break;
			}
		}
		free_sys_dev(&list);
		return result;
	}

	list = find_intel_devices();
	if (!list) {
		if (verbose)
			fprintf(stderr, Name ": no active Intel(R) RAID "
				"controller found.\n");
		free_sys_dev(&list);
		return 2;
	} else if (verbose)
		print_found_intel_controllers(list);

	for (hba = list; hba; hba = hba->next) {
		orom = find_imsm_capability(hba->type);
		if (!orom)
			fprintf(stderr, Name ": imsm capabilities not found for controller: %s (type %s)\n",
				hba->path, get_sys_dev_type(hba->type));
		else
			print_imsm_capability(orom);
	}

	for (hba = list; hba; hba = hba->next) {
		printf(" I/O Controller : %s (%s)\n",
			hba->path, get_sys_dev_type(hba->type));

		if (hba->type == SYS_DEV_SATA) {
			host_base = ahci_get_port_count(hba->path, &port_count);
			if (ahci_enumerate_ports(hba->path, port_count, host_base, verbose)) {
				if (verbose)
					fprintf(stderr, Name ": failed to enumerate "
						"ports on SATA controller at %s.", hba->pci_id);
				result |= 2;
			}
		}
	}

	free_sys_dev(&list);
	return result;
}
#endif

static int match_home_imsm(struct supertype *st, char *homehost)
{
	/* the imsm metadata format does not specify any host
	 * identification information.  We return -1 since we can never
	 * confirm nor deny whether a given array is "meant" for this
	 * host.  We rely on compare_super and the 'family_num' fields to
	 * exclude member disks that do not belong, and we rely on
	 * mdadm.conf to specify the arrays that should be assembled.
	 * Auto-assembly may still pick up "foreign" arrays.
	 */

	return -1;
}

static void uuid_from_super_imsm(struct supertype *st, int uuid[4])
{
	/* The uuid returned here is used for:
	 *  uuid to put into bitmap file (Create, Grow)
	 *  uuid for backup header when saving critical section (Grow)
	 *  comparing uuids when re-adding a device into an array
	 *    In these cases the uuid required is that of the data-array,
	 *    not the device-set.
	 *  uuid to recognise same set when adding a missing device back
	 *    to an array.   This is a uuid for the device-set.
	 *  
	 * For each of these we can make do with a truncated
	 * or hashed uuid rather than the original, as long as
	 * everyone agrees.
	 * In each case the uuid required is that of the data-array,
	 * not the device-set.
	 */
	/* imsm does not track uuid's so we synthesis one using sha1 on
	 * - The signature (Which is constant for all imsm array, but no matter)
	 * - the orig_family_num of the container
	 * - the index number of the volume
	 * - the 'serial' number of the volume.
	 * Hopefully these are all constant.
	 */
	struct intel_super *super = st->sb;

	char buf[20];
	struct sha1_ctx ctx;
	struct imsm_dev *dev = NULL;
	__u32 family_num;

	/* some mdadm versions failed to set ->orig_family_num, in which
	 * case fall back to ->family_num.  orig_family_num will be
	 * fixed up with the first metadata update.
	 */
	family_num = super->anchor->orig_family_num;
	if (family_num == 0)
		family_num = super->anchor->family_num;
	sha1_init_ctx(&ctx);
	sha1_process_bytes(super->anchor->sig, MPB_SIG_LEN, &ctx);
	sha1_process_bytes(&family_num, sizeof(__u32), &ctx);
	if (super->current_vol >= 0)
		dev = get_imsm_dev(super, super->current_vol);
	if (dev) {
		__u32 vol = super->current_vol;
		sha1_process_bytes(&vol, sizeof(vol), &ctx);
		sha1_process_bytes(dev->volume, MAX_RAID_SERIAL_LEN, &ctx);
	}
	sha1_finish_ctx(&ctx, buf);
	memcpy(uuid, buf, 4*4);
}

#if 0
static void
get_imsm_numerical_version(struct imsm_super *mpb, int *m, int *p)
{
	__u8 *v = get_imsm_version(mpb);
	__u8 *end = mpb->sig + MAX_SIGNATURE_LENGTH;
	char major[] = { 0, 0, 0 };
	char minor[] = { 0 ,0, 0 };
	char patch[] = { 0, 0, 0 };
	char *ver_parse[] = { major, minor, patch };
	int i, j;

	i = j = 0;
	while (*v != '\0' && v < end) {
		if (*v != '.' && j < 2)
			ver_parse[i][j++] = *v;
		else {
			i++;
			j = 0;
		}
		v++;
	}

	*m = strtol(minor, NULL, 0);
	*p = strtol(patch, NULL, 0);
}
#endif

static __u32 migr_strip_blocks_resync(struct imsm_dev *dev)
{
	/* migr_strip_size when repairing or initializing parity */
	struct imsm_map *map = get_imsm_map(dev, 0);
	__u32 chunk = __le32_to_cpu(map->blocks_per_strip);

	switch (get_imsm_raid_level(map)) {
	case 5:
	case 10:
		return chunk;
	default:
		return 128*1024 >> 9;
	}
}

static __u32 migr_strip_blocks_rebuild(struct imsm_dev *dev)
{
	/* migr_strip_size when rebuilding a degraded disk, no idea why
	 * this is different than migr_strip_size_resync(), but it's good
	 * to be compatible
	 */
	struct imsm_map *map = get_imsm_map(dev, 1);
	__u32 chunk = __le32_to_cpu(map->blocks_per_strip);

	switch (get_imsm_raid_level(map)) {
	case 1:
	case 10:
		if (map->num_members % map->num_domains == 0)
			return 128*1024 >> 9;
		else
			return chunk;
	case 5:
		return max((__u32) 64*1024 >> 9, chunk);
	default:
		return 128*1024 >> 9;
	}
}

static __u32 num_stripes_per_unit_resync(struct imsm_dev *dev)
{
	struct imsm_map *lo = get_imsm_map(dev, 0);
	struct imsm_map *hi = get_imsm_map(dev, 1);
	__u32 lo_chunk = __le32_to_cpu(lo->blocks_per_strip);
	__u32 hi_chunk = __le32_to_cpu(hi->blocks_per_strip);

	return max((__u32) 1, hi_chunk / lo_chunk);
}

static __u32 num_stripes_per_unit_rebuild(struct imsm_dev *dev)
{
	struct imsm_map *lo = get_imsm_map(dev, 0);
	int level = get_imsm_raid_level(lo);

	if (level == 1 || level == 10) {
		struct imsm_map *hi = get_imsm_map(dev, 1);

		return hi->num_domains;
	} else
		return num_stripes_per_unit_resync(dev);
}

static __u8 imsm_num_data_members(struct imsm_dev *dev, int second_map)
{
	/* named 'imsm_' because raid0, raid1 and raid10
	 * counter-intuitively have the same number of data disks
	 */
	struct imsm_map *map = get_imsm_map(dev, second_map);

	switch (get_imsm_raid_level(map)) {
	case 0:
	case 1:
	case 10:
		return map->num_members;
	case 5:
		return map->num_members - 1;
	default:
		dprintf("%s: unsupported raid level\n", __func__);
		return 0;
	}
}

static __u32 parity_segment_depth(struct imsm_dev *dev)
{
	struct imsm_map *map = get_imsm_map(dev, 0);
	__u32 chunk =  __le32_to_cpu(map->blocks_per_strip);

	switch(get_imsm_raid_level(map)) {
	case 1:
	case 10:
		return chunk * map->num_domains;
	case 5:
		return chunk * map->num_members;
	default:
		return chunk;
	}
}

static __u32 map_migr_block(struct imsm_dev *dev, __u32 block)
{
	struct imsm_map *map = get_imsm_map(dev, 1);
	__u32 chunk = __le32_to_cpu(map->blocks_per_strip);
	__u32 strip = block / chunk;

	switch (get_imsm_raid_level(map)) {
	case 1:
	case 10: {
		__u32 vol_strip = (strip * map->num_domains) + 1;
		__u32 vol_stripe = vol_strip / map->num_members;

		return vol_stripe * chunk + block % chunk;
	} case 5: {
		__u32 stripe = strip / (map->num_members - 1);

		return stripe * chunk + block % chunk;
	}
	default:
		return 0;
	}
}

static __u64 blocks_per_migr_unit(struct intel_super *super,
				  struct imsm_dev *dev)
{
	/* calculate the conversion factor between per member 'blocks'
	 * (md/{resync,rebuild}_start) and imsm migration units, return
	 * 0 for the 'not migrating' and 'unsupported migration' cases
	 */
	if (!dev->vol.migr_state)
		return 0;

	switch (migr_type(dev)) {
	case MIGR_GEN_MIGR: {
		struct migr_record *migr_rec = super->migr_rec;
		return __le32_to_cpu(migr_rec->blocks_per_unit);
	}
	case MIGR_VERIFY:
	case MIGR_REPAIR:
	case MIGR_INIT: {
		struct imsm_map *map = get_imsm_map(dev, 0);
		__u32 stripes_per_unit;
		__u32 blocks_per_unit;
		__u32 parity_depth;
		__u32 migr_chunk;
		__u32 block_map;
		__u32 block_rel;
		__u32 segment;
		__u32 stripe;
		__u8  disks;

		/* yes, this is really the translation of migr_units to
		 * per-member blocks in the 'resync' case
		 */
		stripes_per_unit = num_stripes_per_unit_resync(dev);
		migr_chunk = migr_strip_blocks_resync(dev);
		disks = imsm_num_data_members(dev, 0);
		blocks_per_unit = stripes_per_unit * migr_chunk * disks;
		stripe = __le16_to_cpu(map->blocks_per_strip) * disks;
		segment = blocks_per_unit / stripe;
		block_rel = blocks_per_unit - segment * stripe;
		parity_depth = parity_segment_depth(dev);
		block_map = map_migr_block(dev, block_rel);
		return block_map + parity_depth * segment;
	}
	case MIGR_REBUILD: {
		__u32 stripes_per_unit;
		__u32 migr_chunk;

		stripes_per_unit = num_stripes_per_unit_rebuild(dev);
		migr_chunk = migr_strip_blocks_rebuild(dev);
		return migr_chunk * stripes_per_unit;
	}
	case MIGR_STATE_CHANGE:
	default:
		return 0;
	}
}

static int imsm_level_to_layout(int level)
{
	switch (level) {
	case 0:
	case 1:
		return 0;
	case 5:
	case 6:
		return ALGORITHM_LEFT_ASYMMETRIC;
	case 10:
		return 0x102;
	}
	return UnSet;
}

/*******************************************************************************
 * Function:	read_imsm_migr_rec
 * Description: Function reads imsm migration record from last sector of disk
 * Parameters:
 *	fd	: disk descriptor
 *	super	: metadata info
 * Returns:
 *	 0 : success,
 *	-1 : fail
 ******************************************************************************/
static int read_imsm_migr_rec(int fd, struct intel_super *super)
{
	int ret_val = -1;
	unsigned long long dsize;

	get_dev_size(fd, NULL, &dsize);
	if (lseek64(fd, dsize - 512, SEEK_SET) < 0) {
		fprintf(stderr,
			Name ": Cannot seek to anchor block: %s\n",
			strerror(errno));
		goto out;
	}
	if (read(fd, super->migr_rec_buf, 512) != 512) {
		fprintf(stderr,
			Name ": Cannot read migr record block: %s\n",
			strerror(errno));
		goto out;
	}
	ret_val = 0;

out:
	return ret_val;
}

/*******************************************************************************
 * Function:	load_imsm_migr_rec
 * Description:	Function reads imsm migration record (it is stored at the last
 *		sector of disk)
 * Parameters:
 *	super	: imsm internal array info
 *	info	: general array info
 * Returns:
 *	 0 : success
 *	-1 : fail
 ******************************************************************************/
static int load_imsm_migr_rec(struct intel_super *super, struct mdinfo *info)
{
	struct mdinfo *sd;
	struct dl *dl = NULL;
	char nm[30];
	int retval = -1;
	int fd = -1;

	if (info) {
		for (sd = info->devs ; sd ; sd = sd->next) {
			/* read only from one of the first two slots */
			if ((sd->disk.raid_disk > 1) ||
			    (sd->disk.raid_disk < 0))
				continue;
			sprintf(nm, "%d:%d", sd->disk.major, sd->disk.minor);
			fd = dev_open(nm, O_RDONLY);
			if (fd >= 0)
				break;
		}
	}
	if (fd < 0) {
		for (dl = super->disks; dl; dl = dl->next) {
			/* read only from one of the first two slots */
			if (dl->index > 1)
				continue;
			sprintf(nm, "%d:%d", dl->major, dl->minor);
			fd = dev_open(nm, O_RDONLY);
			if (fd >= 0)
				break;
		}
	}
	if (fd < 0)
		goto out;
	retval = read_imsm_migr_rec(fd, super);

out:
	if (fd >= 0)
		close(fd);
	return retval;
}

#ifndef MDASSEMBLE
/*******************************************************************************
 * function: imsm_create_metadata_checkpoint_update
 * Description: It creates update for checkpoint change.
 * Parameters:
 *	super	: imsm internal array info
 *	u	: pointer to prepared update
 * Returns:
 *	Uptate length.
 *	If length is equal to 0, input pointer u contains no update
 ******************************************************************************/
static int imsm_create_metadata_checkpoint_update(
	struct intel_super *super,
	struct imsm_update_general_migration_checkpoint **u)
{

	int update_memory_size = 0;

	dprintf("imsm_create_metadata_checkpoint_update(enter)\n");

	if (u == NULL)
		return 0;
	*u = NULL;

	/* size of all update data without anchor */
	update_memory_size =
		sizeof(struct imsm_update_general_migration_checkpoint);

	*u = calloc(1, update_memory_size);
	if (*u == NULL) {
		dprintf("error: cannot get memory for "
			"imsm_create_metadata_checkpoint_update update\n");
		return 0;
	}
	(*u)->type = update_general_migration_checkpoint;
	(*u)->curr_migr_unit = __le32_to_cpu(super->migr_rec->curr_migr_unit);
	dprintf("imsm_create_metadata_checkpoint_update: prepared for %u\n",
		(*u)->curr_migr_unit);

	return update_memory_size;
}


static void imsm_update_metadata_locally(struct supertype *st,
					 void *buf, int len);

/*******************************************************************************
 * Function:	write_imsm_migr_rec
 * Description:	Function writes imsm migration record
 *		(at the last sector of disk)
 * Parameters:
 *	super	: imsm internal array info
 * Returns:
 *	 0 : success
 *	-1 : if fail
 ******************************************************************************/
static int write_imsm_migr_rec(struct supertype *st)
{
	struct intel_super *super = st->sb;
	unsigned long long dsize;
	char nm[30];
	int fd = -1;
	int retval = -1;
	struct dl *sd;
	int len;
	struct imsm_update_general_migration_checkpoint *u;

	for (sd = super->disks ; sd ; sd = sd->next) {
		/* write to 2 first slots only */
		if ((sd->index < 0) || (sd->index > 1))
			continue;
		sprintf(nm, "%d:%d", sd->major, sd->minor);
		fd = dev_open(nm, O_RDWR);
		if (fd < 0)
			continue;
		get_dev_size(fd, NULL, &dsize);
		if (lseek64(fd, dsize - 512, SEEK_SET) < 0) {
			fprintf(stderr,
				Name ": Cannot seek to anchor block: %s\n",
				strerror(errno));
			goto out;
		}
		if (write(fd, super->migr_rec_buf, 512) != 512) {
			fprintf(stderr,
				Name ": Cannot write migr record block: %s\n",
				strerror(errno));
			goto out;
		}
		close(fd);
		fd = -1;
	}
	/* update checkpoint information in metadata */
	len = imsm_create_metadata_checkpoint_update(super, &u);

	if (len <= 0) {
		dprintf("imsm: Cannot prepare update\n");
		goto out;
	}
	/* update metadata locally */
	imsm_update_metadata_locally(st, u, len);
	/* and possibly remotely */
	if (st->update_tail) {
		append_metadata_update(st, u, len);
		/* during reshape we do all work inside metadata handler
		 * manage_reshape(), so metadata update has to be triggered
		 * insida it
		 */
		flush_metadata_updates(st);
		st->update_tail = &st->updates;
	} else
		free(u);

	retval = 0;
 out:
	if (fd >= 0)
		close(fd);
	return retval;
}
#endif /* MDASSEMBLE */

static void getinfo_super_imsm_volume(struct supertype *st, struct mdinfo *info, char *dmap)
{
	struct intel_super *super = st->sb;
	struct migr_record *migr_rec = super->migr_rec;
	struct imsm_dev *dev = get_imsm_dev(super, super->current_vol);
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct imsm_map *prev_map = get_imsm_map(dev, 1);
	struct imsm_map *map_to_analyse = map;
	struct dl *dl;
	char *devname;
	unsigned int component_size_alligment;
	int map_disks = info->array.raid_disks;

	memset(info, 0, sizeof(*info));
	if (prev_map)
		map_to_analyse = prev_map;

	dl = super->current_disk;

	info->container_member	  = super->current_vol;
	info->array.raid_disks    = map->num_members;
	info->array.level	  = get_imsm_raid_level(map_to_analyse);
	info->array.layout	  = imsm_level_to_layout(info->array.level);
	info->array.md_minor	  = -1;
	info->array.ctime	  = 0;
	info->array.utime	  = 0;
	info->array.chunk_size	  =
		__le16_to_cpu(map_to_analyse->blocks_per_strip) << 9;
	info->array.state	  = !dev->vol.dirty;
	info->custom_array_size   = __le32_to_cpu(dev->size_high);
	info->custom_array_size   <<= 32;
	info->custom_array_size   |= __le32_to_cpu(dev->size_low);
	if (prev_map && map->map_state == prev_map->map_state) {
		info->reshape_active = 1;
		info->new_level = get_imsm_raid_level(map);
		info->new_layout = imsm_level_to_layout(info->new_level);
		info->new_chunk = __le16_to_cpu(map->blocks_per_strip) << 9;
		info->delta_disks = map->num_members - prev_map->num_members;
		if (info->delta_disks) {
			/* this needs to be applied to every array
			 * in the container.
			 */
			info->reshape_active = 2;
		}
		/* We shape information that we give to md might have to be
		 * modify to cope with md's requirement for reshaping arrays.
		 * For example, when reshaping a RAID0, md requires it to be
		 * presented as a degraded RAID4.
		 * Also if a RAID0 is migrating to a RAID5 we need to specify
		 * the array as already being RAID5, but the 'before' layout
		 * is a RAID4-like layout.
		 */
		switch (info->array.level) {
		case 0:
			switch(info->new_level) {
			case 0:
				/* conversion is happening as RAID4 */
				info->array.level = 4;
				info->array.raid_disks += 1;
				break;
			case 5:
				/* conversion is happening as RAID5 */
				info->array.level = 5;
				info->array.layout = ALGORITHM_PARITY_N;
				info->delta_disks -= 1;
				break;
			default:
				/* FIXME error message */
				info->array.level = UnSet;
				break;
			}
			break;
		}
	} else {
		info->new_level = UnSet;
		info->new_layout = UnSet;
		info->new_chunk = info->array.chunk_size;
		info->delta_disks = 0;
	}

	if (dl) {
		info->disk.major = dl->major;
		info->disk.minor = dl->minor;
		info->disk.number = dl->index;
		info->disk.raid_disk = get_imsm_disk_slot(map_to_analyse,
							  dl->index);
	}

	info->data_offset	  = __le32_to_cpu(map_to_analyse->pba_of_lba0);
	info->component_size	  =
		__le32_to_cpu(map_to_analyse->blocks_per_member);

	/* check component size aligment
	 */
	component_size_alligment =
		info->component_size % (info->array.chunk_size/512);

	if (component_size_alligment &&
	    (info->array.level != 1) && (info->array.level != UnSet)) {
		dprintf("imsm: reported component size alligned from %llu ",
			info->component_size);
		info->component_size -= component_size_alligment;
		dprintf("to %llu (%i).\n",
			info->component_size, component_size_alligment);
	}

	memset(info->uuid, 0, sizeof(info->uuid));
	info->recovery_start = MaxSector;

	info->reshape_progress = 0;
	info->resync_start = MaxSector;
	if (map_to_analyse->map_state == IMSM_T_STATE_UNINITIALIZED ||
	    dev->vol.dirty) {
		info->resync_start = 0;
	}
	if (dev->vol.migr_state) {
		switch (migr_type(dev)) {
		case MIGR_REPAIR:
		case MIGR_INIT: {
			__u64 blocks_per_unit = blocks_per_migr_unit(super,
								     dev);
			__u64 units = __le32_to_cpu(dev->vol.curr_migr_unit);

			info->resync_start = blocks_per_unit * units;
			break;
		}
		case MIGR_GEN_MIGR: {
			__u64 blocks_per_unit = blocks_per_migr_unit(super,
								     dev);
			__u64 units = __le32_to_cpu(migr_rec->curr_migr_unit);
			unsigned long long array_blocks;
			int used_disks;

			if (__le32_to_cpu(migr_rec->ascending_migr) &&
			    (units <
				(__le32_to_cpu(migr_rec->num_migr_units)-1)) &&
			    (super->migr_rec->rec_status ==
					__cpu_to_le32(UNIT_SRC_IN_CP_AREA)))
				units++;

			info->reshape_progress = blocks_per_unit * units;

			dprintf("IMSM: General Migration checkpoint : %llu "
			       "(%llu) -> read reshape progress : %llu\n",
				(unsigned long long)units,
				(unsigned long long)blocks_per_unit,
				info->reshape_progress);

			used_disks = imsm_num_data_members(dev, 1);
			if (used_disks > 0) {
				array_blocks = map->blocks_per_member *
					used_disks;
				/* round array size down to closest MB
				 */
				info->custom_array_size = (array_blocks
						>> SECT_PER_MB_SHIFT)
						<< SECT_PER_MB_SHIFT;
			}
		}
		case MIGR_VERIFY:
			/* we could emulate the checkpointing of
			 * 'sync_action=check' migrations, but for now
			 * we just immediately complete them
			 */
		case MIGR_REBUILD:
			/* this is handled by container_content_imsm() */
		case MIGR_STATE_CHANGE:
			/* FIXME handle other migrations */
		default:
			/* we are not dirty, so... */
			info->resync_start = MaxSector;
		}
	}

	strncpy(info->name, (char *) dev->volume, MAX_RAID_SERIAL_LEN);
	info->name[MAX_RAID_SERIAL_LEN] = 0;

	info->array.major_version = -1;
	info->array.minor_version = -2;
	devname = devnum2devname(st->container_dev);
	*info->text_version = '\0';
	if (devname)
		sprintf(info->text_version, "/%s/%d", devname, info->container_member);
	free(devname);
	info->safe_mode_delay = 4000;  /* 4 secs like the Matrix driver */
	uuid_from_super_imsm(st, info->uuid);

	if (dmap) {
		int i, j;
		for (i=0; i<map_disks; i++) {
			dmap[i] = 0;
			if (i < info->array.raid_disks) {
				struct imsm_disk *dsk;
				j = get_imsm_disk_idx(dev, i, -1);
				dsk = get_imsm_disk(super, j);
				if (dsk && (dsk->status & CONFIGURED_DISK))
					dmap[i] = 1;
			}
		}
	}
}

static __u8 imsm_check_degraded(struct intel_super *super, struct imsm_dev *dev, int failed);
static int imsm_count_failed(struct intel_super *super, struct imsm_dev *dev);

static struct imsm_disk *get_imsm_missing(struct intel_super *super, __u8 index)
{
	struct dl *d;

	for (d = super->missing; d; d = d->next)
		if (d->index == index)
			return &d->disk;
	return NULL;
}

static void getinfo_super_imsm(struct supertype *st, struct mdinfo *info, char *map)
{
	struct intel_super *super = st->sb;
	struct imsm_disk *disk;
	int map_disks = info->array.raid_disks;
	int max_enough = -1;
	int i;
	struct imsm_super *mpb;

	if (super->current_vol >= 0) {
		getinfo_super_imsm_volume(st, info, map);
		return;
	}
	memset(info, 0, sizeof(*info));

	/* Set raid_disks to zero so that Assemble will always pull in valid
	 * spares
	 */
	info->array.raid_disks    = 0;
	info->array.level         = LEVEL_CONTAINER;
	info->array.layout        = 0;
	info->array.md_minor      = -1;
	info->array.ctime         = 0; /* N/A for imsm */ 
	info->array.utime         = 0;
	info->array.chunk_size    = 0;

	info->disk.major = 0;
	info->disk.minor = 0;
	info->disk.raid_disk = -1;
	info->reshape_active = 0;
	info->array.major_version = -1;
	info->array.minor_version = -2;
	strcpy(info->text_version, "imsm");
	info->safe_mode_delay = 0;
	info->disk.number = -1;
	info->disk.state = 0;
	info->name[0] = 0;
	info->recovery_start = MaxSector;

	/* do we have the all the insync disks that we expect? */
	mpb = super->anchor;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);
		int failed, enough, j, missing = 0;
		struct imsm_map *map;
		__u8 state;

		failed = imsm_count_failed(super, dev);
		state = imsm_check_degraded(super, dev, failed);
		map = get_imsm_map(dev, dev->vol.migr_state);

		/* any newly missing disks?
		 * (catches single-degraded vs double-degraded)
		 */
		for (j = 0; j < map->num_members; j++) {
			__u32 ord = get_imsm_ord_tbl_ent(dev, i, -1);
			__u32 idx = ord_to_idx(ord);

			if (!(ord & IMSM_ORD_REBUILD) &&
			    get_imsm_missing(super, idx)) {
				missing = 1;
				break;
			}
		}

		if (state == IMSM_T_STATE_FAILED)
			enough = -1;
		else if (state == IMSM_T_STATE_DEGRADED &&
			 (state != map->map_state || missing))
			enough = 0;
		else /* we're normal, or already degraded */
			enough = 1;

		/* in the missing/failed disk case check to see
		 * if at least one array is runnable
		 */
		max_enough = max(max_enough, enough);
	}
	dprintf("%s: enough: %d\n", __func__, max_enough);
	info->container_enough = max_enough;

	if (super->disks) {
		__u32 reserved = imsm_reserved_sectors(super, super->disks);

		disk = &super->disks->disk;
		info->data_offset = __le32_to_cpu(disk->total_blocks) - reserved;
		info->component_size = reserved;
		info->disk.state  = is_configured(disk) ? (1 << MD_DISK_ACTIVE) : 0;
		/* we don't change info->disk.raid_disk here because
		 * this state will be finalized in mdmon after we have
		 * found the 'most fresh' version of the metadata
		 */
		info->disk.state |= is_failed(disk) ? (1 << MD_DISK_FAULTY) : 0;
		info->disk.state |= is_spare(disk) ? 0 : (1 << MD_DISK_SYNC);
	}

	/* only call uuid_from_super_imsm when this disk is part of a populated container,
	 * ->compare_super may have updated the 'num_raid_devs' field for spares
	 */
	if (info->disk.state & (1 << MD_DISK_SYNC) || super->anchor->num_raid_devs)
		uuid_from_super_imsm(st, info->uuid);
	else
		memcpy(info->uuid, uuid_zero, sizeof(uuid_zero));

	/* I don't know how to compute 'map' on imsm, so use safe default */
	if (map) {
		int i;
		for (i = 0; i < map_disks; i++)
			map[i] = 1;
	}

}

/* allocates memory and fills disk in mdinfo structure
 * for each disk in array */
struct mdinfo *getinfo_super_disks_imsm(struct supertype *st)
{
	struct mdinfo *mddev = NULL;
	struct intel_super *super = st->sb;
	struct imsm_disk *disk;
	int count = 0;
	struct dl *dl;
	if (!super || !super->disks)
		return NULL;
	dl = super->disks;
	mddev = malloc(sizeof(*mddev));
	if (!mddev) {
		fprintf(stderr, Name ": Failed to allocate memory.\n");
		return NULL;
	}
	memset(mddev, 0, sizeof(*mddev));
	while (dl) {
		struct mdinfo *tmp;
		disk = &dl->disk;
		tmp = malloc(sizeof(*tmp));
		if (!tmp) {
			fprintf(stderr, Name ": Failed to allocate memory.\n");
			if (mddev)
				sysfs_free(mddev);
			return NULL;
		}
		memset(tmp, 0, sizeof(*tmp));
		if (mddev->devs)
			tmp->next = mddev->devs;
		mddev->devs = tmp;
		tmp->disk.number = count++;
		tmp->disk.major = dl->major;
		tmp->disk.minor = dl->minor;
		tmp->disk.state = is_configured(disk) ?
				  (1 << MD_DISK_ACTIVE) : 0;
		tmp->disk.state |= is_failed(disk) ? (1 << MD_DISK_FAULTY) : 0;
		tmp->disk.state |= is_spare(disk) ? 0 : (1 << MD_DISK_SYNC);
		tmp->disk.raid_disk = -1;
		dl = dl->next;
	}
	return mddev;
}

static int update_super_imsm(struct supertype *st, struct mdinfo *info,
			     char *update, char *devname, int verbose,
			     int uuid_set, char *homehost)
{
	/* For 'assemble' and 'force' we need to return non-zero if any
	 * change was made.  For others, the return value is ignored.
	 * Update options are:
	 *  force-one : This device looks a bit old but needs to be included,
	 *        update age info appropriately.
	 *  assemble: clear any 'faulty' flag to allow this device to
	 *		be assembled.
	 *  force-array: Array is degraded but being forced, mark it clean
	 *	   if that will be needed to assemble it.
	 *
	 *  newdev:  not used ????
	 *  grow:  Array has gained a new device - this is currently for
	 *		linear only
	 *  resync: mark as dirty so a resync will happen.
	 *  name:  update the name - preserving the homehost
	 *  uuid:  Change the uuid of the array to match watch is given
	 *
	 * Following are not relevant for this imsm:
	 *  sparc2.2 : update from old dodgey metadata
	 *  super-minor: change the preferred_minor number
	 *  summaries:  update redundant counters.
	 *  homehost:  update the recorded homehost
	 *  _reshape_progress: record new reshape_progress position.
	 */
	int rv = 1;
	struct intel_super *super = st->sb;
	struct imsm_super *mpb;

	/* we can only update container info */
	if (!super || super->current_vol >= 0 || !super->anchor)
		return 1;

	mpb = super->anchor;

	if (strcmp(update, "uuid") == 0 && uuid_set && !info->update_private)
		rv = -1;
	else if (strcmp(update, "uuid") == 0 && uuid_set && info->update_private) {
		mpb->orig_family_num = *((__u32 *) info->update_private);
		rv = 0;
	} else if (strcmp(update, "uuid") == 0) {
		__u32 *new_family = malloc(sizeof(*new_family));

		/* update orig_family_number with the incoming random
		 * data, report the new effective uuid, and store the
		 * new orig_family_num for future updates.
		 */
		if (new_family) {
			memcpy(&mpb->orig_family_num, info->uuid, sizeof(__u32));
			uuid_from_super_imsm(st, info->uuid);
			*new_family = mpb->orig_family_num;
			info->update_private = new_family;
			rv = 0;
		}
	} else if (strcmp(update, "assemble") == 0)
		rv = 0;
	else
		rv = -1;

	/* successful update? recompute checksum */
	if (rv == 0)
		mpb->check_sum = __le32_to_cpu(__gen_imsm_checksum(mpb));

	return rv;
}

static size_t disks_to_mpb_size(int disks)
{
	size_t size;

	size = sizeof(struct imsm_super);
	size += (disks - 1) * sizeof(struct imsm_disk);
	size += 2 * sizeof(struct imsm_dev);
	/* up to 2 maps per raid device (-2 for imsm_maps in imsm_dev */
	size += (4 - 2) * sizeof(struct imsm_map);
	/* 4 possible disk_ord_tbl's */
	size += 4 * (disks - 1) * sizeof(__u32);

	return size;
}

static __u64 avail_size_imsm(struct supertype *st, __u64 devsize)
{
	if (devsize < (MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS))
		return 0;

	return devsize - (MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS);
}

static void free_devlist(struct intel_super *super)
{
	struct intel_dev *dv;

	while (super->devlist) {
		dv = super->devlist->next;
		free(super->devlist->dev);
		free(super->devlist);
		super->devlist = dv;
	}
}

static void imsm_copy_dev(struct imsm_dev *dest, struct imsm_dev *src)
{
	memcpy(dest, src, sizeof_imsm_dev(src, 0));
}

static int compare_super_imsm(struct supertype *st, struct supertype *tst)
{
	/*
	 * return:
	 *  0 same, or first was empty, and second was copied
	 *  1 second had wrong number
	 *  2 wrong uuid
	 *  3 wrong other info
	 */
	struct intel_super *first = st->sb;
	struct intel_super *sec = tst->sb;

        if (!first) {
                st->sb = tst->sb;
                tst->sb = NULL;
                return 0;
        }
	/* in platform dependent environment test if the disks
	 * use the same Intel hba
	 */
	if (!check_env("IMSM_NO_PLATFORM")) {
		if (!first->hba || !sec->hba ||
		    (first->hba->type != sec->hba->type))  {
			fprintf(stderr,
				"HBAs of devices does not match %s != %s\n",
				first->hba ? get_sys_dev_type(first->hba->type) : NULL,
				sec->hba ? get_sys_dev_type(sec->hba->type) : NULL);
			return 3;
		}
	}

	/* if an anchor does not have num_raid_devs set then it is a free
	 * floating spare
	 */
	if (first->anchor->num_raid_devs > 0 &&
	    sec->anchor->num_raid_devs > 0) {
		/* Determine if these disks might ever have been
		 * related.  Further disambiguation can only take place
		 * in load_super_imsm_all
		 */
		__u32 first_family = first->anchor->orig_family_num;
		__u32 sec_family = sec->anchor->orig_family_num;

		if (memcmp(first->anchor->sig, sec->anchor->sig,
			   MAX_SIGNATURE_LENGTH) != 0)
			return 3;

		if (first_family == 0)
			first_family = first->anchor->family_num;
		if (sec_family == 0)
			sec_family = sec->anchor->family_num;

		if (first_family != sec_family)
			return 3;

	}


	/* if 'first' is a spare promote it to a populated mpb with sec's
	 * family number
	 */
	if (first->anchor->num_raid_devs == 0 &&
	    sec->anchor->num_raid_devs > 0) {
		int i;
		struct intel_dev *dv;
		struct imsm_dev *dev;

		/* we need to copy raid device info from sec if an allocation
		 * fails here we don't associate the spare
		 */
		for (i = 0; i < sec->anchor->num_raid_devs; i++) {
			dv = malloc(sizeof(*dv));
			if (!dv)
				break;
			dev = malloc(sizeof_imsm_dev(get_imsm_dev(sec, i), 1));
			if (!dev) {
				free(dv);
				break;
			}
			dv->dev = dev;
			dv->index = i;
			dv->next = first->devlist;
			first->devlist = dv;
		}
		if (i < sec->anchor->num_raid_devs) {
			/* allocation failure */
			free_devlist(first);
			fprintf(stderr, "imsm: failed to associate spare\n"); 
			return 3;
		}
		first->anchor->num_raid_devs = sec->anchor->num_raid_devs;
		first->anchor->orig_family_num = sec->anchor->orig_family_num;
		first->anchor->family_num = sec->anchor->family_num;
		memcpy(first->anchor->sig, sec->anchor->sig, MAX_SIGNATURE_LENGTH);
		for (i = 0; i < sec->anchor->num_raid_devs; i++)
			imsm_copy_dev(get_imsm_dev(first, i), get_imsm_dev(sec, i));
	}

	return 0;
}

static void fd2devname(int fd, char *name)
{
	struct stat st;
	char path[256];
	char dname[PATH_MAX];
	char *nm;
	int rv;

	name[0] = '\0';
	if (fstat(fd, &st) != 0)
		return;
	sprintf(path, "/sys/dev/block/%d:%d",
		major(st.st_rdev), minor(st.st_rdev));

	rv = readlink(path, dname, sizeof(dname));
	if (rv <= 0)
		return;
	
	dname[rv] = '\0';
	nm = strrchr(dname, '/');
	nm++;
	snprintf(name, MAX_RAID_SERIAL_LEN, "/dev/%s", nm);
}

extern int scsi_get_serial(int fd, void *buf, size_t buf_len);

static int imsm_read_serial(int fd, char *devname,
			    __u8 serial[MAX_RAID_SERIAL_LEN])
{
	unsigned char scsi_serial[255];
	int rv;
	int rsp_len;
	int len;
	char *dest;
	char *src;
	char *rsp_buf;
	int i;

	memset(scsi_serial, 0, sizeof(scsi_serial));

	rv = scsi_get_serial(fd, scsi_serial, sizeof(scsi_serial));

	if (rv && check_env("IMSM_DEVNAME_AS_SERIAL")) {
		memset(serial, 0, MAX_RAID_SERIAL_LEN);
		fd2devname(fd, (char *) serial);
		return 0;
	}

	if (rv != 0) {
		if (devname)
			fprintf(stderr,
				Name ": Failed to retrieve serial for %s\n",
				devname);
		return rv;
	}

	rsp_len = scsi_serial[3];
	if (!rsp_len) {
		if (devname)
			fprintf(stderr,
				Name ": Failed to retrieve serial for %s\n",
				devname);
		return 2;
	}
	rsp_buf = (char *) &scsi_serial[4];

	/* trim all whitespace and non-printable characters and convert
	 * ':' to ';'
	 */
	for (i = 0, dest = rsp_buf; i < rsp_len; i++) {
		src = &rsp_buf[i];
		if (*src > 0x20) {
			/* ':' is reserved for use in placeholder serial
			 * numbers for missing disks
			 */
			if (*src == ':')
				*dest++ = ';';
			else
				*dest++ = *src;
		}
	}
	len = dest - rsp_buf;
	dest = rsp_buf;

	/* truncate leading characters */
	if (len > MAX_RAID_SERIAL_LEN) {
		dest += len - MAX_RAID_SERIAL_LEN;
		len = MAX_RAID_SERIAL_LEN;
	}

	memset(serial, 0, MAX_RAID_SERIAL_LEN);
	memcpy(serial, dest, len);

	return 0;
}

static int serialcmp(__u8 *s1, __u8 *s2)
{
	return strncmp((char *) s1, (char *) s2, MAX_RAID_SERIAL_LEN);
}

static void serialcpy(__u8 *dest, __u8 *src)
{
	strncpy((char *) dest, (char *) src, MAX_RAID_SERIAL_LEN);
}

#ifndef MDASSEMBLE
static struct dl *serial_to_dl(__u8 *serial, struct intel_super *super)
{
	struct dl *dl;

	for (dl = super->disks; dl; dl = dl->next)
		if (serialcmp(dl->serial, serial) == 0)
			break;

	return dl;
}
#endif

static struct imsm_disk *
__serial_to_disk(__u8 *serial, struct imsm_super *mpb, int *idx)
{
	int i;

	for (i = 0; i < mpb->num_disks; i++) {
		struct imsm_disk *disk = __get_imsm_disk(mpb, i);

		if (serialcmp(disk->serial, serial) == 0) {
			if (idx)
				*idx = i;
			return disk;
		}
	}

	return NULL;
}

static int
load_imsm_disk(int fd, struct intel_super *super, char *devname, int keep_fd)
{
	struct imsm_disk *disk;
	struct dl *dl;
	struct stat stb;
	int rv;
	char name[40];
	__u8 serial[MAX_RAID_SERIAL_LEN];

	rv = imsm_read_serial(fd, devname, serial);

	if (rv != 0)
		return 2;

	dl = calloc(1, sizeof(*dl));
	if (!dl) {
		if (devname)
			fprintf(stderr,
				Name ": failed to allocate disk buffer for %s\n",
				devname);
		return 2;
	}

	fstat(fd, &stb);
	dl->major = major(stb.st_rdev);
	dl->minor = minor(stb.st_rdev);
	dl->next = super->disks;
	dl->fd = keep_fd ? fd : -1;
	assert(super->disks == NULL);
	super->disks = dl;
	serialcpy(dl->serial, serial);
	dl->index = -2;
	dl->e = NULL;
	fd2devname(fd, name);
	if (devname)
		dl->devname = strdup(devname);
	else
		dl->devname = strdup(name);

	/* look up this disk's index in the current anchor */
	disk = __serial_to_disk(dl->serial, super->anchor, &dl->index);
	if (disk) {
		dl->disk = *disk;
		/* only set index on disks that are a member of a
		 * populated contianer, i.e. one with raid_devs
		 */
		if (is_failed(&dl->disk))
			dl->index = -2;
		else if (is_spare(&dl->disk))
			dl->index = -1;
	}

	return 0;
}

#ifndef MDASSEMBLE
/* When migrating map0 contains the 'destination' state while map1
 * contains the current state.  When not migrating map0 contains the
 * current state.  This routine assumes that map[0].map_state is set to
 * the current array state before being called.
 *
 * Migration is indicated by one of the following states
 * 1/ Idle (migr_state=0 map0state=normal||unitialized||degraded||failed)
 * 2/ Initialize (migr_state=1 migr_type=MIGR_INIT map0state=normal
 *    map1state=unitialized)
 * 3/ Repair (Resync) (migr_state=1 migr_type=MIGR_REPAIR  map0state=normal
 *    map1state=normal)
 * 4/ Rebuild (migr_state=1 migr_type=MIGR_REBUILD map0state=normal
 *    map1state=degraded)
 * 5/ Migration (mig_state=1 migr_type=MIGR_GEN_MIGR map0state=normal
 *    map1state=normal)
 */
static void migrate(struct imsm_dev *dev, struct intel_super *super,
		    __u8 to_state, int migr_type)
{
	struct imsm_map *dest;
	struct imsm_map *src = get_imsm_map(dev, 0);

	dev->vol.migr_state = 1;
	set_migr_type(dev, migr_type);
	dev->vol.curr_migr_unit = 0;
	dest = get_imsm_map(dev, 1);

	/* duplicate and then set the target end state in map[0] */
	memcpy(dest, src, sizeof_imsm_map(src));
	if ((migr_type == MIGR_REBUILD) ||
	    (migr_type ==  MIGR_GEN_MIGR)) {
		__u32 ord;
		int i;

		for (i = 0; i < src->num_members; i++) {
			ord = __le32_to_cpu(src->disk_ord_tbl[i]);
			set_imsm_ord_tbl_ent(src, i, ord_to_idx(ord));
		}
	}

	if (migr_type == MIGR_GEN_MIGR)
		/* Clear migration record */
		memset(super->migr_rec, 0, sizeof(struct migr_record));

	src->map_state = to_state;
}

static void end_migration(struct imsm_dev *dev, __u8 map_state)
{
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct imsm_map *prev = get_imsm_map(dev, dev->vol.migr_state);
	int i, j;

	/* merge any IMSM_ORD_REBUILD bits that were not successfully
	 * completed in the last migration.
	 *
	 * FIXME add support for raid-level-migration
	 */
	for (i = 0; i < prev->num_members; i++)
		for (j = 0; j < map->num_members; j++)
			/* during online capacity expansion
			 * disks position can be changed if takeover is used
			 */
			if (ord_to_idx(map->disk_ord_tbl[j]) ==
			    ord_to_idx(prev->disk_ord_tbl[i])) {
				map->disk_ord_tbl[j] |= prev->disk_ord_tbl[i];
				break;
			}

	dev->vol.migr_state = 0;
	dev->vol.migr_type = 0;
	dev->vol.curr_migr_unit = 0;
	map->map_state = map_state;
}
#endif

static int parse_raid_devices(struct intel_super *super)
{
	int i;
	struct imsm_dev *dev_new;
	size_t len, len_migr;
	size_t max_len = 0;
	size_t space_needed = 0;
	struct imsm_super *mpb = super->anchor;

	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev_iter = __get_imsm_dev(super->anchor, i);
		struct intel_dev *dv;

		len = sizeof_imsm_dev(dev_iter, 0);
		len_migr = sizeof_imsm_dev(dev_iter, 1);
		if (len_migr > len)
			space_needed += len_migr - len;
		
		dv = malloc(sizeof(*dv));
		if (!dv)
			return 1;
		if (max_len < len_migr)
			max_len = len_migr;
		if (max_len > len_migr)
			space_needed += max_len - len_migr;
		dev_new = malloc(max_len);
		if (!dev_new) {
			free(dv);
			return 1;
		}
		imsm_copy_dev(dev_new, dev_iter);
		dv->dev = dev_new;
		dv->index = i;
		dv->next = super->devlist;
		super->devlist = dv;
	}

	/* ensure that super->buf is large enough when all raid devices
	 * are migrating
	 */
	if (__le32_to_cpu(mpb->mpb_size) + space_needed > super->len) {
		void *buf;

		len = ROUND_UP(__le32_to_cpu(mpb->mpb_size) + space_needed, 512);
		if (posix_memalign(&buf, 512, len) != 0)
			return 1;

		memcpy(buf, super->buf, super->len);
		memset(buf + super->len, 0, len - super->len);
		free(super->buf);
		super->buf = buf;
		super->len = len;
	}
		
	return 0;
}

/* retrieve a pointer to the bbm log which starts after all raid devices */
struct bbm_log *__get_imsm_bbm_log(struct imsm_super *mpb)
{
	void *ptr = NULL;

	if (__le32_to_cpu(mpb->bbm_log_size)) {
		ptr = mpb;
		ptr += mpb->mpb_size - __le32_to_cpu(mpb->bbm_log_size);
	} 

	return ptr;
}

/*******************************************************************************
 * Function:	check_mpb_migr_compatibility
 * Description:	Function checks for unsupported migration features:
 *		- migration optimization area (pba_of_lba0)
 *		- descending reshape (ascending_migr)
 * Parameters:
 *	super	: imsm metadata information
 * Returns:
 *	 0 : migration is compatible
 *	-1 : migration is not compatible
 ******************************************************************************/
int check_mpb_migr_compatibility(struct intel_super *super)
{
	struct imsm_map *map0, *map1;
	struct migr_record *migr_rec = super->migr_rec;
	int i;

	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev_iter = __get_imsm_dev(super->anchor, i);

		if (dev_iter &&
		    dev_iter->vol.migr_state == 1 &&
		    dev_iter->vol.migr_type == MIGR_GEN_MIGR) {
			/* This device is migrating */
			map0 = get_imsm_map(dev_iter, 0);
			map1 = get_imsm_map(dev_iter, 1);
			if (map0->pba_of_lba0 != map1->pba_of_lba0)
				/* migration optimization area was used */
				return -1;
			if (migr_rec->ascending_migr == 0
				&& migr_rec->dest_depth_per_unit > 0)
				/* descending reshape not supported yet */
				return -1;
		}
	}
	return 0;
}

static void __free_imsm(struct intel_super *super, int free_disks);

/* load_imsm_mpb - read matrix metadata
 * allocates super->mpb to be freed by free_imsm
 */
static int load_imsm_mpb(int fd, struct intel_super *super, char *devname)
{
	unsigned long long dsize;
	unsigned long long sectors;
	struct stat;
	struct imsm_super *anchor;
	__u32 check_sum;

	get_dev_size(fd, NULL, &dsize);
	if (dsize < 1024) {
		if (devname)
			fprintf(stderr,
				Name ": %s: device to small for imsm\n",
				devname);
		return 1;
	}

	if (lseek64(fd, dsize - (512 * 2), SEEK_SET) < 0) {
		if (devname)
			fprintf(stderr, Name
				": Cannot seek to anchor block on %s: %s\n",
				devname, strerror(errno));
		return 1;
	}

	if (posix_memalign((void**)&anchor, 512, 512) != 0) {
		if (devname)
			fprintf(stderr,
				Name ": Failed to allocate imsm anchor buffer"
				" on %s\n", devname);
		return 1;
	}
	if (read(fd, anchor, 512) != 512) {
		if (devname)
			fprintf(stderr,
				Name ": Cannot read anchor block on %s: %s\n",
				devname, strerror(errno));
		free(anchor);
		return 1;
	}

	if (strncmp((char *) anchor->sig, MPB_SIGNATURE, MPB_SIG_LEN) != 0) {
		if (devname)
			fprintf(stderr,
				Name ": no IMSM anchor on %s\n", devname);
		free(anchor);
		return 2;
	}

	__free_imsm(super, 0);
	/*  reload capability and hba */

	/* capability and hba must be updated with new super allocation */
	find_intel_hba_capability(fd, super, devname);
	super->len = ROUND_UP(anchor->mpb_size, 512);
	if (posix_memalign(&super->buf, 512, super->len) != 0) {
		if (devname)
			fprintf(stderr,
				Name ": unable to allocate %zu byte mpb buffer\n",
				super->len);
		free(anchor);
		return 2;
	}
	memcpy(super->buf, anchor, 512);

	sectors = mpb_sectors(anchor) - 1;
	free(anchor);

	if (posix_memalign(&super->migr_rec_buf, 512, 512) != 0) {
		fprintf(stderr, Name
			": %s could not allocate migr_rec buffer\n", __func__);
		free(super->buf);
		return 2;
	}

	if (!sectors) {
		check_sum = __gen_imsm_checksum(super->anchor);
		if (check_sum != __le32_to_cpu(super->anchor->check_sum)) {
			if (devname)
				fprintf(stderr,
					Name ": IMSM checksum %x != %x on %s\n",
					check_sum,
					__le32_to_cpu(super->anchor->check_sum),
					devname);
			return 2;
		}

		return 0;
	}

	/* read the extended mpb */
	if (lseek64(fd, dsize - (512 * (2 + sectors)), SEEK_SET) < 0) {
		if (devname)
			fprintf(stderr,
				Name ": Cannot seek to extended mpb on %s: %s\n",
				devname, strerror(errno));
		return 1;
	}

	if ((unsigned)read(fd, super->buf + 512, super->len - 512) != super->len - 512) {
		if (devname)
			fprintf(stderr,
				Name ": Cannot read extended mpb on %s: %s\n",
				devname, strerror(errno));
		return 2;
	}

	check_sum = __gen_imsm_checksum(super->anchor);
	if (check_sum != __le32_to_cpu(super->anchor->check_sum)) {
		if (devname)
			fprintf(stderr,
				Name ": IMSM checksum %x != %x on %s\n",
				check_sum, __le32_to_cpu(super->anchor->check_sum),
				devname);
		return 3;
	}

	/* FIXME the BBM log is disk specific so we cannot use this global
	 * buffer for all disks.  Ok for now since we only look at the global
	 * bbm_log_size parameter to gate assembly
	 */
	super->bbm_log = __get_imsm_bbm_log(super->anchor);

	return 0;
}

static int read_imsm_migr_rec(int fd, struct intel_super *super);

static int
load_and_parse_mpb(int fd, struct intel_super *super, char *devname, int keep_fd)
{
	int err;

	err = load_imsm_mpb(fd, super, devname);
	if (err)
		return err;
	err = load_imsm_disk(fd, super, devname, keep_fd);
	if (err)
		return err;
	err = parse_raid_devices(super);

	return err;
}

static void __free_imsm_disk(struct dl *d)
{
	if (d->fd >= 0)
		close(d->fd);
	if (d->devname)
		free(d->devname);
	if (d->e)
		free(d->e);
	free(d);

}

static void free_imsm_disks(struct intel_super *super)
{
	struct dl *d;

	while (super->disks) {
		d = super->disks;
		super->disks = d->next;
		__free_imsm_disk(d);
	}
	while (super->disk_mgmt_list) {
		d = super->disk_mgmt_list;
		super->disk_mgmt_list = d->next;
		__free_imsm_disk(d);
	}
	while (super->missing) {
		d = super->missing;
		super->missing = d->next;
		__free_imsm_disk(d);
	}

}

/* free all the pieces hanging off of a super pointer */
static void __free_imsm(struct intel_super *super, int free_disks)
{
	struct intel_hba *elem, *next;

	if (super->buf) {
		free(super->buf);
		super->buf = NULL;
	}
	/* unlink capability description */
	super->orom = NULL;
	if (super->migr_rec_buf) {
		free(super->migr_rec_buf);
		super->migr_rec_buf = NULL;
	}
	if (free_disks)
		free_imsm_disks(super);
	free_devlist(super);
	elem = super->hba;
	while (elem) {
		if (elem->path)
			free((void *)elem->path);
		next = elem->next;
		free(elem);
		elem = next;
	}
	super->hba = NULL;
}

static void free_imsm(struct intel_super *super)
{
	__free_imsm(super, 1);
	free(super);
}

static void free_super_imsm(struct supertype *st)
{
	struct intel_super *super = st->sb;

	if (!super)
		return;

	free_imsm(super);
	st->sb = NULL;
}

static struct intel_super *alloc_super(void)
{
	struct intel_super *super = malloc(sizeof(*super));

	if (super) {
		memset(super, 0, sizeof(*super));
		super->current_vol = -1;
		super->create_offset = ~((__u32 ) 0);
	}
	return super;
}

/*
 * find and allocate hba and OROM/EFI based on valid fd of RAID component device
 */
static int find_intel_hba_capability(int fd, struct intel_super *super, char *devname)
{
	struct sys_dev *hba_name;
	int rv = 0;

	if ((fd < 0) || check_env("IMSM_NO_PLATFORM")) {
		super->orom = NULL;
		super->hba = NULL;
		return 0;
	}
	hba_name = find_disk_attached_hba(fd, NULL);
	if (!hba_name) {
		if (devname)
			fprintf(stderr,
				Name ": %s is not attached to Intel(R) RAID controller.\n",
				devname);
		return 1;
	}
	rv = attach_hba_to_super(super, hba_name);
	if (rv == 2) {
		if (devname) {
			struct intel_hba *hba = super->hba;

			fprintf(stderr, Name ": %s is attached to Intel(R) %s RAID "
				"controller (%s),\n"
				"    but the container is assigned to Intel(R) "
				"%s RAID controller (",
				devname,
				hba_name->path,
				hba_name->pci_id ? : "Err!",
				get_sys_dev_type(hba_name->type));

			while (hba) {
				fprintf(stderr, "%s", hba->pci_id ? : "Err!");
				if (hba->next)
					fprintf(stderr, ", ");
				hba = hba->next;
			}

			fprintf(stderr, ").\n"
				"    Mixing devices attached to different controllers "
				"is not allowed.\n");
		}
		free_sys_dev(&hba_name);
		return 2;
	}
	super->orom = find_imsm_capability(hba_name->type);
	free_sys_dev(&hba_name);
	if (!super->orom)
		return 3;
	return 0;
}

#ifndef MDASSEMBLE
/* find_missing - helper routine for load_super_imsm_all that identifies
 * disks that have disappeared from the system.  This routine relies on
 * the mpb being uptodate, which it is at load time.
 */
static int find_missing(struct intel_super *super)
{
	int i;
	struct imsm_super *mpb = super->anchor;
	struct dl *dl;
	struct imsm_disk *disk;

	for (i = 0; i < mpb->num_disks; i++) {
		disk = __get_imsm_disk(mpb, i);
		dl = serial_to_dl(disk->serial, super);
		if (dl)
			continue;

		dl = malloc(sizeof(*dl));
		if (!dl)
			return 1;
		dl->major = 0;
		dl->minor = 0;
		dl->fd = -1;
		dl->devname = strdup("missing");
		dl->index = i;
		serialcpy(dl->serial, disk->serial);
		dl->disk = *disk;
		dl->e = NULL;
		dl->next = super->missing;
		super->missing = dl;
	}

	return 0;
}

static struct intel_disk *disk_list_get(__u8 *serial, struct intel_disk *disk_list)
{
	struct intel_disk *idisk = disk_list;

	while (idisk) {
		if (serialcmp(idisk->disk.serial, serial) == 0)
			break;
		idisk = idisk->next;
	}

	return idisk;
}

static int __prep_thunderdome(struct intel_super **table, int tbl_size,
			      struct intel_super *super,
			      struct intel_disk **disk_list)
{
	struct imsm_disk *d = &super->disks->disk;
	struct imsm_super *mpb = super->anchor;
	int i, j;

	for (i = 0; i < tbl_size; i++) {
		struct imsm_super *tbl_mpb = table[i]->anchor;
		struct imsm_disk *tbl_d = &table[i]->disks->disk;

		if (tbl_mpb->family_num == mpb->family_num) {
			if (tbl_mpb->check_sum == mpb->check_sum) {
				dprintf("%s: mpb from %d:%d matches %d:%d\n",
					__func__, super->disks->major,
					super->disks->minor,
					table[i]->disks->major,
					table[i]->disks->minor);
				break;
			}

			if (((is_configured(d) && !is_configured(tbl_d)) ||
			     is_configured(d) == is_configured(tbl_d)) &&
			    tbl_mpb->generation_num < mpb->generation_num) {
				/* current version of the mpb is a
				 * better candidate than the one in
				 * super_table, but copy over "cross
				 * generational" status
				 */
				struct intel_disk *idisk;

				dprintf("%s: mpb from %d:%d replaces %d:%d\n",
					__func__, super->disks->major,
					super->disks->minor,
					table[i]->disks->major,
					table[i]->disks->minor);

				idisk = disk_list_get(tbl_d->serial, *disk_list);
				if (idisk && is_failed(&idisk->disk))
					tbl_d->status |= FAILED_DISK;
				break;
			} else {
				struct intel_disk *idisk;
				struct imsm_disk *disk;

				/* tbl_mpb is more up to date, but copy
				 * over cross generational status before
				 * returning
				 */
				disk = __serial_to_disk(d->serial, mpb, NULL);
				if (disk && is_failed(disk))
					d->status |= FAILED_DISK;

				idisk = disk_list_get(d->serial, *disk_list);
				if (idisk) {
					idisk->owner = i;
					if (disk && is_configured(disk))
						idisk->disk.status |= CONFIGURED_DISK;
				}

				dprintf("%s: mpb from %d:%d prefer %d:%d\n",
					__func__, super->disks->major,
					super->disks->minor,
					table[i]->disks->major,
					table[i]->disks->minor);

				return tbl_size;
			}
		}
	}

	if (i >= tbl_size)
		table[tbl_size++] = super;
	else
		table[i] = super;

	/* update/extend the merged list of imsm_disk records */
	for (j = 0; j < mpb->num_disks; j++) {
		struct imsm_disk *disk = __get_imsm_disk(mpb, j);
		struct intel_disk *idisk;

		idisk = disk_list_get(disk->serial, *disk_list);
		if (idisk) {
			idisk->disk.status |= disk->status;
			if (is_configured(&idisk->disk) ||
			    is_failed(&idisk->disk))
				idisk->disk.status &= ~(SPARE_DISK);
		} else {
			idisk = calloc(1, sizeof(*idisk));
			if (!idisk)
				return -1;
			idisk->owner = IMSM_UNKNOWN_OWNER;
			idisk->disk = *disk;
			idisk->next = *disk_list;
			*disk_list = idisk;
		}

		if (serialcmp(idisk->disk.serial, d->serial) == 0)
			idisk->owner = i;
	}

	return tbl_size;
}

static struct intel_super *
validate_members(struct intel_super *super, struct intel_disk *disk_list,
		 const int owner)
{
	struct imsm_super *mpb = super->anchor;
	int ok_count = 0;
	int i;

	for (i = 0; i < mpb->num_disks; i++) {
		struct imsm_disk *disk = __get_imsm_disk(mpb, i);
		struct intel_disk *idisk;

		idisk = disk_list_get(disk->serial, disk_list);
		if (idisk) {
			if (idisk->owner == owner ||
			    idisk->owner == IMSM_UNKNOWN_OWNER)
				ok_count++;
			else
				dprintf("%s: '%.16s' owner %d != %d\n",
					__func__, disk->serial, idisk->owner,
					owner);
		} else {
			dprintf("%s: unknown disk %x [%d]: %.16s\n",
				__func__, __le32_to_cpu(mpb->family_num), i,
				disk->serial);
			break;
		}
	}

	if (ok_count == mpb->num_disks)
		return super;
	return NULL;
}

static void show_conflicts(__u32 family_num, struct intel_super *super_list)
{
	struct intel_super *s;

	for (s = super_list; s; s = s->next) {
		if (family_num != s->anchor->family_num)
			continue;
		fprintf(stderr, "Conflict, offlining family %#x on '%s'\n",
			__le32_to_cpu(family_num), s->disks->devname);
	}
}

static struct intel_super *
imsm_thunderdome(struct intel_super **super_list, int len)
{
	struct intel_super *super_table[len];
	struct intel_disk *disk_list = NULL;
	struct intel_super *champion, *spare;
	struct intel_super *s, **del;
	int tbl_size = 0;
	int conflict;
	int i;

	memset(super_table, 0, sizeof(super_table));
	for (s = *super_list; s; s = s->next)
		tbl_size = __prep_thunderdome(super_table, tbl_size, s, &disk_list);

	for (i = 0; i < tbl_size; i++) {
		struct imsm_disk *d;
		struct intel_disk *idisk;
		struct imsm_super *mpb = super_table[i]->anchor;

		s = super_table[i];
		d = &s->disks->disk;

		/* 'd' must appear in merged disk list for its
		 * configuration to be valid
		 */
		idisk = disk_list_get(d->serial, disk_list);
		if (idisk && idisk->owner == i)
			s = validate_members(s, disk_list, i);
		else
			s = NULL;

		if (!s)
			dprintf("%s: marking family: %#x from %d:%d offline\n",
				__func__, mpb->family_num,
				super_table[i]->disks->major,
				super_table[i]->disks->minor);
		super_table[i] = s;
	}

	/* This is where the mdadm implementation differs from the Windows
	 * driver which has no strict concept of a container.  We can only
	 * assemble one family from a container, so when returning a prodigal
	 * array member to this system the code will not be able to disambiguate
	 * the container contents that should be assembled ("foreign" versus
	 * "local").  It requires user intervention to set the orig_family_num
	 * to a new value to establish a new container.  The Windows driver in
	 * this situation fixes up the volume name in place and manages the
	 * foreign array as an independent entity.
	 */
	s = NULL;
	spare = NULL;
	conflict = 0;
	for (i = 0; i < tbl_size; i++) {
		struct intel_super *tbl_ent = super_table[i];
		int is_spare = 0;

		if (!tbl_ent)
			continue;

		if (tbl_ent->anchor->num_raid_devs == 0) {
			spare = tbl_ent;
			is_spare = 1;
		}

		if (s && !is_spare) {
			show_conflicts(tbl_ent->anchor->family_num, *super_list);
			conflict++;
		} else if (!s && !is_spare)
			s = tbl_ent;
	}

	if (!s)
		s = spare;
	if (!s) {
		champion = NULL;
		goto out;
	}
	champion = s;

	if (conflict)
		fprintf(stderr, "Chose family %#x on '%s', "
			"assemble conflicts to new container with '--update=uuid'\n",
			__le32_to_cpu(s->anchor->family_num), s->disks->devname);

	/* collect all dl's onto 'champion', and update them to
	 * champion's version of the status
	 */
	for (s = *super_list; s; s = s->next) {
		struct imsm_super *mpb = champion->anchor;
		struct dl *dl = s->disks;

		if (s == champion)
			continue;

		for (i = 0; i < mpb->num_disks; i++) {
			struct imsm_disk *disk;

			disk = __serial_to_disk(dl->serial, mpb, &dl->index);
			if (disk) {
				dl->disk = *disk;
				/* only set index on disks that are a member of
				 * a populated contianer, i.e. one with
				 * raid_devs
				 */
				if (is_failed(&dl->disk))
					dl->index = -2;
				else if (is_spare(&dl->disk))
					dl->index = -1;
				break;
			}
		}

		if (i >= mpb->num_disks) {
			struct intel_disk *idisk;

			idisk = disk_list_get(dl->serial, disk_list);
			if (idisk && is_spare(&idisk->disk) &&
			    !is_failed(&idisk->disk) && !is_configured(&idisk->disk))
				dl->index = -1;
			else {
				dl->index = -2;
				continue;
			}
		}

		dl->next = champion->disks;
		champion->disks = dl;
		s->disks = NULL;
	}

	/* delete 'champion' from super_list */
	for (del = super_list; *del; ) {
		if (*del == champion) {
			*del = (*del)->next;
			break;
		} else
			del = &(*del)->next;
	}
	champion->next = NULL;

 out:
	while (disk_list) {
		struct intel_disk *idisk = disk_list;

		disk_list = disk_list->next;
		free(idisk);
	}

	return champion;
}

static int load_super_imsm_all(struct supertype *st, int fd, void **sbp,
			       char *devname)
{
	struct mdinfo *sra;
	struct intel_super *super_list = NULL;
	struct intel_super *super = NULL;
	int devnum = fd2devnum(fd);
	struct mdinfo *sd;
	int retry;
	int err = 0;
	int i;

	/* check if 'fd' an opened container */
	sra = sysfs_read(fd, 0, GET_LEVEL|GET_VERSION|GET_DEVS|GET_STATE);
	if (!sra)
		return 1;

	if (sra->array.major_version != -1 ||
	    sra->array.minor_version != -2 ||
	    strcmp(sra->text_version, "imsm") != 0) {
		err = 1;
		goto error;
	}
	/* load all mpbs */
	for (sd = sra->devs, i = 0; sd; sd = sd->next, i++) {
		struct intel_super *s = alloc_super();
		char nm[32];
		int dfd;
		int rv;

		err = 1;
		if (!s)
			goto error;
		s->next = super_list;
		super_list = s;

		err = 2;
		sprintf(nm, "%d:%d", sd->disk.major, sd->disk.minor);
		dfd = dev_open(nm, O_RDWR);
		if (dfd < 0)
			goto error;

		rv = find_intel_hba_capability(dfd, s, devname);
		/* no orom/efi or non-intel hba of the disk */
		if (rv != 0)
			goto error;

		err = load_and_parse_mpb(dfd, s, NULL, 1);

		/* retry the load if we might have raced against mdmon */
		if (err == 3 && mdmon_running(devnum))
			for (retry = 0; retry < 3; retry++) {
				usleep(3000);
				err = load_and_parse_mpb(dfd, s, NULL, 1);
				if (err != 3)
					break;
			}
		if (err)
			goto error;
	}

	/* all mpbs enter, maybe one leaves */
	super = imsm_thunderdome(&super_list, i);
	if (!super) {
		err = 1;
		goto error;
	}

	if (find_missing(super) != 0) {
		free_imsm(super);
		err = 2;
		goto error;
	}

	/* load migration record */
	err = load_imsm_migr_rec(super, NULL);
	if (err) {
		err = 4;
		goto error;
	}

	/* Check migration compatibility */
	if (check_mpb_migr_compatibility(super) != 0) {
		fprintf(stderr, Name ": Unsupported migration detected");
		if (devname)
			fprintf(stderr, " on %s\n", devname);
		else
			fprintf(stderr, " (IMSM).\n");

		err = 5;
		goto error;
	}

	err = 0;

 error:
	while (super_list) {
		struct intel_super *s = super_list;

		super_list = super_list->next;
		free_imsm(s);
	}
	sysfs_free(sra);

	if (err)
		return err;

	*sbp = super;
	st->container_dev = devnum;
	if (err == 0 && st->ss == NULL) {
		st->ss = &super_imsm;
		st->minor_version = 0;
		st->max_devs = IMSM_MAX_DEVICES;
	}
	return 0;
}

static int load_container_imsm(struct supertype *st, int fd, char *devname)
{
	return load_super_imsm_all(st, fd, &st->sb, devname);
}
#endif

static int load_super_imsm(struct supertype *st, int fd, char *devname)
{
	struct intel_super *super;
	int rv;

	if (test_partition(fd))
		/* IMSM not allowed on partitions */
		return 1;

	free_super_imsm(st);

	super = alloc_super();
	if (!super) {
		fprintf(stderr,
			Name ": malloc of %zu failed.\n",
			sizeof(*super));
		return 1;
	}
	/* Load hba and capabilities if they exist.
	 * But do not preclude loading metadata in case capabilities or hba are
	 * non-compliant and ignore_hw_compat is set.
	 */
	rv = find_intel_hba_capability(fd, super, devname);
	/* no orom/efi or non-intel hba of the disk */
	if ((rv != 0) && (st->ignore_hw_compat == 0)) {
		if (devname)
			fprintf(stderr,
				Name ": No OROM/EFI properties for %s\n", devname);
		free_imsm(super);
		return 2;
	}
	rv = load_and_parse_mpb(fd, super, devname, 0);

	if (rv) {
		if (devname)
			fprintf(stderr,
				Name ": Failed to load all information "
				"sections on %s\n", devname);
		free_imsm(super);
		return rv;
	}

	st->sb = super;
	if (st->ss == NULL) {
		st->ss = &super_imsm;
		st->minor_version = 0;
		st->max_devs = IMSM_MAX_DEVICES;
	}

	/* load migration record */
	if (load_imsm_migr_rec(super, NULL) == 0) {
		/* Check for unsupported migration features */
		if (check_mpb_migr_compatibility(super) != 0) {
			fprintf(stderr,
				Name ": Unsupported migration detected");
			if (devname)
				fprintf(stderr, " on %s\n", devname);
			else
				fprintf(stderr, " (IMSM).\n");
			return 3;
		}
	}

	return 0;
}

static __u16 info_to_blocks_per_strip(mdu_array_info_t *info)
{
	if (info->level == 1)
		return 128;
	return info->chunk_size >> 9;
}

static __u32 info_to_num_data_stripes(mdu_array_info_t *info, int num_domains)
{
	__u32 num_stripes;

	num_stripes = (info->size * 2) / info_to_blocks_per_strip(info);
	num_stripes /= num_domains;

	return num_stripes;
}

static __u32 info_to_blocks_per_member(mdu_array_info_t *info)
{
	if (info->level == 1)
		return info->size * 2;
	else
		return (info->size * 2) & ~(info_to_blocks_per_strip(info) - 1);
}

static void imsm_update_version_info(struct intel_super *super)
{
	/* update the version and attributes */
	struct imsm_super *mpb = super->anchor;
	char *version;
	struct imsm_dev *dev;
	struct imsm_map *map;
	int i;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		dev = get_imsm_dev(super, i);
		map = get_imsm_map(dev, 0);
		if (__le32_to_cpu(dev->size_high) > 0)
			mpb->attributes |= MPB_ATTRIB_2TB;

		/* FIXME detect when an array spans a port multiplier */
		#if 0
		mpb->attributes |= MPB_ATTRIB_PM;
		#endif

		if (mpb->num_raid_devs > 1 ||
		    mpb->attributes != MPB_ATTRIB_CHECKSUM_VERIFY) {
			version = MPB_VERSION_ATTRIBS;
			switch (get_imsm_raid_level(map)) {
			case 0: mpb->attributes |= MPB_ATTRIB_RAID0; break;
			case 1: mpb->attributes |= MPB_ATTRIB_RAID1; break;
			case 10: mpb->attributes |= MPB_ATTRIB_RAID10; break;
			case 5: mpb->attributes |= MPB_ATTRIB_RAID5; break;
			}
		} else {
			if (map->num_members >= 5)
				version = MPB_VERSION_5OR6_DISK_ARRAY;
			else if (dev->status == DEV_CLONE_N_GO)
				version = MPB_VERSION_CNG;
			else if (get_imsm_raid_level(map) == 5)
				version = MPB_VERSION_RAID5;
			else if (map->num_members >= 3)
				version = MPB_VERSION_3OR4_DISK_ARRAY;
			else if (get_imsm_raid_level(map) == 1)
				version = MPB_VERSION_RAID1;
			else
				version = MPB_VERSION_RAID0;
		}
		strcpy(((char *) mpb->sig) + strlen(MPB_SIGNATURE), version);
	}
}

static int check_name(struct intel_super *super, char *name, int quiet)
{
	struct imsm_super *mpb = super->anchor;
	char *reason = NULL;
	int i;

	if (strlen(name) > MAX_RAID_SERIAL_LEN)
		reason = "must be 16 characters or less";

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);

		if (strncmp((char *) dev->volume, name, MAX_RAID_SERIAL_LEN) == 0) {
			reason = "already exists";
			break;
		}
	}

	if (reason && !quiet)
		fprintf(stderr, Name ": imsm volume name %s\n", reason);

	return !reason;
}

static int init_super_imsm_volume(struct supertype *st, mdu_array_info_t *info,
				  unsigned long long size, char *name,
				  char *homehost, int *uuid)
{
	/* We are creating a volume inside a pre-existing container.
	 * so st->sb is already set.
	 */
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct intel_dev *dv;
	struct imsm_dev *dev;
	struct imsm_vol *vol;
	struct imsm_map *map;
	int idx = mpb->num_raid_devs;
	int i;
	unsigned long long array_blocks;
	size_t size_old, size_new;
	__u32 num_data_stripes;

	if (super->orom && mpb->num_raid_devs >= super->orom->vpa) {
		fprintf(stderr, Name": This imsm-container already has the "
			"maximum of %d volumes\n", super->orom->vpa);
		return 0;
	}

	/* ensure the mpb is large enough for the new data */
	size_old = __le32_to_cpu(mpb->mpb_size);
	size_new = disks_to_mpb_size(info->nr_disks);
	if (size_new > size_old) {
		void *mpb_new;
		size_t size_round = ROUND_UP(size_new, 512);

		if (posix_memalign(&mpb_new, 512, size_round) != 0) {
			fprintf(stderr, Name": could not allocate new mpb\n");
			return 0;
		}
		if (posix_memalign(&super->migr_rec_buf, 512, 512) != 0) {
			fprintf(stderr, Name
				": %s could not allocate migr_rec buffer\n",
				__func__);
			free(super->buf);
			free(super);
			return 0;
		}
		memcpy(mpb_new, mpb, size_old);
		free(mpb);
		mpb = mpb_new;
		super->anchor = mpb_new;
		mpb->mpb_size = __cpu_to_le32(size_new);
		memset(mpb_new + size_old, 0, size_round - size_old);
	}
	super->current_vol = idx;
	/* when creating the first raid device in this container set num_disks
	 * to zero, i.e. delete this spare and add raid member devices in
	 * add_to_super_imsm_volume()
	 */
	if (super->current_vol == 0)
		mpb->num_disks = 0;

	if (!check_name(super, name, 0))
		return 0;
	dv = malloc(sizeof(*dv));
	if (!dv) {
		fprintf(stderr, Name ": failed to allocate device list entry\n");
		return 0;
	}
	dev = calloc(1, sizeof(*dev) + sizeof(__u32) * (info->raid_disks - 1));
	if (!dev) {
		free(dv);
		fprintf(stderr, Name": could not allocate raid device\n");
		return 0;
	}

	strncpy((char *) dev->volume, name, MAX_RAID_SERIAL_LEN);
	if (info->level == 1)
		array_blocks = info_to_blocks_per_member(info);
	else
		array_blocks = calc_array_size(info->level, info->raid_disks,
					       info->layout, info->chunk_size,
					       info->size*2);
	/* round array size down to closest MB */
	array_blocks = (array_blocks >> SECT_PER_MB_SHIFT) << SECT_PER_MB_SHIFT;

	dev->size_low = __cpu_to_le32((__u32) array_blocks);
	dev->size_high = __cpu_to_le32((__u32) (array_blocks >> 32));
	dev->status = (DEV_READ_COALESCING | DEV_WRITE_COALESCING);
	vol = &dev->vol;
	vol->migr_state = 0;
	set_migr_type(dev, MIGR_INIT);
	vol->dirty = 0;
	vol->curr_migr_unit = 0;
	map = get_imsm_map(dev, 0);
	map->pba_of_lba0 = __cpu_to_le32(super->create_offset);
	map->blocks_per_member = __cpu_to_le32(info_to_blocks_per_member(info));
	map->blocks_per_strip = __cpu_to_le16(info_to_blocks_per_strip(info));
	map->failed_disk_num = ~0;
	map->map_state = info->level ? IMSM_T_STATE_UNINITIALIZED :
				       IMSM_T_STATE_NORMAL;
	map->ddf = 1;

	if (info->level == 1 && info->raid_disks > 2) {
		free(dev);
		free(dv);
		fprintf(stderr, Name": imsm does not support more than 2 disks"
				"in a raid1 volume\n");
		return 0;
	}

	map->raid_level = info->level;
	if (info->level == 10) {
		map->raid_level = 1;
		map->num_domains = info->raid_disks / 2;
	} else if (info->level == 1)
		map->num_domains = info->raid_disks;
	else
		map->num_domains = 1;

	num_data_stripes = info_to_num_data_stripes(info, map->num_domains);
	map->num_data_stripes = __cpu_to_le32(num_data_stripes);

	map->num_members = info->raid_disks;
	for (i = 0; i < map->num_members; i++) {
		/* initialized in add_to_super */
		set_imsm_ord_tbl_ent(map, i, IMSM_ORD_REBUILD);
	}
	mpb->num_raid_devs++;

	dv->dev = dev;
	dv->index = super->current_vol;
	dv->next = super->devlist;
	super->devlist = dv;

	imsm_update_version_info(super);

	return 1;
}

static int init_super_imsm(struct supertype *st, mdu_array_info_t *info,
			   unsigned long long size, char *name,
			   char *homehost, int *uuid)
{
	/* This is primarily called by Create when creating a new array.
	 * We will then get add_to_super called for each component, and then
	 * write_init_super called to write it out to each device.
	 * For IMSM, Create can create on fresh devices or on a pre-existing
	 * array.
	 * To create on a pre-existing array a different method will be called.
	 * This one is just for fresh drives.
	 */
	struct intel_super *super;
	struct imsm_super *mpb;
	size_t mpb_size;
	char *version;

	if (st->sb)
		return init_super_imsm_volume(st, info, size, name, homehost, uuid);

	if (info)
		mpb_size = disks_to_mpb_size(info->nr_disks);
	else
		mpb_size = 512;

	super = alloc_super();
	if (super && posix_memalign(&super->buf, 512, mpb_size) != 0) {
		free(super);
		super = NULL;
	}
	if (!super) {
		fprintf(stderr, Name
			": %s could not allocate superblock\n", __func__);
		return 0;
	}
	if (posix_memalign(&super->migr_rec_buf, 512, 512) != 0) {
		fprintf(stderr, Name
			": %s could not allocate migr_rec buffer\n", __func__);
		free(super->buf);
		free(super);
		return 0;
	}
	memset(super->buf, 0, mpb_size);
	mpb = super->buf;
	mpb->mpb_size = __cpu_to_le32(mpb_size);
	st->sb = super;

	if (info == NULL) {
		/* zeroing superblock */
		return 0;
	}

	mpb->attributes = MPB_ATTRIB_CHECKSUM_VERIFY;

	version = (char *) mpb->sig;
	strcpy(version, MPB_SIGNATURE);
	version += strlen(MPB_SIGNATURE);
	strcpy(version, MPB_VERSION_RAID0);

	return 1;
}

#ifndef MDASSEMBLE
static int add_to_super_imsm_volume(struct supertype *st, mdu_disk_info_t *dk,
				     int fd, char *devname)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct dl *dl;
	struct imsm_dev *dev;
	struct imsm_map *map;
	int slot;

	dev = get_imsm_dev(super, super->current_vol);
	map = get_imsm_map(dev, 0);

	if (! (dk->state & (1<<MD_DISK_SYNC))) {
		fprintf(stderr, Name ": %s: Cannot add spare devices to IMSM volume\n",
			devname);
		return 1;
	}

	if (fd == -1) {
		/* we're doing autolayout so grab the pre-marked (in
		 * validate_geometry) raid_disk
		 */
		for (dl = super->disks; dl; dl = dl->next)
			if (dl->raiddisk == dk->raid_disk)
				break;
	} else {
		for (dl = super->disks; dl ; dl = dl->next)
			if (dl->major == dk->major &&
			    dl->minor == dk->minor)
				break;
	}

	if (!dl) {
		fprintf(stderr, Name ": %s is not a member of the same container\n", devname);
		return 1;
	}

	/* add a pristine spare to the metadata */
	if (dl->index < 0) {
		dl->index = super->anchor->num_disks;
		super->anchor->num_disks++;
	}
	/* Check the device has not already been added */
	slot = get_imsm_disk_slot(map, dl->index);
	if (slot >= 0 &&
	    (get_imsm_ord_tbl_ent(dev, slot, -1) & IMSM_ORD_REBUILD) == 0) {
		fprintf(stderr, Name ": %s has been included in this array twice\n",
			devname);
		return 1;
	}
	set_imsm_ord_tbl_ent(map, dk->raid_disk, dl->index);
	dl->disk.status = CONFIGURED_DISK;

	/* if we are creating the first raid device update the family number */
	if (super->current_vol == 0) {
		__u32 sum;
		struct imsm_dev *_dev = __get_imsm_dev(mpb, 0);
		struct imsm_disk *_disk = __get_imsm_disk(mpb, dl->index);

		if (!_dev || !_disk) {
			fprintf(stderr, Name ": BUG mpb setup error\n");
			return 1;
		}
		*_dev = *dev;
		*_disk = dl->disk;
		sum = random32();
		sum += __gen_imsm_checksum(mpb);
		mpb->family_num = __cpu_to_le32(sum);
		mpb->orig_family_num = mpb->family_num;
	}
	super->current_disk = dl;
	return 0;
}


static int add_to_super_imsm(struct supertype *st, mdu_disk_info_t *dk,
			     int fd, char *devname)
{
	struct intel_super *super = st->sb;
	struct dl *dd;
	unsigned long long size;
	__u32 id;
	int rv;
	struct stat stb;

	/* If we are on an RAID enabled platform check that the disk is
	 * attached to the raid controller.
	 * We do not need to test disks attachment for container based additions,
	 * they shall be already tested when container was created/assembled.
	 */
	rv = find_intel_hba_capability(fd, super, devname);
	/* no orom/efi or non-intel hba of the disk */
	if (rv != 0) {
		dprintf("capability: %p fd: %d ret: %d\n",
			super->orom, fd, rv);
		return 1;
	}

	if (super->current_vol >= 0)
		return add_to_super_imsm_volume(st, dk, fd, devname);

	fstat(fd, &stb);
	dd = malloc(sizeof(*dd));
	if (!dd) {
		fprintf(stderr,
			Name ": malloc failed %s:%d.\n", __func__, __LINE__);
		return 1;
	}
	memset(dd, 0, sizeof(*dd));
	dd->major = major(stb.st_rdev);
	dd->minor = minor(stb.st_rdev);
	dd->index = -1;
	dd->devname = devname ? strdup(devname) : NULL;
	dd->fd = fd;
	dd->e = NULL;
	dd->action = DISK_ADD;
	rv = imsm_read_serial(fd, devname, dd->serial);
	if (rv) {
		fprintf(stderr,
			Name ": failed to retrieve scsi serial, aborting\n");
		free(dd);
		abort();
	}

	get_dev_size(fd, NULL, &size);
	size /= 512;
	serialcpy(dd->disk.serial, dd->serial);
	dd->disk.total_blocks = __cpu_to_le32(size);
	dd->disk.status = SPARE_DISK;
	if (sysfs_disk_to_scsi_id(fd, &id) == 0)
		dd->disk.scsi_id = __cpu_to_le32(id);
	else
		dd->disk.scsi_id = __cpu_to_le32(0);

	if (st->update_tail) {
		dd->next = super->disk_mgmt_list;
		super->disk_mgmt_list = dd;
	} else {
		dd->next = super->disks;
		super->disks = dd;
		super->updates_pending++;
	}

	return 0;
}


static int remove_from_super_imsm(struct supertype *st, mdu_disk_info_t *dk)
{
	struct intel_super *super = st->sb;
	struct dl *dd;

	/* remove from super works only in mdmon - for communication
	 * manager - monitor. Check if communication memory buffer
	 * is prepared.
	 */
	if (!st->update_tail) {
		fprintf(stderr,
			Name ": %s shall be used in mdmon context only"
			"(line %d).\n", __func__, __LINE__);
		return 1;
	}
	dd = malloc(sizeof(*dd));
	if (!dd) {
		fprintf(stderr,
			Name ": malloc failed %s:%d.\n", __func__, __LINE__);
		return 1;
	}
	memset(dd, 0, sizeof(*dd));
	dd->major = dk->major;
	dd->minor = dk->minor;
	dd->index = -1;
	dd->fd = -1;
	dd->disk.status = SPARE_DISK;
	dd->action = DISK_REMOVE;

	dd->next = super->disk_mgmt_list;
	super->disk_mgmt_list = dd;


	return 0;
}

static int store_imsm_mpb(int fd, struct imsm_super *mpb);

static union {
	char buf[512];
	struct imsm_super anchor;
} spare_record __attribute__ ((aligned(512)));

/* spare records have their own family number and do not have any defined raid
 * devices
 */
static int write_super_imsm_spares(struct intel_super *super, int doclose)
{
	struct imsm_super *mpb = super->anchor;
	struct imsm_super *spare = &spare_record.anchor;
	__u32 sum;
	struct dl *d;

	spare->mpb_size = __cpu_to_le32(sizeof(struct imsm_super)),
	spare->generation_num = __cpu_to_le32(1UL),
	spare->attributes = MPB_ATTRIB_CHECKSUM_VERIFY;
	spare->num_disks = 1,
	spare->num_raid_devs = 0,
	spare->cache_size = mpb->cache_size,
	spare->pwr_cycle_count = __cpu_to_le32(1),

	snprintf((char *) spare->sig, MAX_SIGNATURE_LENGTH,
		 MPB_SIGNATURE MPB_VERSION_RAID0);

	for (d = super->disks; d; d = d->next) {
		if (d->index != -1)
			continue;

		spare->disk[0] = d->disk;
		sum = __gen_imsm_checksum(spare);
		spare->family_num = __cpu_to_le32(sum);
		spare->orig_family_num = 0;
		sum = __gen_imsm_checksum(spare);
		spare->check_sum = __cpu_to_le32(sum);

		if (store_imsm_mpb(d->fd, spare)) {
			fprintf(stderr, "%s: failed for device %d:%d %s\n",
				__func__, d->major, d->minor, strerror(errno));
			return 1;
		}
		if (doclose) {
			close(d->fd);
			d->fd = -1;
		}
	}

	return 0;
}

static int write_super_imsm(struct supertype *st, int doclose)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct dl *d;
	__u32 generation;
	__u32 sum;
	int spares = 0;
	int i;
	__u32 mpb_size = sizeof(struct imsm_super) - sizeof(struct imsm_disk);
	int num_disks = 0;
	int clear_migration_record = 1;

	/* 'generation' is incremented everytime the metadata is written */
	generation = __le32_to_cpu(mpb->generation_num);
	generation++;
	mpb->generation_num = __cpu_to_le32(generation);

	/* fix up cases where previous mdadm releases failed to set
	 * orig_family_num
	 */
	if (mpb->orig_family_num == 0)
		mpb->orig_family_num = mpb->family_num;

	for (d = super->disks; d; d = d->next) {
		if (d->index == -1)
			spares++;
		else {
			mpb->disk[d->index] = d->disk;
			num_disks++;
		}
	}
	for (d = super->missing; d; d = d->next) {
		mpb->disk[d->index] = d->disk;
		num_disks++;
	}
	mpb->num_disks = num_disks;
	mpb_size += sizeof(struct imsm_disk) * mpb->num_disks;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev = __get_imsm_dev(mpb, i);
		struct imsm_dev *dev2 = get_imsm_dev(super, i);
		if (dev && dev2) {
			imsm_copy_dev(dev, dev2);
			mpb_size += sizeof_imsm_dev(dev, 0);
		}
		if (is_gen_migration(dev2))
			clear_migration_record = 0;
	}
	mpb_size += __le32_to_cpu(mpb->bbm_log_size);
	mpb->mpb_size = __cpu_to_le32(mpb_size);

	/* recalculate checksum */
	sum = __gen_imsm_checksum(mpb);
	mpb->check_sum = __cpu_to_le32(sum);

	if (clear_migration_record)
		memset(super->migr_rec_buf, 0, 512);

	/* write the mpb for disks that compose raid devices */
	for (d = super->disks; d ; d = d->next) {
		if (d->index < 0 || is_failed(&d->disk))
			continue;
		if (store_imsm_mpb(d->fd, mpb))
			fprintf(stderr, "%s: failed for device %d:%d %s\n",
				__func__, d->major, d->minor, strerror(errno));
		if (clear_migration_record) {
			unsigned long long dsize;

			get_dev_size(d->fd, NULL, &dsize);
			if (lseek64(d->fd, dsize - 512, SEEK_SET) >= 0) {
				if (write(d->fd, super->migr_rec_buf, 512) != 512)
					perror("Write migr_rec failed");
			}
		}
		if (doclose) {
			close(d->fd);
			d->fd = -1;
		}
	}

	if (spares)
		return write_super_imsm_spares(super, doclose);

	return 0;
}


static int create_array(struct supertype *st, int dev_idx)
{
	size_t len;
	struct imsm_update_create_array *u;
	struct intel_super *super = st->sb;
	struct imsm_dev *dev = get_imsm_dev(super, dev_idx);
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct disk_info *inf;
	struct imsm_disk *disk;
	int i;

	len = sizeof(*u) - sizeof(*dev) + sizeof_imsm_dev(dev, 0) +
	      sizeof(*inf) * map->num_members;
	u = malloc(len);
	if (!u) {
		fprintf(stderr, "%s: failed to allocate update buffer\n",
			__func__);
		return 1;
	}

	u->type = update_create_array;
	u->dev_idx = dev_idx;
	imsm_copy_dev(&u->dev, dev);
	inf = get_disk_info(u);
	for (i = 0; i < map->num_members; i++) {
		int idx = get_imsm_disk_idx(dev, i, -1);

		disk = get_imsm_disk(super, idx);
		serialcpy(inf[i].serial, disk->serial);
	}
	append_metadata_update(st, u, len);

	return 0;
}

static int mgmt_disk(struct supertype *st)
{
	struct intel_super *super = st->sb;
	size_t len;
	struct imsm_update_add_remove_disk *u;

	if (!super->disk_mgmt_list)
		return 0;

	len = sizeof(*u);
	u = malloc(len);
	if (!u) {
		fprintf(stderr, "%s: failed to allocate update buffer\n",
			__func__);
		return 1;
	}

	u->type = update_add_remove_disk;
	append_metadata_update(st, u, len);

	return 0;
}

static int write_init_super_imsm(struct supertype *st)
{
	struct intel_super *super = st->sb;
	int current_vol = super->current_vol;

	/* we are done with current_vol reset it to point st at the container */
	super->current_vol = -1;

	if (st->update_tail) {
		/* queue the recently created array / added disk
		 * as a metadata update */
		int rv;

		/* determine if we are creating a volume or adding a disk */
		if (current_vol < 0) {
			/* in the mgmt (add/remove) disk case we are running
			 * in mdmon context, so don't close fd's
			 */
			return mgmt_disk(st);
		} else
			rv = create_array(st, current_vol);

		return rv;
	} else {
		struct dl *d;
		for (d = super->disks; d; d = d->next)
			Kill(d->devname, NULL, 0, 1, 1);
		return write_super_imsm(st, 1);
	}
}
#endif

static int store_super_imsm(struct supertype *st, int fd)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super ? super->anchor : NULL;

	if (!mpb)
		return 1;

#ifndef MDASSEMBLE
	return store_imsm_mpb(fd, mpb);
#else
	return 1;
#endif
}

static int imsm_bbm_log_size(struct imsm_super *mpb)
{
	return __le32_to_cpu(mpb->bbm_log_size);
}

#ifndef MDASSEMBLE
static int validate_geometry_imsm_container(struct supertype *st, int level,
					    int layout, int raiddisks, int chunk,
					    unsigned long long size, char *dev,
					    unsigned long long *freesize,
					    int verbose)
{
	int fd;
	unsigned long long ldsize;
	struct intel_super *super=NULL;
	int rv = 0;

	if (level != LEVEL_CONTAINER)
		return 0;
	if (!dev)
		return 1;

	fd = open(dev, O_RDONLY|O_EXCL, 0);
	if (fd < 0) {
		if (verbose)
			fprintf(stderr, Name ": imsm: Cannot open %s: %s\n",
				dev, strerror(errno));
		return 0;
	}
	if (!get_dev_size(fd, dev, &ldsize)) {
		close(fd);
		return 0;
	}

	/* capabilities retrieve could be possible
	 * note that there is no fd for the disks in array.
	 */
	super = alloc_super();
	if (!super) {
		fprintf(stderr,
			Name ": malloc of %zu failed.\n",
			sizeof(*super));
		close(fd);
		return 0;
	}

	rv = find_intel_hba_capability(fd, super, verbose ? dev : NULL);
	if (rv != 0) {
#if DEBUG
		char str[256];
		fd2devname(fd, str);
		dprintf("validate_geometry_imsm_container: fd: %d %s orom: %p rv: %d raiddisk: %d\n",
			fd, str, super->orom, rv, raiddisks);
#endif
		/* no orom/efi or non-intel hba of the disk */
		close(fd);
		free_imsm(super);
		return 0;
	}
	close(fd);
	if (super->orom && raiddisks > super->orom->tds) {
		if (verbose)
			fprintf(stderr, Name ": %d exceeds maximum number of"
				" platform supported disks: %d\n",
				raiddisks, super->orom->tds);

		free_imsm(super);
		return 0;
	}

	*freesize = avail_size_imsm(st, ldsize >> 9);
	free_imsm(super);

	return 1;
}

static unsigned long long find_size(struct extent *e, int *idx, int num_extents)
{
	const unsigned long long base_start = e[*idx].start;
	unsigned long long end = base_start + e[*idx].size;
	int i;

	if (base_start == end)
		return 0;

	*idx = *idx + 1;
	for (i = *idx; i < num_extents; i++) {
		/* extend overlapping extents */
		if (e[i].start >= base_start &&
		    e[i].start <= end) {
			if (e[i].size == 0)
				return 0;
			if (e[i].start + e[i].size > end)
				end = e[i].start + e[i].size;
		} else if (e[i].start > end) {
			*idx = i;
			break;
		}
	}

	return end - base_start;
}

static unsigned long long merge_extents(struct intel_super *super, int sum_extents)
{
	/* build a composite disk with all known extents and generate a new
	 * 'maxsize' given the "all disks in an array must share a common start
	 * offset" constraint
	 */
	struct extent *e = calloc(sum_extents, sizeof(*e));
	struct dl *dl;
	int i, j;
	int start_extent;
	unsigned long long pos;
	unsigned long long start = 0;
	unsigned long long maxsize;
	unsigned long reserve;

	if (!e)
		return 0;

	/* coalesce and sort all extents. also, check to see if we need to
	 * reserve space between member arrays
	 */
	j = 0;
	for (dl = super->disks; dl; dl = dl->next) {
		if (!dl->e)
			continue;
		for (i = 0; i < dl->extent_cnt; i++)
			e[j++] = dl->e[i];
	}
	qsort(e, sum_extents, sizeof(*e), cmp_extent);

	/* merge extents */
	i = 0;
	j = 0;
	while (i < sum_extents) {
		e[j].start = e[i].start;
		e[j].size = find_size(e, &i, sum_extents);
		j++;
		if (e[j-1].size == 0)
			break;
	}

	pos = 0;
	maxsize = 0;
	start_extent = 0;
	i = 0;
	do {
		unsigned long long esize;

		esize = e[i].start - pos;
		if (esize >= maxsize) {
			maxsize = esize;
			start = pos;
			start_extent = i;
		}
		pos = e[i].start + e[i].size;
		i++;
	} while (e[i-1].size);
	free(e);

	if (maxsize == 0)
		return 0;

	/* FIXME assumes volume at offset 0 is the first volume in a
	 * container
	 */
	if (start_extent > 0)
		reserve = IMSM_RESERVED_SECTORS; /* gap between raid regions */
	else
		reserve = 0;

	if (maxsize < reserve)
		return 0;

	super->create_offset = ~((__u32) 0);
	if (start + reserve > super->create_offset)
		return 0; /* start overflows create_offset */
	super->create_offset = start + reserve;

	return maxsize - reserve;
}

static int is_raid_level_supported(const struct imsm_orom *orom, int level, int raiddisks)
{
	if (level < 0 || level == 6 || level == 4)
		return 0;

	/* if we have an orom prevent invalid raid levels */
	if (orom)
		switch (level) {
		case 0: return imsm_orom_has_raid0(orom);
		case 1:
			if (raiddisks > 2)
				return imsm_orom_has_raid1e(orom);
			return imsm_orom_has_raid1(orom) && raiddisks == 2;
		case 10: return imsm_orom_has_raid10(orom) && raiddisks == 4;
		case 5: return imsm_orom_has_raid5(orom) && raiddisks > 2;
		}
	else
		return 1; /* not on an Intel RAID platform so anything goes */

	return 0;
}

static int imsm_default_chunk(const struct imsm_orom *orom)
{
	/* up to 512 if the plaform supports it, otherwise the platform max.
	 * 128 if no platform detected
	 */
	int fs = max(7, orom ? fls(orom->sss) : 0);

	return min(512, (1 << fs));
}

#define pr_vrb(fmt, arg...) (void) (verbose && fprintf(stderr, Name fmt, ##arg))
static int
validate_geometry_imsm_orom(struct intel_super *super, int level, int layout,
			    int raiddisks, int *chunk, int verbose)
{
	/* check/set platform and metadata limits/defaults */
	if (super->orom && raiddisks > super->orom->dpa) {
		pr_vrb(": platform supports a maximum of %d disks per array\n",
		       super->orom->dpa);
		return 0;
	}

        /* capabilities of OROM tested - copied from validate_geometry_imsm_volume */
	if (!is_raid_level_supported(super->orom, level, raiddisks)) {
		pr_vrb(": platform does not support raid%d with %d disk%s\n",
			level, raiddisks, raiddisks > 1 ? "s" : "");
		return 0;
	}

	if (chunk && (*chunk == 0 || *chunk == UnSet))
		*chunk = imsm_default_chunk(super->orom);

	if (super->orom && chunk && !imsm_orom_has_chunk(super->orom, *chunk)) {
		pr_vrb(": platform does not support a chunk size of: "
		       "%d\n", *chunk);
		return 0;
	}

	if (layout != imsm_level_to_layout(level)) {
		if (level == 5)
			pr_vrb(": imsm raid 5 only supports the left-asymmetric layout\n");
		else if (level == 10)
			pr_vrb(": imsm raid 10 only supports the n2 layout\n");
		else
			pr_vrb(": imsm unknown layout %#x for this raid level %d\n",
				layout, level);
		return 0;
	}
	return 1;
}

/* validate_geometry_imsm_volume - lifted from validate_geometry_ddf_bvd 
 * FIX ME add ahci details
 */
static int validate_geometry_imsm_volume(struct supertype *st, int level,
					 int layout, int raiddisks, int *chunk,
					 unsigned long long size, char *dev,
					 unsigned long long *freesize,
					 int verbose)
{
	struct stat stb;
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct dl *dl;
	unsigned long long pos = 0;
	unsigned long long maxsize;
	struct extent *e;
	int i;

	/* We must have the container info already read in. */
	if (!super)
		return 0;

	if (!validate_geometry_imsm_orom(super, level, layout, raiddisks, chunk, verbose)) {
		fprintf(stderr, Name ": RAID gemetry validation failed. "
			"Cannot proceed with the action(s).\n");
		return 0;
	}
	if (!dev) {
		/* General test:  make sure there is space for
		 * 'raiddisks' device extents of size 'size' at a given
		 * offset
		 */
		unsigned long long minsize = size;
		unsigned long long start_offset = MaxSector;
		int dcnt = 0;
		if (minsize == 0)
			minsize = MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS;
		for (dl = super->disks; dl ; dl = dl->next) {
			int found = 0;

			pos = 0;
			i = 0;
			e = get_extents(super, dl);
			if (!e) continue;
			do {
				unsigned long long esize;
				esize = e[i].start - pos;
				if (esize >= minsize)
					found = 1;
				if (found && start_offset == MaxSector) {
					start_offset = pos;
					break;
				} else if (found && pos != start_offset) {
					found = 0;
					break;
				}
				pos = e[i].start + e[i].size;
				i++;
			} while (e[i-1].size);
			if (found)
				dcnt++;
			free(e);
		}
		if (dcnt < raiddisks) {
			if (verbose)
				fprintf(stderr, Name ": imsm: Not enough "
					"devices with space for this array "
					"(%d < %d)\n",
					dcnt, raiddisks);
			return 0;
		}
		return 1;
	}

	/* This device must be a member of the set */
	if (stat(dev, &stb) < 0)
		return 0;
	if ((S_IFMT & stb.st_mode) != S_IFBLK)
		return 0;
	for (dl = super->disks ; dl ; dl = dl->next) {
		if (dl->major == (int)major(stb.st_rdev) &&
		    dl->minor == (int)minor(stb.st_rdev))
			break;
	}
	if (!dl) {
		if (verbose)
			fprintf(stderr, Name ": %s is not in the "
				"same imsm set\n", dev);
		return 0;
	} else if (super->orom && dl->index < 0 && mpb->num_raid_devs) {
		/* If a volume is present then the current creation attempt
		 * cannot incorporate new spares because the orom may not
		 * understand this configuration (all member disks must be
		 * members of each array in the container).
		 */
		fprintf(stderr, Name ": %s is a spare and a volume"
			" is already defined for this container\n", dev);
		fprintf(stderr, Name ": The option-rom requires all member"
			" disks to be a member of all volumes\n");
		return 0;
	}

	/* retrieve the largest free space block */
	e = get_extents(super, dl);
	maxsize = 0;
	i = 0;
	if (e) {
		do {
			unsigned long long esize;

			esize = e[i].start - pos;
			if (esize >= maxsize)
				maxsize = esize;
			pos = e[i].start + e[i].size;
			i++;
		} while (e[i-1].size);
		dl->e = e;
		dl->extent_cnt = i;
	} else {
		if (verbose)
			fprintf(stderr, Name ": unable to determine free space for: %s\n",
				dev);
		return 0;
	}
	if (maxsize < size) {
		if (verbose)
			fprintf(stderr, Name ": %s not enough space (%llu < %llu)\n",
				dev, maxsize, size);
		return 0;
	}

	/* count total number of extents for merge */
	i = 0;
	for (dl = super->disks; dl; dl = dl->next)
		if (dl->e)
			i += dl->extent_cnt;

	maxsize = merge_extents(super, i);
	if (maxsize < size || maxsize == 0) {
		if (verbose)
			fprintf(stderr, Name ": not enough space after merge (%llu < %llu)\n",
				maxsize, size);
		return 0;
	}

	*freesize = maxsize;

	return 1;
}

static int reserve_space(struct supertype *st, int raiddisks,
			 unsigned long long size, int chunk,
			 unsigned long long *freesize)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct dl *dl;
	int i;
	int extent_cnt;
	struct extent *e;
	unsigned long long maxsize;
	unsigned long long minsize;
	int cnt;
	int used;

	/* find the largest common start free region of the possible disks */
	used = 0;
	extent_cnt = 0;
	cnt = 0;
	for (dl = super->disks; dl; dl = dl->next) {
		dl->raiddisk = -1;

		if (dl->index >= 0)
			used++;

		/* don't activate new spares if we are orom constrained
		 * and there is already a volume active in the container
		 */
		if (super->orom && dl->index < 0 && mpb->num_raid_devs)
			continue;

		e = get_extents(super, dl);
		if (!e)
			continue;
		for (i = 1; e[i-1].size; i++)
			;
		dl->e = e;
		dl->extent_cnt = i;
		extent_cnt += i;
		cnt++;
	}

	maxsize = merge_extents(super, extent_cnt);
	minsize = size;
	if (size == 0)
		/* chunk is in K */
		minsize = chunk * 2;

	if (cnt < raiddisks ||
	    (super->orom && used && used != raiddisks) ||
	    maxsize < minsize ||
	    maxsize == 0) {
		fprintf(stderr, Name ": not enough devices with space to create array.\n");
		return 0; /* No enough free spaces large enough */
	}

	if (size == 0) {
		size = maxsize;
		if (chunk) {
			size /= 2 * chunk;
			size *= 2 * chunk;
		}
	}

	cnt = 0;
	for (dl = super->disks; dl; dl = dl->next)
		if (dl->e)
			dl->raiddisk = cnt++;

	*freesize = size;

	return 1;
}

static int validate_geometry_imsm(struct supertype *st, int level, int layout,
				  int raiddisks, int *chunk, unsigned long long size,
				  char *dev, unsigned long long *freesize,
				  int verbose)
{
	int fd, cfd;
	struct mdinfo *sra;
	int is_member = 0;

	/* load capability
	 * if given unused devices create a container
	 * if given given devices in a container create a member volume
	 */
	if (level == LEVEL_CONTAINER) {
		/* Must be a fresh device to add to a container */
		return validate_geometry_imsm_container(st, level, layout,
							raiddisks,
							chunk?*chunk:0, size,
							dev, freesize,
							verbose);
	}
	
	if (!dev) {
		if (st->sb && freesize) {
			/* we are being asked to automatically layout a
			 * new volume based on the current contents of
			 * the container.  If the the parameters can be
			 * satisfied reserve_space will record the disks,
			 * start offset, and size of the volume to be
			 * created.  add_to_super and getinfo_super
			 * detect when autolayout is in progress.
			 */
			if (!validate_geometry_imsm_orom(st->sb, level, layout,
							 raiddisks, chunk,
							 verbose))
				return 0;
			return reserve_space(st, raiddisks, size,
					     chunk?*chunk:0, freesize);
		}
		return 1;
	}
	if (st->sb) {
		/* creating in a given container */
		return validate_geometry_imsm_volume(st, level, layout,
						     raiddisks, chunk, size,
						     dev, freesize, verbose);
	}

	/* This device needs to be a device in an 'imsm' container */
	fd = open(dev, O_RDONLY|O_EXCL, 0);
	if (fd >= 0) {
		if (verbose)
			fprintf(stderr,
				Name ": Cannot create this array on device %s\n",
				dev);
		close(fd);
		return 0;
	}
	if (errno != EBUSY || (fd = open(dev, O_RDONLY, 0)) < 0) {
		if (verbose)
			fprintf(stderr, Name ": Cannot open %s: %s\n",
				dev, strerror(errno));
		return 0;
	}
	/* Well, it is in use by someone, maybe an 'imsm' container. */
	cfd = open_container(fd);
	close(fd);
	if (cfd < 0) {
		if (verbose)
			fprintf(stderr, Name ": Cannot use %s: It is busy\n",
				dev);
		return 0;
	}
	sra = sysfs_read(cfd, 0, GET_VERSION);
	if (sra && sra->array.major_version == -1 &&
	    strcmp(sra->text_version, "imsm") == 0)
		is_member = 1;
	sysfs_free(sra);
	if (is_member) {
		/* This is a member of a imsm container.  Load the container
		 * and try to create a volume
		 */
		struct intel_super *super;

		if (load_super_imsm_all(st, cfd, (void **) &super, NULL) == 0) {
			st->sb = super;
			st->container_dev = fd2devnum(cfd);
			close(cfd);
			return validate_geometry_imsm_volume(st, level, layout,
							     raiddisks, chunk,
							     size, dev,
							     freesize, verbose);
		}
	}

	if (verbose)
		fprintf(stderr, Name ": failed container membership check\n");

	close(cfd);
	return 0;
}

static void default_geometry_imsm(struct supertype *st, int *level, int *layout, int *chunk)
{
	struct intel_super *super = st->sb;

	if (level && *level == UnSet)
		*level = LEVEL_CONTAINER;

	if (level && layout && *layout == UnSet)
		*layout = imsm_level_to_layout(*level);

	if (chunk && (*chunk == UnSet || *chunk == 0))
		*chunk = imsm_default_chunk(super->orom);
}

static void handle_missing(struct intel_super *super, struct imsm_dev *dev);

static int kill_subarray_imsm(struct supertype *st)
{
	/* remove the subarray currently referenced by ->current_vol */
	__u8 i;
	struct intel_dev **dp;
	struct intel_super *super = st->sb;
	__u8 current_vol = super->current_vol;
	struct imsm_super *mpb = super->anchor;

	if (super->current_vol < 0)
		return 2;
	super->current_vol = -1; /* invalidate subarray cursor */

	/* block deletions that would change the uuid of active subarrays
	 *
	 * FIXME when immutable ids are available, but note that we'll
	 * also need to fixup the invalidated/active subarray indexes in
	 * mdstat
	 */
	for (i = 0; i < mpb->num_raid_devs; i++) {
		char subarray[4];

		if (i < current_vol)
			continue;
		sprintf(subarray, "%u", i);
		if (is_subarray_active(subarray, st->devname)) {
			fprintf(stderr,
				Name ": deleting subarray-%d would change the UUID of active subarray-%d, aborting\n",
				current_vol, i);

			return 2;
		}
	}

	if (st->update_tail) {
		struct imsm_update_kill_array *u = malloc(sizeof(*u));

		if (!u)
			return 2;
		u->type = update_kill_array;
		u->dev_idx = current_vol;
		append_metadata_update(st, u, sizeof(*u));

		return 0;
	}

	for (dp = &super->devlist; *dp;)
		if ((*dp)->index == current_vol) {
			*dp = (*dp)->next;
		} else {
			handle_missing(super, (*dp)->dev);
			if ((*dp)->index > current_vol)
				(*dp)->index--;
			dp = &(*dp)->next;
		}

	/* no more raid devices, all active components are now spares,
	 * but of course failed are still failed
	 */
	if (--mpb->num_raid_devs == 0) {
		struct dl *d;

		for (d = super->disks; d; d = d->next)
			if (d->index > -2) {
				d->index = -1;
				d->disk.status = SPARE_DISK;
			}
	}

	super->updates_pending++;

	return 0;
}

static int update_subarray_imsm(struct supertype *st, char *subarray,
				char *update, struct mddev_ident *ident)
{
	/* update the subarray currently referenced by ->current_vol */
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;

	if (strcmp(update, "name") == 0) {
		char *name = ident->name;
		char *ep;
		int vol;

		if (is_subarray_active(subarray, st->devname)) {
			fprintf(stderr,
				Name ": Unable to update name of active subarray\n");
			return 2;
		}

		if (!check_name(super, name, 0))
			return 2;

		vol = strtoul(subarray, &ep, 10);
		if (*ep != '\0' || vol >= super->anchor->num_raid_devs)
			return 2;

		if (st->update_tail) {
			struct imsm_update_rename_array *u = malloc(sizeof(*u));

			if (!u)
				return 2;
			u->type = update_rename_array;
			u->dev_idx = vol;
			snprintf((char *) u->name, MAX_RAID_SERIAL_LEN, "%s", name);
			append_metadata_update(st, u, sizeof(*u));
		} else {
			struct imsm_dev *dev;
			int i;

			dev = get_imsm_dev(super, vol);
			snprintf((char *) dev->volume, MAX_RAID_SERIAL_LEN, "%s", name);
			for (i = 0; i < mpb->num_raid_devs; i++) {
				dev = get_imsm_dev(super, i);
				handle_missing(super, dev);
			}
			super->updates_pending++;
		}
	} else
		return 2;

	return 0;
}

static int is_gen_migration(struct imsm_dev *dev)
{
	if (dev == NULL)
		return 0;

	if (!dev->vol.migr_state)
		return 0;

	if (migr_type(dev) == MIGR_GEN_MIGR)
		return 1;

	return 0;
}
#endif /* MDASSEMBLE */

static int is_rebuilding(struct imsm_dev *dev)
{
	struct imsm_map *migr_map;

	if (!dev->vol.migr_state)
		return 0;

	if (migr_type(dev) != MIGR_REBUILD)
		return 0;

	migr_map = get_imsm_map(dev, 1);

	if (migr_map->map_state == IMSM_T_STATE_DEGRADED)
		return 1;
	else
		return 0;
}

static void update_recovery_start(struct intel_super *super,
					struct imsm_dev *dev,
					struct mdinfo *array)
{
	struct mdinfo *rebuild = NULL;
	struct mdinfo *d;
	__u32 units;

	if (!is_rebuilding(dev))
		return;

	/* Find the rebuild target, but punt on the dual rebuild case */
	for (d = array->devs; d; d = d->next)
		if (d->recovery_start == 0) {
			if (rebuild)
				return;
			rebuild = d;
		}

	if (!rebuild) {
		/* (?) none of the disks are marked with
		 * IMSM_ORD_REBUILD, so assume they are missing and the
		 * disk_ord_tbl was not correctly updated
		 */
		dprintf("%s: failed to locate out-of-sync disk\n", __func__);
		return;
	}

	units = __le32_to_cpu(dev->vol.curr_migr_unit);
	rebuild->recovery_start = units * blocks_per_migr_unit(super, dev);
}

#ifndef MDASSEMBLE
static int recover_backup_imsm(struct supertype *st, struct mdinfo *info);
#endif

static struct mdinfo *container_content_imsm(struct supertype *st, char *subarray)
{
	/* Given a container loaded by load_super_imsm_all,
	 * extract information about all the arrays into
	 * an mdinfo tree.
	 * If 'subarray' is given, just extract info about that array.
	 *
	 * For each imsm_dev create an mdinfo, fill it in,
	 *  then look for matching devices in super->disks
	 *  and create appropriate device mdinfo.
	 */
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct mdinfo *rest = NULL;
	unsigned int i;
	int bbm_errors = 0;
	struct dl *d;
	int spare_disks = 0;

	/* do not assemble arrays when not all attributes are supported */
	if (imsm_check_attributes(mpb->attributes) == 0) {
		fprintf(stderr, Name ": IMSM metadata loading not allowed "
			"due to attributes incompatibility.\n");
		return NULL;
	}

	/* check for bad blocks */
	if (imsm_bbm_log_size(super->anchor))
		bbm_errors = 1;

	/* count spare devices, not used in maps
	 */
	for (d = super->disks; d; d = d->next)
		if (d->index == -1)
			spare_disks++;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev;
		struct imsm_map *map;
		struct imsm_map *map2;
		struct mdinfo *this;
		int slot, chunk;
		char *ep;

		if (subarray &&
		    (i != strtoul(subarray, &ep, 10) || *ep != '\0'))
			continue;

		dev = get_imsm_dev(super, i);
		map = get_imsm_map(dev, 0);
		map2 = get_imsm_map(dev, 1);

		/* do not publish arrays that are in the middle of an
		 * unsupported migration
		 */
		if (dev->vol.migr_state &&
		    (migr_type(dev) == MIGR_STATE_CHANGE)) {
			fprintf(stderr, Name ": cannot assemble volume '%.16s':"
				" unsupported migration in progress\n",
				dev->volume);
			continue;
		}
		/* do not publish arrays that are not support by controller's
		 * OROM/EFI
		 */

		chunk = __le16_to_cpu(map->blocks_per_strip) >> 1;
#ifndef MDASSEMBLE
		if (!validate_geometry_imsm_orom(super,
						 get_imsm_raid_level(map), /* RAID level */
						 imsm_level_to_layout(get_imsm_raid_level(map)),
						 map->num_members, /* raid disks */
						 &chunk,
						 1 /* verbose */)) {
			fprintf(stderr, Name ": RAID gemetry validation failed. "
				"Cannot proceed with the action(s).\n");
			continue;
		}
#endif /* MDASSEMBLE */
		this = malloc(sizeof(*this));
		if (!this) {
			fprintf(stderr, Name ": failed to allocate %zu bytes\n",
				sizeof(*this));
			break;
		}

		super->current_vol = i;
		getinfo_super_imsm_volume(st, this, NULL);
		this->next = rest;
		for (slot = 0 ; slot <  map->num_members; slot++) {
			unsigned long long recovery_start;
			struct mdinfo *info_d;
			struct dl *d;
			int idx;
			int skip;
			__u32 ord;

			skip = 0;
			idx = get_imsm_disk_idx(dev, slot, 0);
			ord = get_imsm_ord_tbl_ent(dev, slot, -1);
			for (d = super->disks; d ; d = d->next)
				if (d->index == idx)
					break;

			recovery_start = MaxSector;
			if (d == NULL)
				skip = 1;
			if (d && is_failed(&d->disk))
				skip = 1;
			if (ord & IMSM_ORD_REBUILD)
				recovery_start = 0;

			/* 
			 * if we skip some disks the array will be assmebled degraded;
			 * reset resync start to avoid a dirty-degraded
			 * situation when performing the intial sync
			 *
			 * FIXME handle dirty degraded
			 */
			if ((skip || recovery_start == 0) && !dev->vol.dirty)
				this->resync_start = MaxSector;
			if (skip)
				continue;

			info_d = calloc(1, sizeof(*info_d));
			if (!info_d) {
				fprintf(stderr, Name ": failed to allocate disk"
					" for volume %.16s\n", dev->volume);
				info_d = this->devs;
				while (info_d) {
					struct mdinfo *d = info_d->next;

					free(info_d);
					info_d = d;
				}
				free(this);
				this = rest;
				break;
			}
			info_d->next = this->devs;
			this->devs = info_d;

			info_d->disk.number = d->index;
			info_d->disk.major = d->major;
			info_d->disk.minor = d->minor;
			info_d->disk.raid_disk = slot;
			info_d->recovery_start = recovery_start;
			if (map2) {
				if (slot < map2->num_members)
					info_d->disk.state = (1 << MD_DISK_ACTIVE);
				else
					this->array.spare_disks++;
			} else {
				if (slot < map->num_members)
					info_d->disk.state = (1 << MD_DISK_ACTIVE);
				else
					this->array.spare_disks++;
			}
			if (info_d->recovery_start == MaxSector)
				this->array.working_disks++;

			info_d->events = __le32_to_cpu(mpb->generation_num);
			info_d->data_offset = __le32_to_cpu(map->pba_of_lba0);
			info_d->component_size = __le32_to_cpu(map->blocks_per_member);
		}
		/* now that the disk list is up-to-date fixup recovery_start */
		update_recovery_start(super, dev, this);
		this->array.spare_disks += spare_disks;

#ifndef MDASSEMBLE
		/* check for reshape */
		if (this->reshape_active == 1)
			recover_backup_imsm(st, this);
#endif
		rest = this;
	}

	/* if array has bad blocks, set suitable bit in array status */
	if (bbm_errors)
		rest->array.state |= (1<<MD_SB_BBM_ERRORS);

	return rest;
}


static __u8 imsm_check_degraded(struct intel_super *super, struct imsm_dev *dev, int failed)
{
	struct imsm_map *map = get_imsm_map(dev, 0);

	if (!failed)
		return map->map_state == IMSM_T_STATE_UNINITIALIZED ? 
			IMSM_T_STATE_UNINITIALIZED : IMSM_T_STATE_NORMAL;

	switch (get_imsm_raid_level(map)) {
	case 0:
		return IMSM_T_STATE_FAILED;
		break;
	case 1:
		if (failed < map->num_members)
			return IMSM_T_STATE_DEGRADED;
		else
			return IMSM_T_STATE_FAILED;
		break;
	case 10:
	{
		/**
		 * check to see if any mirrors have failed, otherwise we
		 * are degraded.  Even numbered slots are mirrored on
		 * slot+1
		 */
		int i;
		/* gcc -Os complains that this is unused */
		int insync = insync;

		for (i = 0; i < map->num_members; i++) {
			__u32 ord = get_imsm_ord_tbl_ent(dev, i, -1);
			int idx = ord_to_idx(ord);
			struct imsm_disk *disk;

			/* reset the potential in-sync count on even-numbered
			 * slots.  num_copies is always 2 for imsm raid10 
			 */
			if ((i & 1) == 0)
				insync = 2;

			disk = get_imsm_disk(super, idx);
			if (!disk || is_failed(disk) || ord & IMSM_ORD_REBUILD)
				insync--;

			/* no in-sync disks left in this mirror the
			 * array has failed
			 */
			if (insync == 0)
				return IMSM_T_STATE_FAILED;
		}

		return IMSM_T_STATE_DEGRADED;
	}
	case 5:
		if (failed < 2)
			return IMSM_T_STATE_DEGRADED;
		else
			return IMSM_T_STATE_FAILED;
		break;
	default:
		break;
	}

	return map->map_state;
}

static int imsm_count_failed(struct intel_super *super, struct imsm_dev *dev)
{
	int i;
	int failed = 0;
	struct imsm_disk *disk;
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct imsm_map *prev = get_imsm_map(dev, dev->vol.migr_state);
	__u32 ord;
	int idx;

	/* at the beginning of migration we set IMSM_ORD_REBUILD on
	 * disks that are being rebuilt.  New failures are recorded to
	 * map[0].  So we look through all the disks we started with and
	 * see if any failures are still present, or if any new ones
	 * have arrived
	 *
	 * FIXME add support for online capacity expansion and
	 * raid-level-migration
	 */
	for (i = 0; i < prev->num_members; i++) {
		ord = __le32_to_cpu(prev->disk_ord_tbl[i]);
		ord |= __le32_to_cpu(map->disk_ord_tbl[i]);
		idx = ord_to_idx(ord);

		disk = get_imsm_disk(super, idx);
		if (!disk || is_failed(disk) || ord & IMSM_ORD_REBUILD)
			failed++;
	}

	return failed;
}

#ifndef MDASSEMBLE
static int imsm_open_new(struct supertype *c, struct active_array *a,
			 char *inst)
{
	struct intel_super *super = c->sb;
	struct imsm_super *mpb = super->anchor;
	
	if (atoi(inst) >= mpb->num_raid_devs) {
		fprintf(stderr, "%s: subarry index %d, out of range\n",
			__func__, atoi(inst));
		return -ENODEV;
	}

	dprintf("imsm: open_new %s\n", inst);
	a->info.container_member = atoi(inst);
	return 0;
}

static int is_resyncing(struct imsm_dev *dev)
{
	struct imsm_map *migr_map;

	if (!dev->vol.migr_state)
		return 0;

	if (migr_type(dev) == MIGR_INIT ||
	    migr_type(dev) == MIGR_REPAIR)
		return 1;

	if (migr_type(dev) == MIGR_GEN_MIGR)
		return 0;

	migr_map = get_imsm_map(dev, 1);

	if ((migr_map->map_state == IMSM_T_STATE_NORMAL) &&
	    (dev->vol.migr_type != MIGR_GEN_MIGR))
		return 1;
	else
		return 0;
}

/* return true if we recorded new information */
static int mark_failure(struct imsm_dev *dev, struct imsm_disk *disk, int idx)
{
	__u32 ord;
	int slot;
	struct imsm_map *map;
	char buf[MAX_RAID_SERIAL_LEN+3];
	unsigned int len, shift = 0;

	/* new failures are always set in map[0] */
	map = get_imsm_map(dev, 0);

	slot = get_imsm_disk_slot(map, idx);
	if (slot < 0)
		return 0;

	ord = __le32_to_cpu(map->disk_ord_tbl[slot]);
	if (is_failed(disk) && (ord & IMSM_ORD_REBUILD))
		return 0;

	sprintf(buf, "%s:0", disk->serial);
	if ((len = strlen(buf)) >= MAX_RAID_SERIAL_LEN)
		shift = len - MAX_RAID_SERIAL_LEN + 1;
	strncpy((char *)disk->serial, &buf[shift], MAX_RAID_SERIAL_LEN);

	disk->status |= FAILED_DISK;
	set_imsm_ord_tbl_ent(map, slot, idx | IMSM_ORD_REBUILD);
	if (map->failed_disk_num == 0xff)
		map->failed_disk_num = slot;
	return 1;
}

static void mark_missing(struct imsm_dev *dev, struct imsm_disk *disk, int idx)
{
	mark_failure(dev, disk, idx);

	if (disk->scsi_id == __cpu_to_le32(~(__u32)0))
		return;

	disk->scsi_id = __cpu_to_le32(~(__u32)0);
	memmove(&disk->serial[0], &disk->serial[1], MAX_RAID_SERIAL_LEN - 1);
}

static void handle_missing(struct intel_super *super, struct imsm_dev *dev)
{
	__u8 map_state;
	struct dl *dl;
	int failed;

	if (!super->missing)
		return;
	failed = imsm_count_failed(super, dev);
	map_state = imsm_check_degraded(super, dev, failed);

	dprintf("imsm: mark missing\n");
	end_migration(dev, map_state);
	for (dl = super->missing; dl; dl = dl->next)
		mark_missing(dev, &dl->disk, dl->index);
	super->updates_pending++;
}

static unsigned long long imsm_set_array_size(struct imsm_dev *dev)
{
	int used_disks = imsm_num_data_members(dev, 0);
	unsigned long long array_blocks;
	struct imsm_map *map;

	if (used_disks == 0) {
		/* when problems occures
		 * return current array_blocks value
		 */
		array_blocks = __le32_to_cpu(dev->size_high);
		array_blocks = array_blocks << 32;
		array_blocks += __le32_to_cpu(dev->size_low);

		return array_blocks;
	}

	/* set array size in metadata
	 */
	map = get_imsm_map(dev, 0);
	array_blocks = map->blocks_per_member * used_disks;

	/* round array size down to closest MB
	 */
	array_blocks = (array_blocks >> SECT_PER_MB_SHIFT) << SECT_PER_MB_SHIFT;
	dev->size_low = __cpu_to_le32((__u32)array_blocks);
	dev->size_high = __cpu_to_le32((__u32)(array_blocks >> 32));

	return array_blocks;
}

static void imsm_set_disk(struct active_array *a, int n, int state);

static void imsm_progress_container_reshape(struct intel_super *super)
{
	/* if no device has a migr_state, but some device has a
	 * different number of members than the previous device, start
	 * changing the number of devices in this device to match
	 * previous.
	 */
	struct imsm_super *mpb = super->anchor;
	int prev_disks = -1;
	int i;
	int copy_map_size;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);
		struct imsm_map *map = get_imsm_map(dev, 0);
		struct imsm_map *map2;
		int prev_num_members;

		if (dev->vol.migr_state)
			return;

		if (prev_disks == -1)
			prev_disks = map->num_members;
		if (prev_disks == map->num_members)
			continue;

		/* OK, this array needs to enter reshape mode.
		 * i.e it needs a migr_state
		 */

		copy_map_size = sizeof_imsm_map(map);
		prev_num_members = map->num_members;
		map->num_members = prev_disks;
		dev->vol.migr_state = 1;
		dev->vol.curr_migr_unit = 0;
		dev->vol.migr_type = MIGR_GEN_MIGR;
		for (i = prev_num_members;
		     i < map->num_members; i++)
			set_imsm_ord_tbl_ent(map, i, i);
		map2 = get_imsm_map(dev, 1);
		/* Copy the current map */
		memcpy(map2, map, copy_map_size);
		map2->num_members = prev_num_members;

		imsm_set_array_size(dev);
		super->updates_pending++;
	}
}

/* Handle dirty -> clean transititions, resync and reshape.  Degraded and rebuild
 * states are handled in imsm_set_disk() with one exception, when a
 * resync is stopped due to a new failure this routine will set the
 * 'degraded' state for the array.
 */
static int imsm_set_array_state(struct active_array *a, int consistent)
{
	int inst = a->info.container_member;
	struct intel_super *super = a->container->sb;
	struct imsm_dev *dev = get_imsm_dev(super, inst);
	struct imsm_map *map = get_imsm_map(dev, 0);
	int failed = imsm_count_failed(super, dev);
	__u8 map_state = imsm_check_degraded(super, dev, failed);
	__u32 blocks_per_unit;

	if (dev->vol.migr_state &&
	    dev->vol.migr_type  == MIGR_GEN_MIGR) {
		/* array state change is blocked due to reshape action
		 * We might need to
		 * - abort the reshape (if last_checkpoint is 0 and action!= reshape)
		 * - finish the reshape (if last_checkpoint is big and action != reshape)
		 * - update curr_migr_unit
		 */
		if (a->curr_action == reshape) {
			/* still reshaping, maybe update curr_migr_unit */
			goto mark_checkpoint;
		} else {
			if (a->last_checkpoint == 0 && a->prev_action == reshape) {
				/* for some reason we aborted the reshape.
				 *
				 * disable automatic metadata rollback
				 * user action is required to recover process
				 */
				if (0) {
				struct imsm_map *map2 = get_imsm_map(dev, 1);
				dev->vol.migr_state = 0;
				dev->vol.migr_type = 0;
				dev->vol.curr_migr_unit = 0;
				memcpy(map, map2, sizeof_imsm_map(map2));
				super->updates_pending++;
				}
			}
			if (a->last_checkpoint >= a->info.component_size) {
				unsigned long long array_blocks;
				int used_disks;
				struct mdinfo *mdi;

				used_disks = imsm_num_data_members(dev, 0);
				if (used_disks > 0) {
					array_blocks =
						map->blocks_per_member *
						used_disks;
					/* round array size down to closest MB
					 */
					array_blocks = (array_blocks
							>> SECT_PER_MB_SHIFT)
						<< SECT_PER_MB_SHIFT;
					a->info.custom_array_size = array_blocks;
					/* encourage manager to update array
					 * size
					 */

					a->check_reshape = 1;
				}
				/* finalize online capacity expansion/reshape */
				for (mdi = a->info.devs; mdi; mdi = mdi->next)
					imsm_set_disk(a,
						      mdi->disk.raid_disk,
						      mdi->curr_state);

				imsm_progress_container_reshape(super);
			}
		}
	}

	/* before we activate this array handle any missing disks */
	if (consistent == 2)
		handle_missing(super, dev);

	if (consistent == 2 &&
	    (!is_resync_complete(&a->info) ||
	     map_state != IMSM_T_STATE_NORMAL ||
	     dev->vol.migr_state))
		consistent = 0;

	if (is_resync_complete(&a->info)) {
		/* complete intialization / resync,
		 * recovery and interrupted recovery is completed in
		 * ->set_disk
		 */
		if (is_resyncing(dev)) {
			dprintf("imsm: mark resync done\n");
			end_migration(dev, map_state);
			super->updates_pending++;
			a->last_checkpoint = 0;
		}
	} else if (!is_resyncing(dev) && !failed) {
		/* mark the start of the init process if nothing is failed */
		dprintf("imsm: mark resync start\n");
		if (map->map_state == IMSM_T_STATE_UNINITIALIZED)
			migrate(dev, super, IMSM_T_STATE_NORMAL, MIGR_INIT);
		else
			migrate(dev, super, IMSM_T_STATE_NORMAL, MIGR_REPAIR);
		super->updates_pending++;
	}

mark_checkpoint:
	/* skip checkpointing for general migration,
	 * it is controlled in mdadm
	 */
	if (is_gen_migration(dev))
		goto skip_mark_checkpoint;

	/* check if we can update curr_migr_unit from resync_start, recovery_start */
	blocks_per_unit = blocks_per_migr_unit(super, dev);
	if (blocks_per_unit) {
		__u32 units32;
		__u64 units;

		units = a->last_checkpoint / blocks_per_unit;
		units32 = units;

		/* check that we did not overflow 32-bits, and that
		 * curr_migr_unit needs updating
		 */
		if (units32 == units &&
		    units32 != 0 &&
		    __le32_to_cpu(dev->vol.curr_migr_unit) != units32) {
			dprintf("imsm: mark checkpoint (%u)\n", units32);
			dev->vol.curr_migr_unit = __cpu_to_le32(units32);
			super->updates_pending++;
		}
	}

skip_mark_checkpoint:
	/* mark dirty / clean */
	if (dev->vol.dirty != !consistent) {
		dprintf("imsm: mark '%s'\n", consistent ? "clean" : "dirty");
		if (consistent)
			dev->vol.dirty = 0;
		else
			dev->vol.dirty = 1;
		super->updates_pending++;
	}

	return consistent;
}

static void imsm_set_disk(struct active_array *a, int n, int state)
{
	int inst = a->info.container_member;
	struct intel_super *super = a->container->sb;
	struct imsm_dev *dev = get_imsm_dev(super, inst);
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct imsm_disk *disk;
	int failed;
	__u32 ord;
	__u8 map_state;

	if (n > map->num_members)
		fprintf(stderr, "imsm: set_disk %d out of range 0..%d\n",
			n, map->num_members - 1);

	if (n < 0)
		return;

	dprintf("imsm: set_disk %d:%x\n", n, state);

	ord = get_imsm_ord_tbl_ent(dev, n, -1);
	disk = get_imsm_disk(super, ord_to_idx(ord));

	/* check for new failures */
	if (state & DS_FAULTY) {
		if (mark_failure(dev, disk, ord_to_idx(ord)))
			super->updates_pending++;
	}

	/* check if in_sync */
	if (state & DS_INSYNC && ord & IMSM_ORD_REBUILD && is_rebuilding(dev)) {
		struct imsm_map *migr_map = get_imsm_map(dev, 1);

		set_imsm_ord_tbl_ent(migr_map, n, ord_to_idx(ord));
		super->updates_pending++;
	}

	failed = imsm_count_failed(super, dev);
	map_state = imsm_check_degraded(super, dev, failed);

	/* check if recovery complete, newly degraded, or failed */
	if (map_state == IMSM_T_STATE_NORMAL && is_rebuilding(dev)) {
		end_migration(dev, map_state);
		map = get_imsm_map(dev, 0);
		map->failed_disk_num = ~0;
		super->updates_pending++;
		a->last_checkpoint = 0;
	} else if (map_state == IMSM_T_STATE_DEGRADED &&
		   map->map_state != map_state &&
		   !dev->vol.migr_state) {
		dprintf("imsm: mark degraded\n");
		map->map_state = map_state;
		super->updates_pending++;
		a->last_checkpoint = 0;
	} else if (map_state == IMSM_T_STATE_FAILED &&
		   map->map_state != map_state) {
		dprintf("imsm: mark failed\n");
		end_migration(dev, map_state);
		super->updates_pending++;
		a->last_checkpoint = 0;
	} else if (is_gen_migration(dev)) {
		dprintf("imsm: Detected General Migration in state: ");
		if (map_state == IMSM_T_STATE_NORMAL) {
			end_migration(dev, map_state);
			map = get_imsm_map(dev, 0);
			map->failed_disk_num = ~0;
			dprintf("normal\n");
		} else {
			if (map_state == IMSM_T_STATE_DEGRADED) {
				printf("degraded\n");
				end_migration(dev, map_state);
			} else {
				dprintf("failed\n");
			}
			map->map_state = map_state;
		}
		super->updates_pending++;
	}
}

static int store_imsm_mpb(int fd, struct imsm_super *mpb)
{
	void *buf = mpb;
	__u32 mpb_size = __le32_to_cpu(mpb->mpb_size);
	unsigned long long dsize;
	unsigned long long sectors;

	get_dev_size(fd, NULL, &dsize);

	if (mpb_size > 512) {
		/* -1 to account for anchor */
		sectors = mpb_sectors(mpb) - 1;

		/* write the extended mpb to the sectors preceeding the anchor */
		if (lseek64(fd, dsize - (512 * (2 + sectors)), SEEK_SET) < 0)
			return 1;

		if ((unsigned long long)write(fd, buf + 512, 512 * sectors)
		    != 512 * sectors)
			return 1;
	}

	/* first block is stored on second to last sector of the disk */
	if (lseek64(fd, dsize - (512 * 2), SEEK_SET) < 0)
		return 1;

	if (write(fd, buf, 512) != 512)
		return 1;

	return 0;
}

static void imsm_sync_metadata(struct supertype *container)
{
	struct intel_super *super = container->sb;

	dprintf("sync metadata: %d\n", super->updates_pending);
	if (!super->updates_pending)
		return;

	write_super_imsm(container, 0);

	super->updates_pending = 0;
}

static struct dl *imsm_readd(struct intel_super *super, int idx, struct active_array *a)
{
	struct imsm_dev *dev = get_imsm_dev(super, a->info.container_member);
	int i = get_imsm_disk_idx(dev, idx, -1);
	struct dl *dl;

	for (dl = super->disks; dl; dl = dl->next)
		if (dl->index == i)
			break;

	if (dl && is_failed(&dl->disk))
		dl = NULL;

	if (dl)
		dprintf("%s: found %x:%x\n", __func__, dl->major, dl->minor);

	return dl;
}

static struct dl *imsm_add_spare(struct intel_super *super, int slot,
				 struct active_array *a, int activate_new,
				 struct mdinfo *additional_test_list)
{
	struct imsm_dev *dev = get_imsm_dev(super, a->info.container_member);
	int idx = get_imsm_disk_idx(dev, slot, -1);
	struct imsm_super *mpb = super->anchor;
	struct imsm_map *map;
	unsigned long long pos;
	struct mdinfo *d;
	struct extent *ex;
	int i, j;
	int found;
	__u32 array_start = 0;
	__u32 array_end = 0;
	struct dl *dl;
	struct mdinfo *test_list;

	for (dl = super->disks; dl; dl = dl->next) {
		/* If in this array, skip */
		for (d = a->info.devs ; d ; d = d->next)
			if (d->state_fd >= 0 &&
			    d->disk.major == dl->major &&
			    d->disk.minor == dl->minor) {
				dprintf("%x:%x already in array\n",
					dl->major, dl->minor);
				break;
			}
		if (d)
			continue;
		test_list = additional_test_list;
		while (test_list) {
			if (test_list->disk.major == dl->major &&
			    test_list->disk.minor == dl->minor) {
				dprintf("%x:%x already in additional test list\n",
					dl->major, dl->minor);
				break;
			}
			test_list = test_list->next;
		}
		if (test_list)
			continue;

		/* skip in use or failed drives */
		if (is_failed(&dl->disk) || idx == dl->index ||
		    dl->index == -2) {
			dprintf("%x:%x status (failed: %d index: %d)\n",
				dl->major, dl->minor, is_failed(&dl->disk), idx);
			continue;
		}

		/* skip pure spares when we are looking for partially
		 * assimilated drives
		 */
		if (dl->index == -1 && !activate_new)
			continue;

		/* Does this unused device have the requisite free space?
		 * It needs to be able to cover all member volumes
		 */
		ex = get_extents(super, dl);
		if (!ex) {
			dprintf("cannot get extents\n");
			continue;
		}
		for (i = 0; i < mpb->num_raid_devs; i++) {
			dev = get_imsm_dev(super, i);
			map = get_imsm_map(dev, 0);

			/* check if this disk is already a member of
			 * this array
			 */
			if (get_imsm_disk_slot(map, dl->index) >= 0)
				continue;

			found = 0;
			j = 0;
			pos = 0;
			array_start = __le32_to_cpu(map->pba_of_lba0);
			array_end = array_start +
				    __le32_to_cpu(map->blocks_per_member) - 1;

			do {
				/* check that we can start at pba_of_lba0 with
				 * blocks_per_member of space
				 */
				if (array_start >= pos && array_end < ex[j].start) {
					found = 1;
					break;
				}
				pos = ex[j].start + ex[j].size;
				j++;
			} while (ex[j-1].size);

			if (!found)
				break;
		}

		free(ex);
		if (i < mpb->num_raid_devs) {
			dprintf("%x:%x does not have %u to %u available\n",
				dl->major, dl->minor, array_start, array_end);
			/* No room */
			continue;
		}
		return dl;
	}

	return dl;
}


static int imsm_rebuild_allowed(struct supertype *cont, int dev_idx, int failed)
{
	struct imsm_dev *dev2;
	struct imsm_map *map;
	struct dl *idisk;
	int slot;
	int idx;
	__u8 state;

	dev2 = get_imsm_dev(cont->sb, dev_idx);
	if (dev2) {
		state = imsm_check_degraded(cont->sb, dev2, failed);
		if (state == IMSM_T_STATE_FAILED) {
			map = get_imsm_map(dev2, 0);
			if (!map)
				return 1;
			for (slot = 0; slot < map->num_members; slot++) {
				/*
				 * Check if failed disks are deleted from intel
				 * disk list or are marked to be deleted
				 */
				idx = get_imsm_disk_idx(dev2, slot, -1);
				idisk = get_imsm_dl_disk(cont->sb, idx);
				/*
				 * Do not rebuild the array if failed disks
				 * from failed sub-array are not removed from
				 * container.
				 */
				if (idisk &&
				    is_failed(&idisk->disk) &&
				    (idisk->action != DISK_REMOVE))
					return 0;
			}
		}
	}
	return 1;
}

static struct mdinfo *imsm_activate_spare(struct active_array *a,
					  struct metadata_update **updates)
{
	/**
	 * Find a device with unused free space and use it to replace a
	 * failed/vacant region in an array.  We replace failed regions one a
	 * array at a time.  The result is that a new spare disk will be added
	 * to the first failed array and after the monitor has finished
	 * propagating failures the remainder will be consumed.
	 *
	 * FIXME add a capability for mdmon to request spares from another
	 * container.
	 */

	struct intel_super *super = a->container->sb;
	int inst = a->info.container_member;
	struct imsm_dev *dev = get_imsm_dev(super, inst);
	struct imsm_map *map = get_imsm_map(dev, 0);
	int failed = a->info.array.raid_disks;
	struct mdinfo *rv = NULL;
	struct mdinfo *d;
	struct mdinfo *di;
	struct metadata_update *mu;
	struct dl *dl;
	struct imsm_update_activate_spare *u;
	int num_spares = 0;
	int i;
	int allowed;

	for (d = a->info.devs ; d ; d = d->next) {
		if ((d->curr_state & DS_FAULTY) &&
			d->state_fd >= 0)
			/* wait for Removal to happen */
			return NULL;
		if (d->state_fd >= 0)
			failed--;
	}

	dprintf("imsm: activate spare: inst=%d failed=%d (%d) level=%d\n",
		inst, failed, a->info.array.raid_disks, a->info.array.level);

	if (dev->vol.migr_state &&
	    dev->vol.migr_type == MIGR_GEN_MIGR)
		/* No repair during migration */
		return NULL;

	if (a->info.array.level == 4)
		/* No repair for takeovered array
		 * imsm doesn't support raid4
		 */
		return NULL;

	if (imsm_check_degraded(super, dev, failed) != IMSM_T_STATE_DEGRADED)
		return NULL;

	/*
	 * If there are any failed disks check state of the other volume.
	 * Block rebuild if the another one is failed until failed disks
	 * are removed from container.
	 */
	if (failed) {
		dprintf("found failed disks in %s, check if there another"
			"failed sub-array.\n",
			dev->volume);
		/* check if states of the other volumes allow for rebuild */
		for (i = 0; i <  super->anchor->num_raid_devs; i++) {
			if (i != inst) {
				allowed = imsm_rebuild_allowed(a->container,
							       i, failed);
				if (!allowed)
					return NULL;
			}
		}
	}

	/* For each slot, if it is not working, find a spare */
	for (i = 0; i < a->info.array.raid_disks; i++) {
		for (d = a->info.devs ; d ; d = d->next)
			if (d->disk.raid_disk == i)
				break;
		dprintf("found %d: %p %x\n", i, d, d?d->curr_state:0);
		if (d && (d->state_fd >= 0))
			continue;

		/*
		 * OK, this device needs recovery.  Try to re-add the
		 * previous occupant of this slot, if this fails see if
		 * we can continue the assimilation of a spare that was
		 * partially assimilated, finally try to activate a new
		 * spare.
		 */
		dl = imsm_readd(super, i, a);
		if (!dl)
			dl = imsm_add_spare(super, i, a, 0, NULL);
		if (!dl)
			dl = imsm_add_spare(super, i, a, 1, NULL);
		if (!dl)
			continue;
 
		/* found a usable disk with enough space */
		di = malloc(sizeof(*di));
		if (!di)
			continue;
		memset(di, 0, sizeof(*di));

		/* dl->index will be -1 in the case we are activating a
		 * pristine spare.  imsm_process_update() will create a
		 * new index in this case.  Once a disk is found to be
		 * failed in all member arrays it is kicked from the
		 * metadata
		 */
		di->disk.number = dl->index;

		/* (ab)use di->devs to store a pointer to the device
		 * we chose
		 */
		di->devs = (struct mdinfo *) dl;

		di->disk.raid_disk = i;
		di->disk.major = dl->major;
		di->disk.minor = dl->minor;
		di->disk.state = 0;
		di->recovery_start = 0;
		di->data_offset = __le32_to_cpu(map->pba_of_lba0);
		di->component_size = a->info.component_size;
		di->container_member = inst;
		super->random = random32();
		di->next = rv;
		rv = di;
		num_spares++;
		dprintf("%x:%x to be %d at %llu\n", dl->major, dl->minor,
			i, di->data_offset);

		break;
	}

	if (!rv)
		/* No spares found */
		return rv;
	/* Now 'rv' has a list of devices to return.
	 * Create a metadata_update record to update the
	 * disk_ord_tbl for the array
	 */
	mu = malloc(sizeof(*mu));
	if (mu) {
		mu->buf = malloc(sizeof(struct imsm_update_activate_spare) * num_spares);
		if (mu->buf == NULL) {
			free(mu);
			mu = NULL;
		}
	}
	if (!mu) {
		while (rv) {
			struct mdinfo *n = rv->next;

			free(rv);
			rv = n;
		}
		return NULL;
	}
			
	mu->space = NULL;
	mu->space_list = NULL;
	mu->len = sizeof(struct imsm_update_activate_spare) * num_spares;
	mu->next = *updates;
	u = (struct imsm_update_activate_spare *) mu->buf;

	for (di = rv ; di ; di = di->next) {
		u->type = update_activate_spare;
		u->dl = (struct dl *) di->devs;
		di->devs = NULL;
		u->slot = di->disk.raid_disk;
		u->array = inst;
		u->next = u + 1;
		u++;
	}
	(u-1)->next = NULL;
	*updates = mu;

	return rv;
}

static int disks_overlap(struct intel_super *super, int idx, struct imsm_update_create_array *u)
{
	struct imsm_dev *dev = get_imsm_dev(super, idx);
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct imsm_map *new_map = get_imsm_map(&u->dev, 0);
	struct disk_info *inf = get_disk_info(u);
	struct imsm_disk *disk;
	int i;
	int j;

	for (i = 0; i < map->num_members; i++) {
		disk = get_imsm_disk(super, get_imsm_disk_idx(dev, i, -1));
		for (j = 0; j < new_map->num_members; j++)
			if (serialcmp(disk->serial, inf[j].serial) == 0)
				return 1;
	}

	return 0;
}


static struct dl *get_disk_super(struct intel_super *super, int major, int minor)
{
	struct dl *dl = NULL;
	for (dl = super->disks; dl; dl = dl->next)
		if ((dl->major == major) &&  (dl->minor == minor))
			return dl;
	return NULL;
}

static int remove_disk_super(struct intel_super *super, int major, int minor)
{
	struct dl *prev = NULL;
	struct dl *dl;

	prev = NULL;
	for (dl = super->disks; dl; dl = dl->next) {
		if ((dl->major == major) && (dl->minor == minor)) {
			/* remove */
			if (prev)
				prev->next = dl->next;
			else
				super->disks = dl->next;
			dl->next = NULL;
			__free_imsm_disk(dl);
			dprintf("%s: removed %x:%x\n",
				__func__, major, minor);
			break;
		}
		prev = dl;
	}
	return 0;
}

static void imsm_delete(struct intel_super *super, struct dl **dlp, unsigned index);

static int add_remove_disk_update(struct intel_super *super)
{
	int check_degraded = 0;
	struct dl *disk = NULL;
	/* add/remove some spares to/from the metadata/contrainer */
	while (super->disk_mgmt_list) {
		struct dl *disk_cfg;

		disk_cfg = super->disk_mgmt_list;
		super->disk_mgmt_list = disk_cfg->next;
		disk_cfg->next = NULL;

		if (disk_cfg->action == DISK_ADD) {
			disk_cfg->next = super->disks;
			super->disks = disk_cfg;
			check_degraded = 1;
			dprintf("%s: added %x:%x\n",
				__func__, disk_cfg->major,
				disk_cfg->minor);
		} else if (disk_cfg->action == DISK_REMOVE) {
			dprintf("Disk remove action processed: %x.%x\n",
				disk_cfg->major, disk_cfg->minor);
			disk = get_disk_super(super,
					      disk_cfg->major,
					      disk_cfg->minor);
			if (disk) {
				/* store action status */
				disk->action = DISK_REMOVE;
				/* remove spare disks only */
				if (disk->index == -1) {
					remove_disk_super(super,
							  disk_cfg->major,
							  disk_cfg->minor);
				}
			}
			/* release allocate disk structure */
			__free_imsm_disk(disk_cfg);
		}
	}
	return check_degraded;
}


static int apply_reshape_migration_update(struct imsm_update_reshape_migration *u,
						struct intel_super *super,
						void ***space_list)
{
	struct intel_dev *id;
	void **tofree = NULL;
	int ret_val = 0;

	dprintf("apply_reshape_migration_update()\n");
	if ((u->subdev < 0) ||
	    (u->subdev > 1)) {
		dprintf("imsm: Error: Wrong subdev: %i\n", u->subdev);
		return ret_val;
	}
	if ((space_list == NULL) || (*space_list == NULL)) {
		dprintf("imsm: Error: Memory is not allocated\n");
		return ret_val;
	}

	for (id = super->devlist ; id; id = id->next) {
		if (id->index == (unsigned)u->subdev) {
			struct imsm_dev *dev = get_imsm_dev(super, u->subdev);
			struct imsm_map *map;
			struct imsm_dev *new_dev =
				(struct imsm_dev *)*space_list;
			struct imsm_map *migr_map = get_imsm_map(dev, 1);
			int to_state;
			struct dl *new_disk;

			if (new_dev == NULL)
				return ret_val;
			*space_list = **space_list;
			memcpy(new_dev, dev, sizeof_imsm_dev(dev, 0));
			map = get_imsm_map(new_dev, 0);
			if (migr_map) {
				dprintf("imsm: Error: migration in progress");
				return ret_val;
			}

			to_state = map->map_state;
			if ((u->new_level == 5) && (map->raid_level == 0)) {
				map->num_members++;
				/* this should not happen */
				if (u->new_disks[0] < 0) {
					map->failed_disk_num =
						map->num_members - 1;
					to_state = IMSM_T_STATE_DEGRADED;
				} else
					to_state = IMSM_T_STATE_NORMAL;
			}
			migrate(new_dev, super, to_state, MIGR_GEN_MIGR);
			if (u->new_level > -1)
				map->raid_level = u->new_level;
			migr_map = get_imsm_map(new_dev, 1);
			if ((u->new_level == 5) &&
			    (migr_map->raid_level == 0)) {
				int ord = map->num_members - 1;
				migr_map->num_members--;
				if (u->new_disks[0] < 0)
					ord |= IMSM_ORD_REBUILD;
				set_imsm_ord_tbl_ent(map,
						     map->num_members - 1,
						     ord);
			}
			id->dev = new_dev;
			tofree = (void **)dev;

			/* update chunk size
			 */
			if (u->new_chunksize > 0)
				map->blocks_per_strip =
					__cpu_to_le16(u->new_chunksize * 2);

			/* add disk
			 */
			if ((u->new_level != 5) ||
			    (migr_map->raid_level != 0) ||
			    (migr_map->raid_level == map->raid_level))
				goto skip_disk_add;

			if (u->new_disks[0] >= 0) {
				/* use passes spare
				 */
				new_disk = get_disk_super(super,
							major(u->new_disks[0]),
							minor(u->new_disks[0]));
				dprintf("imsm: new disk for reshape is: %i:%i "
					"(%p, index = %i)\n",
					major(u->new_disks[0]),
					minor(u->new_disks[0]),
					new_disk, new_disk->index);
				if (new_disk == NULL)
					goto error_disk_add;

				new_disk->index = map->num_members - 1;
				/* slot to fill in autolayout
				 */
				new_disk->raiddisk = new_disk->index;
				new_disk->disk.status |= CONFIGURED_DISK;
				new_disk->disk.status &= ~SPARE_DISK;
			} else
				goto error_disk_add;

skip_disk_add:
			*tofree = *space_list;
			/* calculate new size
			 */
			imsm_set_array_size(new_dev);

			ret_val = 1;
		}
	}

	if (tofree)
		*space_list = tofree;
	return ret_val;

error_disk_add:
	dprintf("Error: imsm: Cannot find disk.\n");
	return ret_val;
}


static int apply_reshape_container_disks_update(struct imsm_update_reshape *u,
						struct intel_super *super,
						void ***space_list)
{
	struct dl *new_disk;
	struct intel_dev *id;
	int i;
	int delta_disks = u->new_raid_disks - u->old_raid_disks;
	int disk_count = u->old_raid_disks;
	void **tofree = NULL;
	int devices_to_reshape = 1;
	struct imsm_super *mpb = super->anchor;
	int ret_val = 0;
	unsigned int dev_id;

	dprintf("imsm: apply_reshape_container_disks_update()\n");

	/* enable spares to use in array */
	for (i = 0; i < delta_disks; i++) {
		new_disk = get_disk_super(super,
					  major(u->new_disks[i]),
					  minor(u->new_disks[i]));
		dprintf("imsm: new disk for reshape is: %i:%i "
			"(%p, index = %i)\n",
			major(u->new_disks[i]), minor(u->new_disks[i]),
			new_disk, new_disk->index);
		if ((new_disk == NULL) ||
		    ((new_disk->index >= 0) &&
		     (new_disk->index < u->old_raid_disks)))
			goto update_reshape_exit;
		new_disk->index = disk_count++;
		/* slot to fill in autolayout
		 */
		new_disk->raiddisk = new_disk->index;
		new_disk->disk.status |=
			CONFIGURED_DISK;
		new_disk->disk.status &= ~SPARE_DISK;
	}

	dprintf("imsm: volume set mpb->num_raid_devs = %i\n",
		mpb->num_raid_devs);
	/* manage changes in volume
	 */
	for (dev_id = 0; dev_id < mpb->num_raid_devs; dev_id++) {
		void **sp = *space_list;
		struct imsm_dev *newdev;
		struct imsm_map *newmap, *oldmap;

		for (id = super->devlist ; id; id = id->next) {
			if (id->index == dev_id)
				break;
		}
		if (id == NULL)
			break;
		if (!sp)
			continue;
		*space_list = *sp;
		newdev = (void*)sp;
		/* Copy the dev, but not (all of) the map */
		memcpy(newdev, id->dev, sizeof(*newdev));
		oldmap = get_imsm_map(id->dev, 0);
		newmap = get_imsm_map(newdev, 0);
		/* Copy the current map */
		memcpy(newmap, oldmap, sizeof_imsm_map(oldmap));
		/* update one device only
		 */
		if (devices_to_reshape) {
			dprintf("imsm: modifying subdev: %i\n",
				id->index);
			devices_to_reshape--;
			newdev->vol.migr_state = 1;
			newdev->vol.curr_migr_unit = 0;
			newdev->vol.migr_type = MIGR_GEN_MIGR;
			newmap->num_members = u->new_raid_disks;
			for (i = 0; i < delta_disks; i++) {
				set_imsm_ord_tbl_ent(newmap,
						     u->old_raid_disks + i,
						     u->old_raid_disks + i);
			}
			/* New map is correct, now need to save old map
			 */
			newmap = get_imsm_map(newdev, 1);
			memcpy(newmap, oldmap, sizeof_imsm_map(oldmap));

			imsm_set_array_size(newdev);
		}

		sp = (void **)id->dev;
		id->dev = newdev;
		*sp = tofree;
		tofree = sp;

		/* Clear migration record */
		memset(super->migr_rec, 0, sizeof(struct migr_record));
	}
	if (tofree)
		*space_list = tofree;
	ret_val = 1;

update_reshape_exit:

	return ret_val;
}

static int apply_takeover_update(struct imsm_update_takeover *u,
				 struct intel_super *super,
				 void ***space_list)
{
	struct imsm_dev *dev = NULL;
	struct intel_dev *dv;
	struct imsm_dev *dev_new;
	struct imsm_map *map;
	struct dl *dm, *du;
	int i;

	for (dv = super->devlist; dv; dv = dv->next)
		if (dv->index == (unsigned int)u->subarray) {
			dev = dv->dev;
			break;
		}

	if (dev == NULL)
		return 0;

	map = get_imsm_map(dev, 0);

	if (u->direction == R10_TO_R0) {
		/* Number of failed disks must be half of initial disk number */
		if (imsm_count_failed(super, dev) != (map->num_members / 2))
			return 0;

		/* iterate through devices to mark removed disks as spare */
		for (dm = super->disks; dm; dm = dm->next) {
			if (dm->disk.status & FAILED_DISK) {
				int idx = dm->index;
				/* update indexes on the disk list */
/* FIXME this loop-with-the-loop looks wrong,  I'm not convinced
   the index values will end up being correct.... NB */
				for (du = super->disks; du; du = du->next)
					if (du->index > idx)
						du->index--;
				/* mark as spare disk */
				dm->disk.status = SPARE_DISK;
				dm->index = -1;
			}
		}
		/* update map */
		map->num_members = map->num_members / 2;
		map->map_state = IMSM_T_STATE_NORMAL;
		map->num_domains = 1;
		map->raid_level = 0;
		map->failed_disk_num = -1;
	}

	if (u->direction == R0_TO_R10) {
		void **space;
		/* update slots in current disk list */
		for (dm = super->disks; dm; dm = dm->next) {
			if (dm->index >= 0)
				dm->index *= 2;
		}
		/* create new *missing* disks */
		for (i = 0; i < map->num_members; i++) {
			space = *space_list;
			if (!space)
				continue;
			*space_list = *space;
			du = (void *)space;
			memcpy(du, super->disks, sizeof(*du));
			du->fd = -1;
			du->minor = 0;
			du->major = 0;
			du->index = (i * 2) + 1;
			sprintf((char *)du->disk.serial,
				" MISSING_%d", du->index);
			sprintf((char *)du->serial,
				"MISSING_%d", du->index);
			du->next = super->missing;
			super->missing = du;
		}
		/* create new dev and map */
		space = *space_list;
		if (!space)
			return 0;
		*space_list = *space;
		dev_new = (void *)space;
		memcpy(dev_new, dev, sizeof(*dev));
		/* update new map */
		map = get_imsm_map(dev_new, 0);
		map->num_members = map->num_members * 2;
		map->map_state = IMSM_T_STATE_DEGRADED;
		map->num_domains = 2;
		map->raid_level = 1;
		/* replace dev<->dev_new */
		dv->dev = dev_new;
	}
	/* update disk order table */
	for (du = super->disks; du; du = du->next)
		if (du->index >= 0)
			set_imsm_ord_tbl_ent(map, du->index, du->index);
	for (du = super->missing; du; du = du->next)
		if (du->index >= 0) {
			set_imsm_ord_tbl_ent(map, du->index, du->index);
			mark_missing(dv->dev, &du->disk, du->index);
		}

	return 1;
}

static void imsm_process_update(struct supertype *st,
			        struct metadata_update *update)
{
	/**
	 * crack open the metadata_update envelope to find the update record
	 * update can be one of:
	 *    update_reshape_container_disks - all the arrays in the container
	 *      are being reshaped to have more devices.  We need to mark
	 *      the arrays for general migration and convert selected spares
	 *      into active devices.
	 *    update_activate_spare - a spare device has replaced a failed
	 * 	device in an array, update the disk_ord_tbl.  If this disk is
	 * 	present in all member arrays then also clear the SPARE_DISK
	 * 	flag
	 *    update_create_array
	 *    update_kill_array
	 *    update_rename_array
	 *    update_add_remove_disk
	 */
	struct intel_super *super = st->sb;
	struct imsm_super *mpb;
	enum imsm_update_type type = *(enum imsm_update_type *) update->buf;

	/* update requires a larger buf but the allocation failed */
	if (super->next_len && !super->next_buf) {
		super->next_len = 0;
		return;
	}

	if (super->next_buf) {
		memcpy(super->next_buf, super->buf, super->len);
		free(super->buf);
		super->len = super->next_len;
		super->buf = super->next_buf;

		super->next_len = 0;
		super->next_buf = NULL;
	}

	mpb = super->anchor;

	switch (type) {
	case update_general_migration_checkpoint: {
		struct intel_dev *id;
		struct imsm_update_general_migration_checkpoint *u =
							(void *)update->buf;

		dprintf("imsm: process_update() "
			"for update_general_migration_checkpoint called\n");

		/* find device under general migration */
		for (id = super->devlist ; id; id = id->next) {
			if (is_gen_migration(id->dev)) {
				id->dev->vol.curr_migr_unit =
					__cpu_to_le32(u->curr_migr_unit);
				super->updates_pending++;
			}
		}
		break;
	}
	case update_takeover: {
		struct imsm_update_takeover *u = (void *)update->buf;
		if (apply_takeover_update(u, super, &update->space_list)) {
			imsm_update_version_info(super);
			super->updates_pending++;
		}
		break;
	}

	case update_reshape_container_disks: {
		struct imsm_update_reshape *u = (void *)update->buf;
		if (apply_reshape_container_disks_update(
			    u, super, &update->space_list))
			super->updates_pending++;
		break;
	}
	case update_reshape_migration: {
		struct imsm_update_reshape_migration *u = (void *)update->buf;
		if (apply_reshape_migration_update(
			    u, super, &update->space_list))
			super->updates_pending++;
		break;
	}
	case update_activate_spare: {
		struct imsm_update_activate_spare *u = (void *) update->buf; 
		struct imsm_dev *dev = get_imsm_dev(super, u->array);
		struct imsm_map *map = get_imsm_map(dev, 0);
		struct imsm_map *migr_map;
		struct active_array *a;
		struct imsm_disk *disk;
		__u8 to_state;
		struct dl *dl;
		unsigned int found;
		int failed;
		int victim = get_imsm_disk_idx(dev, u->slot, -1);
		int i;

		for (dl = super->disks; dl; dl = dl->next)
			if (dl == u->dl)
				break;

		if (!dl) {
			fprintf(stderr, "error: imsm_activate_spare passed "
				"an unknown disk (index: %d)\n",
				u->dl->index);
			return;
		}

		super->updates_pending++;
		/* count failures (excluding rebuilds and the victim)
		 * to determine map[0] state
		 */
		failed = 0;
		for (i = 0; i < map->num_members; i++) {
			if (i == u->slot)
				continue;
			disk = get_imsm_disk(super,
					     get_imsm_disk_idx(dev, i, -1));
			if (!disk || is_failed(disk))
				failed++;
		}

		/* adding a pristine spare, assign a new index */
		if (dl->index < 0) {
			dl->index = super->anchor->num_disks;
			super->anchor->num_disks++;
		}
		disk = &dl->disk;
		disk->status |= CONFIGURED_DISK;
		disk->status &= ~SPARE_DISK;

		/* mark rebuild */
		to_state = imsm_check_degraded(super, dev, failed);
		map->map_state = IMSM_T_STATE_DEGRADED;
		migrate(dev, super, to_state, MIGR_REBUILD);
		migr_map = get_imsm_map(dev, 1);
		set_imsm_ord_tbl_ent(map, u->slot, dl->index);
		set_imsm_ord_tbl_ent(migr_map, u->slot, dl->index | IMSM_ORD_REBUILD);

		/* update the family_num to mark a new container
		 * generation, being careful to record the existing
		 * family_num in orig_family_num to clean up after
		 * earlier mdadm versions that neglected to set it.
		 */
		if (mpb->orig_family_num == 0)
			mpb->orig_family_num = mpb->family_num;
		mpb->family_num += super->random;

		/* count arrays using the victim in the metadata */
		found = 0;
		for (a = st->arrays; a ; a = a->next) {
			dev = get_imsm_dev(super, a->info.container_member);
			map = get_imsm_map(dev, 0);

			if (get_imsm_disk_slot(map, victim) >= 0)
				found++;
		}

		/* delete the victim if it is no longer being
		 * utilized anywhere
		 */
		if (!found) {
			struct dl **dlp;

			/* We know that 'manager' isn't touching anything,
			 * so it is safe to delete
			 */
			for (dlp = &super->disks; *dlp; dlp = &(*dlp)->next)
				if ((*dlp)->index == victim)
					break;

			/* victim may be on the missing list */
			if (!*dlp)
				for (dlp = &super->missing; *dlp; dlp = &(*dlp)->next)
					if ((*dlp)->index == victim)
						break;
			imsm_delete(super, dlp, victim);
		}
		break;
	}
	case update_create_array: {
		/* someone wants to create a new array, we need to be aware of
		 * a few races/collisions:
		 * 1/ 'Create' called by two separate instances of mdadm
		 * 2/ 'Create' versus 'activate_spare': mdadm has chosen
		 *     devices that have since been assimilated via
		 *     activate_spare.
		 * In the event this update can not be carried out mdadm will
		 * (FIX ME) notice that its update did not take hold.
		 */
		struct imsm_update_create_array *u = (void *) update->buf;
		struct intel_dev *dv;
		struct imsm_dev *dev;
		struct imsm_map *map, *new_map;
		unsigned long long start, end;
		unsigned long long new_start, new_end;
		int i;
		struct disk_info *inf;
		struct dl *dl;

		/* handle racing creates: first come first serve */
		if (u->dev_idx < mpb->num_raid_devs) {
			dprintf("%s: subarray %d already defined\n",
				__func__, u->dev_idx);
			goto create_error;
		}

		/* check update is next in sequence */
		if (u->dev_idx != mpb->num_raid_devs) {
			dprintf("%s: can not create array %d expected index %d\n",
				__func__, u->dev_idx, mpb->num_raid_devs);
			goto create_error;
		}

		new_map = get_imsm_map(&u->dev, 0);
		new_start = __le32_to_cpu(new_map->pba_of_lba0);
		new_end = new_start + __le32_to_cpu(new_map->blocks_per_member);
		inf = get_disk_info(u);

		/* handle activate_spare versus create race:
		 * check to make sure that overlapping arrays do not include
		 * overalpping disks
		 */
		for (i = 0; i < mpb->num_raid_devs; i++) {
			dev = get_imsm_dev(super, i);
			map = get_imsm_map(dev, 0);
			start = __le32_to_cpu(map->pba_of_lba0);
			end = start + __le32_to_cpu(map->blocks_per_member);
			if ((new_start >= start && new_start <= end) ||
			    (start >= new_start && start <= new_end))
				/* overlap */;
			else
				continue;

			if (disks_overlap(super, i, u)) {
				dprintf("%s: arrays overlap\n", __func__);
				goto create_error;
			}
		}

		/* check that prepare update was successful */
		if (!update->space) {
			dprintf("%s: prepare update failed\n", __func__);
			goto create_error;
		}

		/* check that all disks are still active before committing
		 * changes.  FIXME: could we instead handle this by creating a
		 * degraded array?  That's probably not what the user expects,
		 * so better to drop this update on the floor.
		 */
		for (i = 0; i < new_map->num_members; i++) {
			dl = serial_to_dl(inf[i].serial, super);
			if (!dl) {
				dprintf("%s: disk disappeared\n", __func__);
				goto create_error;
			}
		}

		super->updates_pending++;

		/* convert spares to members and fixup ord_tbl */
		for (i = 0; i < new_map->num_members; i++) {
			dl = serial_to_dl(inf[i].serial, super);
			if (dl->index == -1) {
				dl->index = mpb->num_disks;
				mpb->num_disks++;
				dl->disk.status |= CONFIGURED_DISK;
				dl->disk.status &= ~SPARE_DISK;
			}
			set_imsm_ord_tbl_ent(new_map, i, dl->index);
		}

		dv = update->space;
		dev = dv->dev;
		update->space = NULL;
		imsm_copy_dev(dev, &u->dev);
		dv->index = u->dev_idx;
		dv->next = super->devlist;
		super->devlist = dv;
		mpb->num_raid_devs++;

		imsm_update_version_info(super);
		break;
 create_error:
		/* mdmon knows how to release update->space, but not
		 * ((struct intel_dev *) update->space)->dev
		 */
		if (update->space) {
			dv = update->space;
			free(dv->dev);
		}
		break;
	}
	case update_kill_array: {
		struct imsm_update_kill_array *u = (void *) update->buf;
		int victim = u->dev_idx;
		struct active_array *a;
		struct intel_dev **dp;
		struct imsm_dev *dev;

		/* sanity check that we are not affecting the uuid of
		 * active arrays, or deleting an active array
		 *
		 * FIXME when immutable ids are available, but note that
		 * we'll also need to fixup the invalidated/active
		 * subarray indexes in mdstat
		 */
		for (a = st->arrays; a; a = a->next)
			if (a->info.container_member >= victim)
				break;
		/* by definition if mdmon is running at least one array
		 * is active in the container, so checking
		 * mpb->num_raid_devs is just extra paranoia
		 */
		dev = get_imsm_dev(super, victim);
		if (a || !dev || mpb->num_raid_devs == 1) {
			dprintf("failed to delete subarray-%d\n", victim);
			break;
		}

		for (dp = &super->devlist; *dp;)
			if ((*dp)->index == (unsigned)super->current_vol) {
				*dp = (*dp)->next;
			} else {
				if ((*dp)->index > (unsigned)victim)
					(*dp)->index--;
				dp = &(*dp)->next;
			}
		mpb->num_raid_devs--;
		super->updates_pending++;
		break;
	}
	case update_rename_array: {
		struct imsm_update_rename_array *u = (void *) update->buf;
		char name[MAX_RAID_SERIAL_LEN+1];
		int target = u->dev_idx;
		struct active_array *a;
		struct imsm_dev *dev;

		/* sanity check that we are not affecting the uuid of
		 * an active array
		 */
		snprintf(name, MAX_RAID_SERIAL_LEN, "%s", (char *) u->name);
		name[MAX_RAID_SERIAL_LEN] = '\0';
		for (a = st->arrays; a; a = a->next)
			if (a->info.container_member == target)
				break;
		dev = get_imsm_dev(super, u->dev_idx);
		if (a || !dev || !check_name(super, name, 1)) {
			dprintf("failed to rename subarray-%d\n", target);
			break;
		}

		snprintf((char *) dev->volume, MAX_RAID_SERIAL_LEN, "%s", name);
		super->updates_pending++;
		break;
	}
	case update_add_remove_disk: {
		/* we may be able to repair some arrays if disks are
		 * being added, check teh status of add_remove_disk
		 * if discs has been added.
		 */
		if (add_remove_disk_update(super)) {
			struct active_array *a;

			super->updates_pending++;
			for (a = st->arrays; a; a = a->next)
				a->check_degraded = 1;
		}
		break;
	}
	default:
		fprintf(stderr, "error: unsuported process update type:"
			"(type: %d)\n",	type);
	}
}

static struct mdinfo *get_spares_for_grow(struct supertype *st);

static void imsm_prepare_update(struct supertype *st,
				struct metadata_update *update)
{
	/**
	 * Allocate space to hold new disk entries, raid-device entries or a new
	 * mpb if necessary.  The manager synchronously waits for updates to
	 * complete in the monitor, so new mpb buffers allocated here can be
	 * integrated by the monitor thread without worrying about live pointers
	 * in the manager thread.
	 */
	enum imsm_update_type type = *(enum imsm_update_type *) update->buf;
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	size_t buf_len;
	size_t len = 0;

	switch (type) {
	case update_general_migration_checkpoint:
		dprintf("imsm: prepare_update() "
			"for update_general_migration_checkpoint called\n");
		break;
	case update_takeover: {
		struct imsm_update_takeover *u = (void *)update->buf;
		if (u->direction == R0_TO_R10) {
			void **tail = (void **)&update->space_list;
			struct imsm_dev *dev = get_imsm_dev(super, u->subarray);
			struct imsm_map *map = get_imsm_map(dev, 0);
			int num_members = map->num_members;
			void *space;
			int size, i;
			int err = 0;
			/* allocate memory for added disks */
			for (i = 0; i < num_members; i++) {
				size = sizeof(struct dl);
				space = malloc(size);
				if (!space) {
					err++;
					break;
				}
				*tail = space;
				tail = space;
				*tail = NULL;
			}
			/* allocate memory for new device */
			size = sizeof_imsm_dev(super->devlist->dev, 0) +
				(num_members * sizeof(__u32));
			space = malloc(size);
			if (!space)
				err++;
			else {
				*tail = space;
				tail = space;
				*tail = NULL;
			}
			if (!err) {
				len = disks_to_mpb_size(num_members * 2);
			} else {
				/* if allocation didn't success, free buffer */
				while (update->space_list) {
					void **sp = update->space_list;
					update->space_list = *sp;
					free(sp);
				}
			}
		}

		break;
	}
	case update_reshape_container_disks: {
		/* Every raid device in the container is about to
		 * gain some more devices, and we will enter a
		 * reconfiguration.
		 * So each 'imsm_map' will be bigger, and the imsm_vol
		 * will now hold 2 of them.
		 * Thus we need new 'struct imsm_dev' allocations sized
		 * as sizeof_imsm_dev but with more devices in both maps.
		 */
		struct imsm_update_reshape *u = (void *)update->buf;
		struct intel_dev *dl;
		void **space_tail = (void**)&update->space_list;

		dprintf("imsm: imsm_prepare_update() for update_reshape\n");

		for (dl = super->devlist; dl; dl = dl->next) {
			int size = sizeof_imsm_dev(dl->dev, 1);
			void *s;
			if (u->new_raid_disks > u->old_raid_disks)
				size += sizeof(__u32)*2*
					(u->new_raid_disks - u->old_raid_disks);
			s = malloc(size);
			if (!s)
				break;
			*space_tail = s;
			space_tail = s;
			*space_tail = NULL;
		}

		len = disks_to_mpb_size(u->new_raid_disks);
		dprintf("New anchor length is %llu\n", (unsigned long long)len);
		break;
	}
	case update_reshape_migration: {
		/* for migration level 0->5 we need to add disks
		 * so the same as for container operation we will copy
		 * device to the bigger location.
		 * in memory prepared device and new disk area are prepared
		 * for usage in process update
		 */
		struct imsm_update_reshape_migration *u = (void *)update->buf;
		struct intel_dev *id;
		void **space_tail = (void **)&update->space_list;
		int size;
		void *s;
		int current_level = -1;

		dprintf("imsm: imsm_prepare_update() for update_reshape\n");

		/* add space for bigger array in update
		 */
		for (id = super->devlist; id; id = id->next) {
			if (id->index == (unsigned)u->subdev) {
				size = sizeof_imsm_dev(id->dev, 1);
				if (u->new_raid_disks > u->old_raid_disks)
					size += sizeof(__u32)*2*
					(u->new_raid_disks - u->old_raid_disks);
				s = malloc(size);
				if (!s)
					break;
				*space_tail = s;
				space_tail = s;
				*space_tail = NULL;
				break;
			}
		}
		if (update->space_list == NULL)
			break;

		/* add space for disk in update
		 */
		size = sizeof(struct dl);
		s = malloc(size);
		if (!s) {
			free(update->space_list);
			update->space_list = NULL;
			break;
		}
		*space_tail = s;
		space_tail = s;
		*space_tail = NULL;

		/* add spare device to update
		 */
		for (id = super->devlist ; id; id = id->next)
			if (id->index == (unsigned)u->subdev) {
				struct imsm_dev *dev;
				struct imsm_map *map;

				dev = get_imsm_dev(super, u->subdev);
				map = get_imsm_map(dev, 0);
				current_level = map->raid_level;
				break;
			}
		if ((u->new_level == 5) && (u->new_level != current_level)) {
			struct mdinfo *spares;

			spares = get_spares_for_grow(st);
			if (spares) {
				struct dl *dl;
				struct mdinfo *dev;

				dev = spares->devs;
				if (dev) {
					u->new_disks[0] =
						makedev(dev->disk.major,
							dev->disk.minor);
					dl = get_disk_super(super,
							    dev->disk.major,
							    dev->disk.minor);
					dl->index = u->old_raid_disks;
					dev = dev->next;
				}
				sysfs_free(spares);
			}
		}
		len = disks_to_mpb_size(u->new_raid_disks);
		dprintf("New anchor length is %llu\n", (unsigned long long)len);
		break;
	}
	case update_create_array: {
		struct imsm_update_create_array *u = (void *) update->buf;
		struct intel_dev *dv;
		struct imsm_dev *dev = &u->dev;
		struct imsm_map *map = get_imsm_map(dev, 0);
		struct dl *dl;
		struct disk_info *inf;
		int i;
		int activate = 0;

		inf = get_disk_info(u);
		len = sizeof_imsm_dev(dev, 1);
		/* allocate a new super->devlist entry */
		dv = malloc(sizeof(*dv));
		if (dv) {
			dv->dev = malloc(len);
			if (dv->dev)
				update->space = dv;
			else {
				free(dv);
				update->space = NULL;
			}
		}

		/* count how many spares will be converted to members */
		for (i = 0; i < map->num_members; i++) {
			dl = serial_to_dl(inf[i].serial, super);
			if (!dl) {
				/* hmm maybe it failed?, nothing we can do about
				 * it here
				 */
				continue;
			}
			if (count_memberships(dl, super) == 0)
				activate++;
		}
		len += activate * sizeof(struct imsm_disk);
		break;
	default:
		break;
	}
	}

	/* check if we need a larger metadata buffer */
	if (super->next_buf)
		buf_len = super->next_len;
	else
		buf_len = super->len;

	if (__le32_to_cpu(mpb->mpb_size) + len > buf_len) {
		/* ok we need a larger buf than what is currently allocated
		 * if this allocation fails process_update will notice that
		 * ->next_len is set and ->next_buf is NULL
		 */
		buf_len = ROUND_UP(__le32_to_cpu(mpb->mpb_size) + len, 512);
		if (super->next_buf)
			free(super->next_buf);

		super->next_len = buf_len;
		if (posix_memalign(&super->next_buf, 512, buf_len) == 0)
			memset(super->next_buf, 0, buf_len);
		else
			super->next_buf = NULL;
	}
}

/* must be called while manager is quiesced */
static void imsm_delete(struct intel_super *super, struct dl **dlp, unsigned index)
{
	struct imsm_super *mpb = super->anchor;
	struct dl *iter;
	struct imsm_dev *dev;
	struct imsm_map *map;
	int i, j, num_members;
	__u32 ord;

	dprintf("%s: deleting device[%d] from imsm_super\n",
		__func__, index);

	/* shift all indexes down one */
	for (iter = super->disks; iter; iter = iter->next)
		if (iter->index > (int)index)
			iter->index--;
	for (iter = super->missing; iter; iter = iter->next)
		if (iter->index > (int)index)
			iter->index--;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		dev = get_imsm_dev(super, i);
		map = get_imsm_map(dev, 0);
		num_members = map->num_members;
		for (j = 0; j < num_members; j++) {
			/* update ord entries being careful not to propagate
			 * ord-flags to the first map
			 */
			ord = get_imsm_ord_tbl_ent(dev, j, -1);

			if (ord_to_idx(ord) <= index)
				continue;

			map = get_imsm_map(dev, 0);
			set_imsm_ord_tbl_ent(map, j, ord_to_idx(ord - 1));
			map = get_imsm_map(dev, 1);
			if (map)
				set_imsm_ord_tbl_ent(map, j, ord - 1);
		}
	}

	mpb->num_disks--;
	super->updates_pending++;
	if (*dlp) {
		struct dl *dl = *dlp;

		*dlp = (*dlp)->next;
		__free_imsm_disk(dl);
	}
}
#endif /* MDASSEMBLE */
/*******************************************************************************
 * Function:	open_backup_targets
 * Description:	Function opens file descriptors for all devices given in
 *		info->devs
 * Parameters:
 *	info		: general array info
 *	raid_disks	: number of disks
 *	raid_fds	: table of device's file descriptors
 * Returns:
 *	 0 : success
 *	-1 : fail
 ******************************************************************************/
int open_backup_targets(struct mdinfo *info, int raid_disks, int *raid_fds)
{
	struct mdinfo *sd;

	for (sd = info->devs ; sd ; sd = sd->next) {
		char *dn;

		if (sd->disk.state & (1<<MD_DISK_FAULTY)) {
			dprintf("disk is faulty!!\n");
			continue;
		}

		if ((sd->disk.raid_disk >= raid_disks) ||
		    (sd->disk.raid_disk < 0))
			continue;

		dn = map_dev(sd->disk.major,
			     sd->disk.minor, 1);
		raid_fds[sd->disk.raid_disk] = dev_open(dn, O_RDWR);
		if (raid_fds[sd->disk.raid_disk] < 0) {
			fprintf(stderr, "cannot open component\n");
			return -1;
		}
	}
	return 0;
}

#ifndef MDASSEMBLE
/*******************************************************************************
 * Function:	init_migr_record_imsm
 * Description:	Function inits imsm migration record
 * Parameters:
 *	super	: imsm internal array info
 *	dev	: device under migration
 *	info	: general array info to find the smallest device
 * Returns:
 *	none
 ******************************************************************************/
void init_migr_record_imsm(struct supertype *st, struct imsm_dev *dev,
			   struct mdinfo *info)
{
	struct intel_super *super = st->sb;
	struct migr_record *migr_rec = super->migr_rec;
	int new_data_disks;
	unsigned long long dsize, dev_sectors;
	long long unsigned min_dev_sectors = -1LLU;
	struct mdinfo *sd;
	char nm[30];
	int fd;
	struct imsm_map *map_dest = get_imsm_map(dev, 0);
	struct imsm_map *map_src = get_imsm_map(dev, 1);
	unsigned long long num_migr_units;
	unsigned long long array_blocks;

	memset(migr_rec, 0, sizeof(struct migr_record));
	migr_rec->family_num = __cpu_to_le32(super->anchor->family_num);

	/* only ascending reshape supported now */
	migr_rec->ascending_migr = __cpu_to_le32(1);

	migr_rec->dest_depth_per_unit = GEN_MIGR_AREA_SIZE /
		max(map_dest->blocks_per_strip, map_src->blocks_per_strip);
	migr_rec->dest_depth_per_unit *= map_dest->blocks_per_strip;
	new_data_disks = imsm_num_data_members(dev, 0);
	migr_rec->blocks_per_unit =
		__cpu_to_le32(migr_rec->dest_depth_per_unit * new_data_disks);
	migr_rec->dest_depth_per_unit =
		__cpu_to_le32(migr_rec->dest_depth_per_unit);
	array_blocks = info->component_size * new_data_disks;
	num_migr_units =
		array_blocks / __le32_to_cpu(migr_rec->blocks_per_unit);

	if (array_blocks % __le32_to_cpu(migr_rec->blocks_per_unit))
		num_migr_units++;
	migr_rec->num_migr_units = __cpu_to_le32(num_migr_units);

	migr_rec->post_migr_vol_cap =  dev->size_low;
	migr_rec->post_migr_vol_cap_hi = dev->size_high;


	/* Find the smallest dev */
	for (sd = info->devs ; sd ; sd = sd->next) {
		sprintf(nm, "%d:%d", sd->disk.major, sd->disk.minor);
		fd = dev_open(nm, O_RDONLY);
		if (fd < 0)
			continue;
		get_dev_size(fd, NULL, &dsize);
		dev_sectors = dsize / 512;
		if (dev_sectors < min_dev_sectors)
			min_dev_sectors = dev_sectors;
		close(fd);
	}
	migr_rec->ckpt_area_pba = __cpu_to_le32(min_dev_sectors -
					RAID_DISK_RESERVED_BLOCKS_IMSM_HI);

	write_imsm_migr_rec(st);

	return;
}

/*******************************************************************************
 * Function:	save_backup_imsm
 * Description:	Function saves critical data stripes to Migration Copy Area
 *		and updates the current migration unit status.
 *		Use restore_stripes() to form a destination stripe,
 *		and to write it to the Copy Area.
 * Parameters:
 *	st		: supertype information
 *	dev		: imsm device that backup is saved for
 *	info		: general array info
 *	buf		: input buffer
 *	length		: length of data to backup (blocks_per_unit)
 * Returns:
 *	 0 : success
 *,	-1 : fail
 ******************************************************************************/
int save_backup_imsm(struct supertype *st,
		     struct imsm_dev *dev,
		     struct mdinfo *info,
		     void *buf,
		     int length)
{
	int rv = -1;
	struct intel_super *super = st->sb;
	unsigned long long *target_offsets = NULL;
	int *targets = NULL;
	int i;
	struct imsm_map *map_dest = get_imsm_map(dev, 0);
	int new_disks = map_dest->num_members;
	int dest_layout = 0;
	int dest_chunk;
	unsigned long long start;
	int data_disks = imsm_num_data_members(dev, 0);

	targets = malloc(new_disks * sizeof(int));
	if (!targets)
		goto abort;

	for (i = 0; i < new_disks; i++)
		targets[i] = -1;

	target_offsets = malloc(new_disks * sizeof(unsigned long long));
	if (!target_offsets)
		goto abort;

	start = info->reshape_progress * 512;
	for (i = 0; i < new_disks; i++) {
		target_offsets[i] = (unsigned long long)
		  __le32_to_cpu(super->migr_rec->ckpt_area_pba) * 512;
		/* move back copy area adderss, it will be moved forward
		 * in restore_stripes() using start input variable
		 */
		target_offsets[i] -= start/data_disks;
	}

	if (open_backup_targets(info, new_disks, targets))
		goto abort;

	dest_layout = imsm_level_to_layout(map_dest->raid_level);
	dest_chunk = __le16_to_cpu(map_dest->blocks_per_strip) * 512;

	if (restore_stripes(targets, /* list of dest devices */
			    target_offsets, /* migration record offsets */
			    new_disks,
			    dest_chunk,
			    map_dest->raid_level,
			    dest_layout,
			    -1,    /* source backup file descriptor */
			    0,     /* input buf offset
				    * always 0 buf is already offseted */
			    start,
			    length,
			    buf) != 0) {
		fprintf(stderr, Name ": Error restoring stripes\n");
		goto abort;
	}

	rv = 0;

abort:
	if (targets) {
		for (i = 0; i < new_disks; i++)
			if (targets[i] >= 0)
				close(targets[i]);
		free(targets);
	}
	free(target_offsets);

	return rv;
}

/*******************************************************************************
 * Function:	save_checkpoint_imsm
 * Description:	Function called for current unit status update
 *		in the migration record. It writes it to disk.
 * Parameters:
 *	super	: imsm internal array info
 *	info	: general array info
 * Returns:
 *	0: success
 *	1: failure
 *	2: failure, means no valid migration record
 *		   / no general migration in progress /
 ******************************************************************************/
int save_checkpoint_imsm(struct supertype *st, struct mdinfo *info, int state)
{
	struct intel_super *super = st->sb;
	unsigned long long blocks_per_unit;
	unsigned long long curr_migr_unit;

	if (load_imsm_migr_rec(super, info) != 0) {
		dprintf("imsm: ERROR: Cannot read migration record "
			"for checkpoint save.\n");
		return 1;
	}

	blocks_per_unit = __le32_to_cpu(super->migr_rec->blocks_per_unit);
	if (blocks_per_unit == 0) {
		dprintf("imsm: no migration in progress.\n");
		return 2;
	}
	curr_migr_unit = info->reshape_progress / blocks_per_unit;
	/* check if array is alligned to copy area
	 * if it is not alligned, add one to current migration unit value
	 * this can happend on array reshape finish only
	 */
	if (info->reshape_progress % blocks_per_unit)
		curr_migr_unit++;

	super->migr_rec->curr_migr_unit =
		__cpu_to_le32(curr_migr_unit);
	super->migr_rec->rec_status = __cpu_to_le32(state);
	super->migr_rec->dest_1st_member_lba =
		__cpu_to_le32(curr_migr_unit *
			      __le32_to_cpu(super->migr_rec->dest_depth_per_unit));
	if (write_imsm_migr_rec(st) < 0) {
		dprintf("imsm: Cannot write migration record "
			"outside backup area\n");
		return 1;
	}

	return 0;
}

/*******************************************************************************
 * Function:	recover_backup_imsm
 * Description:	Function recovers critical data from the Migration Copy Area
 *		while assembling an array.
 * Parameters:
 *	super	: imsm internal array info
 *	info	: general array info
 * Returns:
 *	0 : success (or there is no data to recover)
 *	1 : fail
 ******************************************************************************/
int recover_backup_imsm(struct supertype *st, struct mdinfo *info)
{
	struct intel_super *super = st->sb;
	struct migr_record *migr_rec = super->migr_rec;
	struct imsm_map *map_dest = NULL;
	struct intel_dev *id = NULL;
	unsigned long long read_offset;
	unsigned long long write_offset;
	unsigned unit_len;
	int *targets = NULL;
	int new_disks, i, err;
	char *buf = NULL;
	int retval = 1;
	unsigned long curr_migr_unit = __le32_to_cpu(migr_rec->curr_migr_unit);
	unsigned long num_migr_units = __le32_to_cpu(migr_rec->num_migr_units);
	char buffer[20];
	int skipped_disks = 0;
	int max_degradation;

	err = sysfs_get_str(info, NULL, "array_state", (char *)buffer, 20);
	if (err < 1)
		return 1;

	/* recover data only during assemblation */
	if (strncmp(buffer, "inactive", 8) != 0)
		return 0;
	/* no data to recover */
	if (__le32_to_cpu(migr_rec->rec_status) == UNIT_SRC_NORMAL)
		return 0;
	if (curr_migr_unit >= num_migr_units)
		return 1;

	/* find device during reshape */
	for (id = super->devlist; id; id = id->next)
		if (is_gen_migration(id->dev))
			break;
	if (id == NULL)
		return 1;

	map_dest = get_imsm_map(id->dev, 0);
	new_disks = map_dest->num_members;
	max_degradation = new_disks - imsm_num_data_members(id->dev, 0);

	read_offset = (unsigned long long)
			__le32_to_cpu(migr_rec->ckpt_area_pba) * 512;

	write_offset = ((unsigned long long)
			__le32_to_cpu(migr_rec->dest_1st_member_lba) +
			__le32_to_cpu(map_dest->pba_of_lba0)) * 512;

	unit_len = __le32_to_cpu(migr_rec->dest_depth_per_unit) * 512;
	if (posix_memalign((void **)&buf, 512, unit_len) != 0)
		goto abort;
	targets = malloc(new_disks * sizeof(int));
	if (!targets)
		goto abort;

	open_backup_targets(info, new_disks, targets);

	for (i = 0; i < new_disks; i++) {
		if (targets[i] < 0) {
			skipped_disks++;
			continue;
		}
		if (lseek64(targets[i], read_offset, SEEK_SET) < 0) {
			fprintf(stderr,
				Name ": Cannot seek to block: %s\n",
				strerror(errno));
			goto abort;
		}
		if ((unsigned)read(targets[i], buf, unit_len) != unit_len) {
			fprintf(stderr,
				Name ": Cannot read copy area block: %s\n",
				strerror(errno));
			goto abort;
		}
		if (lseek64(targets[i], write_offset, SEEK_SET) < 0) {
			fprintf(stderr,
				Name ": Cannot seek to block: %s\n",
				strerror(errno));
			goto abort;
		}
		if ((unsigned)write(targets[i], buf, unit_len) != unit_len) {
			fprintf(stderr,
				Name ": Cannot restore block: %s\n",
				strerror(errno));
			goto abort;
		}
	}

	if (skipped_disks > max_degradation) {
		fprintf(stderr,
			Name ": Cannot restore data from backup."
			" Too many failed disks\n");
		goto abort;
	}

	if (save_checkpoint_imsm(st, info, UNIT_SRC_NORMAL)) {
		/* ignore error == 2, this can mean end of reshape here
		 */
		dprintf("imsm: Cannot write checkpoint to "
			"migration record (UNIT_SRC_NORMAL) during restart\n");
	} else
		retval = 0;

abort:
	if (targets) {
		for (i = 0; i < new_disks; i++)
			if (targets[i])
				close(targets[i]);
		free(targets);
	}
	free(buf);
	return retval;
}

static char disk_by_path[] = "/dev/disk/by-path/";

static const char *imsm_get_disk_controller_domain(const char *path)
{
	char disk_path[PATH_MAX];
	char *drv=NULL;
	struct stat st;

	strncpy(disk_path, disk_by_path, PATH_MAX - 1);
	strncat(disk_path, path, PATH_MAX - strlen(disk_path) - 1);
	if (stat(disk_path, &st) == 0) {
		struct sys_dev* hba;
		char *path=NULL;

		path = devt_to_devpath(st.st_rdev);
		if (path == NULL)
			return "unknown";
		hba = find_disk_attached_hba(-1, path);
		if (hba && hba->type == SYS_DEV_SAS)
			drv = "isci";
		else if (hba && hba->type == SYS_DEV_SATA)
			drv = "ahci";
		else 
			drv = "unknown";
		dprintf("path: %s hba: %s attached: %s\n",
			path, (hba) ? hba->path : "NULL", drv);
		free(path);
		if (hba)
			free_sys_dev(&hba);
	}
	return drv;
}

static int imsm_find_array_minor_by_subdev(int subdev, int container, int *minor)
{
	char subdev_name[20];
	struct mdstat_ent *mdstat;

	sprintf(subdev_name, "%d", subdev);
	mdstat = mdstat_by_subdev(subdev_name, container);
	if (!mdstat)
		return -1;

	*minor = mdstat->devnum;
	free_mdstat(mdstat);
	return 0;
}

static int imsm_reshape_is_allowed_on_container(struct supertype *st,
						struct geo_params *geo,
						int *old_raid_disks)
{
	/* currently we only support increasing the number of devices
	 * for a container.  This increases the number of device for each
	 * member array.  They must all be RAID0 or RAID5.
	 */
	int ret_val = 0;
	struct mdinfo *info, *member;
	int devices_that_can_grow = 0;

	dprintf("imsm: imsm_reshape_is_allowed_on_container(ENTER): "
		"st->devnum = (%i)\n",
		st->devnum);

	if (geo->size != -1 ||
	    geo->level != UnSet ||
	    geo->layout != UnSet ||
	    geo->chunksize != 0 ||
	    geo->raid_disks == UnSet) {
		dprintf("imsm: Container operation is allowed for "
			"raid disks number change only.\n");
		return ret_val;
	}

	info = container_content_imsm(st, NULL);
	for (member = info; member; member = member->next) {
		int result;
		int minor;

		dprintf("imsm: checking device_num: %i\n",
			member->container_member);

		if (geo->raid_disks <= member->array.raid_disks) {
			/* we work on container for Online Capacity Expansion
			 * only so raid_disks has to grow
			 */
			dprintf("imsm: for container operation raid disks "
				"increase is required\n");
			break;
		}

		if ((info->array.level != 0) &&
		    (info->array.level != 5)) {
			/* we cannot use this container with other raid level
			 */
			dprintf("imsm: for container operation wrong"
				" raid level (%i) detected\n",
				info->array.level);
			break;
		} else {
			/* check for platform support
			 * for this raid level configuration
			 */
			struct intel_super *super = st->sb;
			if (!is_raid_level_supported(super->orom,
						     member->array.level,
						     geo->raid_disks)) {
				dprintf("platform does not support raid%d with"
					" %d disk%s\n",
					 info->array.level,
					 geo->raid_disks,
					 geo->raid_disks > 1 ? "s" : "");
				break;
			}
			/* check if component size is aligned to chunk size
			 */
			if (info->component_size %
			    (info->array.chunk_size/512)) {
				dprintf("Component size is not aligned to "
					"chunk size\n");
				break;
			}
		}

		if (*old_raid_disks &&
		    info->array.raid_disks != *old_raid_disks)
			break;
		*old_raid_disks = info->array.raid_disks;

		/* All raid5 and raid0 volumes in container
		 * have to be ready for Online Capacity Expansion
		 * so they need to be assembled.  We have already
		 * checked that no recovery etc is happening.
		 */
		result = imsm_find_array_minor_by_subdev(member->container_member,
							 st->container_dev,
							 &minor);
		if (result < 0) {
			dprintf("imsm: cannot find array\n");
			break;
		}
		devices_that_can_grow++;
	}
	sysfs_free(info);
	if (!member && devices_that_can_grow)
		ret_val = 1;

	if (ret_val)
		dprintf("\tContainer operation allowed\n");
	else
		dprintf("\tError: %i\n", ret_val);

	return ret_val;
}

/* Function: get_spares_for_grow
 * Description: Allocates memory and creates list of spare devices
 * 		avaliable in container. Checks if spare drive size is acceptable.
 * Parameters: Pointer to the supertype structure
 * Returns: Pointer to the list of spare devices (mdinfo structure) on success,
 * 		NULL if fail
 */
static struct mdinfo *get_spares_for_grow(struct supertype *st)
{
	unsigned long long min_size = min_acceptable_spare_size_imsm(st);
	return container_choose_spares(st, min_size, NULL, NULL, NULL, 0);
}

/******************************************************************************
 * function: imsm_create_metadata_update_for_reshape
 * Function creates update for whole IMSM container.
 *
 ******************************************************************************/
static int imsm_create_metadata_update_for_reshape(
	struct supertype *st,
	struct geo_params *geo,
	int old_raid_disks,
	struct imsm_update_reshape **updatep)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	int update_memory_size = 0;
	struct imsm_update_reshape *u = NULL;
	struct mdinfo *spares = NULL;
	int i;
	int delta_disks = 0;
	struct mdinfo *dev;

	dprintf("imsm_update_metadata_for_reshape(enter) raid_disks = %i\n",
		geo->raid_disks);

	delta_disks = geo->raid_disks - old_raid_disks;

	/* size of all update data without anchor */
	update_memory_size = sizeof(struct imsm_update_reshape);

	/* now add space for spare disks that we need to add. */
	update_memory_size += sizeof(u->new_disks[0]) * (delta_disks - 1);

	u = calloc(1, update_memory_size);
	if (u == NULL) {
		dprintf("error: "
			"cannot get memory for imsm_update_reshape update\n");
		return 0;
	}
	u->type = update_reshape_container_disks;
	u->old_raid_disks = old_raid_disks;
	u->new_raid_disks = geo->raid_disks;

	/* now get spare disks list
	 */
	spares = get_spares_for_grow(st);

	if (spares == NULL
	    || delta_disks > spares->array.spare_disks) {
		fprintf(stderr, Name ": imsm: ERROR: Cannot get spare devices "
			"for %s.\n", geo->dev_name);
		i = -1;
		goto abort;
	}

	/* we have got spares
	 * update disk list in imsm_disk list table in anchor
	 */
	dprintf("imsm: %i spares are available.\n\n",
		spares->array.spare_disks);

	dev = spares->devs;
	for (i = 0; i < delta_disks; i++) {
		struct dl *dl;

		if (dev == NULL)
			break;
		u->new_disks[i] = makedev(dev->disk.major,
					  dev->disk.minor);
		dl = get_disk_super(super, dev->disk.major, dev->disk.minor);
		dl->index = mpb->num_disks;
		mpb->num_disks++;
		dev = dev->next;
	}

abort:
	/* free spares
	 */
	sysfs_free(spares);

	dprintf("imsm: reshape update preparation :");
	if (i == delta_disks) {
		dprintf(" OK\n");
		*updatep = u;
		return update_memory_size;
	}
	free(u);
	dprintf(" Error\n");

	return 0;
}

/******************************************************************************
 * function: imsm_create_metadata_update_for_migration()
 *           Creates update for IMSM array.
 *
 ******************************************************************************/
static int imsm_create_metadata_update_for_migration(
					struct supertype *st,
					struct geo_params *geo,
					struct imsm_update_reshape_migration **updatep)
{
	struct intel_super *super = st->sb;
	int update_memory_size = 0;
	struct imsm_update_reshape_migration *u = NULL;
	struct imsm_dev *dev;
	int previous_level = -1;

	dprintf("imsm_create_metadata_update_for_migration(enter)"
		" New Level = %i\n", geo->level);

	/* size of all update data without anchor */
	update_memory_size = sizeof(struct imsm_update_reshape_migration);

	u = calloc(1, update_memory_size);
	if (u == NULL) {
		dprintf("error: cannot get memory for "
			"imsm_create_metadata_update_for_migration\n");
		return 0;
	}
	u->type = update_reshape_migration;
	u->subdev = super->current_vol;
	u->new_level = geo->level;
	u->new_layout = geo->layout;
	u->new_raid_disks = u->old_raid_disks = geo->raid_disks;
	u->new_disks[0] = -1;
	u->new_chunksize = -1;

	dev = get_imsm_dev(super, u->subdev);
	if (dev) {
		struct imsm_map *map;

		map = get_imsm_map(dev, 0);
		if (map) {
			int current_chunk_size =
				__le16_to_cpu(map->blocks_per_strip) / 2;

			if (geo->chunksize != current_chunk_size) {
				u->new_chunksize = geo->chunksize / 1024;
				dprintf("imsm: "
					"chunk size change from %i to %i\n",
					current_chunk_size, u->new_chunksize);
			}
			previous_level = map->raid_level;
		}
	}
	if ((geo->level == 5) && (previous_level == 0)) {
		struct mdinfo *spares = NULL;

		u->new_raid_disks++;
		spares = get_spares_for_grow(st);
		if ((spares == NULL) || (spares->array.spare_disks < 1)) {
			free(u);
			sysfs_free(spares);
			update_memory_size = 0;
			dprintf("error: cannot get spare device "
				"for requested migration");
			return 0;
		}
		sysfs_free(spares);
	}
	dprintf("imsm: reshape update preparation : OK\n");
	*updatep = u;

	return update_memory_size;
}

static void imsm_update_metadata_locally(struct supertype *st,
					 void *buf, int len)
{
	struct metadata_update mu;

	mu.buf = buf;
	mu.len = len;
	mu.space = NULL;
	mu.space_list = NULL;
	mu.next = NULL;
	imsm_prepare_update(st, &mu);
	imsm_process_update(st, &mu);

	while (mu.space_list) {
		void **space = mu.space_list;
		mu.space_list = *space;
		free(space);
	}
}

/***************************************************************************
* Function:	imsm_analyze_change
* Description:	Function analyze change for single volume
* 		and validate if transition is supported
* Parameters:	Geometry parameters, supertype structure
* Returns:	Operation type code on success, -1 if fail
****************************************************************************/
enum imsm_reshape_type imsm_analyze_change(struct supertype *st,
					   struct geo_params *geo)
{
	struct mdinfo info;
	int change = -1;
	int check_devs = 0;
	int chunk;

	getinfo_super_imsm_volume(st, &info, NULL);
	if ((geo->level != info.array.level) &&
	    (geo->level >= 0) &&
	    (geo->level != UnSet)) {
		switch (info.array.level) {
		case 0:
			if (geo->level == 5) {
				change = CH_MIGRATION;
				if (geo->layout != ALGORITHM_LEFT_ASYMMETRIC) {
					fprintf(stderr,
					Name " Error. Requested Layout "
					"not supported (left-asymmetric layout "
					"is supported only)!\n");
					change = -1;
					goto analyse_change_exit;
				}
				check_devs = 1;
			}
			if (geo->level == 10) {
				change = CH_TAKEOVER;
				check_devs = 1;
			}
			break;
		case 1:
			if (geo->level == 0) {
				change = CH_TAKEOVER;
				check_devs = 1;
			}
			break;
		case 10:
			if (geo->level == 0) {
				change = CH_TAKEOVER;
				check_devs = 1;
			}
			break;
		}
		if (change == -1) {
			fprintf(stderr,
				Name " Error. Level Migration from %d to %d "
				"not supported!\n",
				info.array.level, geo->level);
			goto analyse_change_exit;
		}
	} else
		geo->level = info.array.level;

	if ((geo->layout != info.array.layout)
	    && ((geo->layout != UnSet) && (geo->layout != -1))) {
		change = CH_MIGRATION;
		if ((info.array.layout == 0)
		    && (info.array.level == 5)
		    && (geo->layout == 5)) {
			/* reshape 5 -> 4 */
		} else if ((info.array.layout == 5)
			   && (info.array.level == 5)
			   && (geo->layout == 0)) {
			/* reshape 4 -> 5 */
			geo->layout = 0;
			geo->level = 5;
		} else {
			fprintf(stderr,
				Name " Error. Layout Migration from %d to %d "
				"not supported!\n",
				info.array.layout, geo->layout);
			change = -1;
			goto analyse_change_exit;
		}
	} else
		geo->layout = info.array.layout;

	if ((geo->chunksize > 0) && (geo->chunksize != UnSet)
	    && (geo->chunksize != info.array.chunk_size))
		change = CH_MIGRATION;
	else
		geo->chunksize = info.array.chunk_size;

	chunk = geo->chunksize / 1024;
	if (!validate_geometry_imsm(st,
				    geo->level,
				    geo->layout,
				    geo->raid_disks,
				    &chunk,
				    geo->size,
				    0, 0, 1))
		change = -1;

	if (check_devs) {
		struct intel_super *super = st->sb;
		struct imsm_super *mpb = super->anchor;

		if (mpb->num_raid_devs > 1) {
			fprintf(stderr,
				Name " Error. Cannot perform operation on %s"
				"- for this operation it MUST be single "
				"array in container\n",
				geo->dev_name);
			change = -1;
		}
	}

analyse_change_exit:

	return change;
}

int imsm_takeover(struct supertype *st, struct geo_params *geo)
{
	struct intel_super *super = st->sb;
	struct imsm_update_takeover *u;

	u = malloc(sizeof(struct imsm_update_takeover));
	if (u == NULL)
		return 1;

	u->type = update_takeover;
	u->subarray = super->current_vol;

	/* 10->0 transition */
	if (geo->level == 0)
		u->direction = R10_TO_R0;

	/* 0->10 transition */
	if (geo->level == 10)
		u->direction = R0_TO_R10;

	/* update metadata locally */
	imsm_update_metadata_locally(st, u,
					sizeof(struct imsm_update_takeover));
	/* and possibly remotely */
	if (st->update_tail)
		append_metadata_update(st, u,
					sizeof(struct imsm_update_takeover));
	else
		free(u);

	return 0;
}

static int imsm_reshape_super(struct supertype *st, long long size, int level,
			      int layout, int chunksize, int raid_disks,
			      int delta_disks, char *backup, char *dev,
			      int verbose)
{
	int ret_val = 1;
	struct geo_params geo;

	dprintf("imsm: reshape_super called.\n");

	memset(&geo, 0, sizeof(struct geo_params));

	geo.dev_name = dev;
	geo.dev_id = st->devnum;
	geo.size = size;
	geo.level = level;
	geo.layout = layout;
	geo.chunksize = chunksize;
	geo.raid_disks = raid_disks;
	if (delta_disks != UnSet)
		geo.raid_disks += delta_disks;

	dprintf("\tfor level      : %i\n", geo.level);
	dprintf("\tfor raid_disks : %i\n", geo.raid_disks);

	if (experimental() == 0)
		return ret_val;

	if (st->container_dev == st->devnum) {
		/* On container level we can only increase number of devices. */
		dprintf("imsm: info: Container operation\n");
		int old_raid_disks = 0;

		if (imsm_reshape_is_allowed_on_container(
			    st, &geo, &old_raid_disks)) {
			struct imsm_update_reshape *u = NULL;
			int len;

			len = imsm_create_metadata_update_for_reshape(
				st, &geo, old_raid_disks, &u);

			if (len <= 0) {
				dprintf("imsm: Cannot prepare update\n");
				goto exit_imsm_reshape_super;
			}

			ret_val = 0;
			/* update metadata locally */
			imsm_update_metadata_locally(st, u, len);
			/* and possibly remotely */
			if (st->update_tail)
				append_metadata_update(st, u, len);
			else
				free(u);

		} else {
			fprintf(stderr, Name ": (imsm) Operation "
				"is not allowed on this container\n");
		}
	} else {
		/* On volume level we support following operations
		 * - takeover: raid10 -> raid0; raid0 -> raid10
		 * - chunk size migration
		 * - migration: raid5 -> raid0; raid0 -> raid5
		 */
		struct intel_super *super = st->sb;
		struct intel_dev *dev = super->devlist;
		int change, devnum;
		dprintf("imsm: info: Volume operation\n");
		/* find requested device */
		while (dev) {
			if (imsm_find_array_minor_by_subdev(
				    dev->index, st->container_dev, &devnum) == 0
			    && devnum == geo.dev_id)
				break;
			dev = dev->next;
		}
		if (dev == NULL) {
			fprintf(stderr, Name " Cannot find %s (%i) subarray\n",
				geo.dev_name, geo.dev_id);
			goto exit_imsm_reshape_super;
		}
		super->current_vol = dev->index;
		change = imsm_analyze_change(st, &geo);
		switch (change) {
		case CH_TAKEOVER:
			ret_val = imsm_takeover(st, &geo);
			break;
		case CH_MIGRATION: {
			struct imsm_update_reshape_migration *u = NULL;
			int len =
				imsm_create_metadata_update_for_migration(
					st, &geo, &u);
			if (len < 1) {
				dprintf("imsm: "
					"Cannot prepare update\n");
				break;
			}
			ret_val = 0;
			/* update metadata locally */
			imsm_update_metadata_locally(st, u, len);
			/* and possibly remotely */
			if (st->update_tail)
				append_metadata_update(st, u, len);
			else
				free(u);
		}
		break;
		default:
			ret_val = 1;
		}
	}

exit_imsm_reshape_super:
	dprintf("imsm: reshape_super Exit code = %i\n", ret_val);
	return ret_val;
}

/*******************************************************************************
 * Function:	wait_for_reshape_imsm
 * Description:	Function writes new sync_max value and waits until
 *		reshape process reach new position
 * Parameters:
 *	sra		: general array info
 *	ndata		: number of disks in new array's layout
 * Returns:
 *	 0 : success,
 *	 1 : there is no reshape in progress,
 *	-1 : fail
 ******************************************************************************/
int wait_for_reshape_imsm(struct mdinfo *sra, int ndata)
{
	int fd = sysfs_get_fd(sra, NULL, "reshape_position");
	unsigned long long completed;
	/* to_complete : new sync_max position */
	unsigned long long to_complete = sra->reshape_progress;
	unsigned long long position_to_set = to_complete / ndata;

	if (fd < 0) {
		dprintf("imsm: wait_for_reshape_imsm() "
			"cannot open reshape_position\n");
		return 1;
	}

	if (sysfs_fd_get_ll(fd, &completed) < 0) {
		dprintf("imsm: wait_for_reshape_imsm() "
			"cannot read reshape_position (no reshape in progres)\n");
		close(fd);
		return 0;
	}

	if (completed > to_complete) {
		dprintf("imsm: wait_for_reshape_imsm() "
			"wrong next position to set %llu (%llu)\n",
			to_complete, completed);
		close(fd);
		return -1;
	}
	dprintf("Position set: %llu\n", position_to_set);
	if (sysfs_set_num(sra, NULL, "sync_max",
			  position_to_set) != 0) {
		dprintf("imsm: wait_for_reshape_imsm() "
			"cannot set reshape position to %llu\n",
			position_to_set);
		close(fd);
		return -1;
	}

	do {
		char action[20];
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		select(fd+1, &rfds, NULL, NULL, NULL);
		if (sysfs_get_str(sra, NULL, "sync_action",
				  action, 20) > 0 &&
				strncmp(action, "reshape", 7) != 0)
			break;
		if (sysfs_fd_get_ll(fd, &completed) < 0) {
			dprintf("imsm: wait_for_reshape_imsm() "
				"cannot read reshape_position (in loop)\n");
			close(fd);
			return 1;
		}
	} while (completed < to_complete);
	close(fd);
	return 0;

}

/*******************************************************************************
 * Function:	check_degradation_change
 * Description:	Check that array hasn't become failed.
 * Parameters:
 *	info	: for sysfs access
 *	sources	: source disks descriptors
 *	degraded: previous degradation level
 * Returns:
 *	degradation level
 ******************************************************************************/
int check_degradation_change(struct mdinfo *info,
			     int *sources,
			     int degraded)
{
	unsigned long long new_degraded;
	sysfs_get_ll(info, NULL, "degraded", &new_degraded);
	if (new_degraded != (unsigned long long)degraded) {
		/* check each device to ensure it is still working */
		struct mdinfo *sd;
		new_degraded = 0;
		for (sd = info->devs ; sd ; sd = sd->next) {
			if (sd->disk.state & (1<<MD_DISK_FAULTY))
				continue;
			if (sd->disk.state & (1<<MD_DISK_SYNC)) {
				char sbuf[20];
				if (sysfs_get_str(info,
					sd, "state", sbuf, 20) < 0 ||
					strstr(sbuf, "faulty") ||
					strstr(sbuf, "in_sync") == NULL) {
					/* this device is dead */
					sd->disk.state = (1<<MD_DISK_FAULTY);
					if (sd->disk.raid_disk >= 0 &&
					    sources[sd->disk.raid_disk] >= 0) {
						close(sources[
							sd->disk.raid_disk]);
						sources[sd->disk.raid_disk] =
							-1;
					}
					new_degraded++;
				}
			}
		}
	}

	return new_degraded;
}

/*******************************************************************************
 * Function:	imsm_manage_reshape
 * Description:	Function finds array under reshape and it manages reshape
 *		process. It creates stripes backups (if required) and sets
 *		checheckpoits.
 * Parameters:
 *	afd		: Backup handle (nattive) - not used
 *	sra		: general array info
 *	reshape		: reshape parameters - not used
 *	st		: supertype structure
 *	blocks		: size of critical section [blocks]
 *	fds		: table of source device descriptor
 *	offsets		: start of array (offest per devices)
 *	dests		: not used
 *	destfd		: table of destination device descriptor
 *	destoffsets	: table of destination offsets (per device)
 * Returns:
 *	1 : success, reshape is done
 *	0 : fail
 ******************************************************************************/
static int imsm_manage_reshape(
	int afd, struct mdinfo *sra, struct reshape *reshape,
	struct supertype *st, unsigned long backup_blocks,
	int *fds, unsigned long long *offsets,
	int dests, int *destfd, unsigned long long *destoffsets)
{
	int ret_val = 0;
	struct intel_super *super = st->sb;
	struct intel_dev *dv = NULL;
	struct imsm_dev *dev = NULL;
	struct imsm_map *map_src;
	int migr_vol_qan = 0;
	int ndata, odata; /* [bytes] */
	int chunk; /* [bytes] */
	struct migr_record *migr_rec;
	char *buf = NULL;
	unsigned int buf_size; /* [bytes] */
	unsigned long long max_position; /* array size [bytes] */
	unsigned long long next_step; /* [blocks]/[bytes] */
	unsigned long long old_data_stripe_length;
	unsigned long long start_src; /* [bytes] */
	unsigned long long start; /* [bytes] */
	unsigned long long start_buf_shift; /* [bytes] */
	int degraded = 0;
	int source_layout = 0;

	if (!fds || !offsets || !sra)
		goto abort;

	/* Find volume during the reshape */
	for (dv = super->devlist; dv; dv = dv->next) {
		if (dv->dev->vol.migr_type == MIGR_GEN_MIGR
		    && dv->dev->vol.migr_state == 1) {
			dev = dv->dev;
			migr_vol_qan++;
		}
	}
	/* Only one volume can migrate at the same time */
	if (migr_vol_qan != 1) {
		fprintf(stderr, Name " : %s", migr_vol_qan ?
			"Number of migrating volumes greater than 1\n" :
			"There is no volume during migrationg\n");
		goto abort;
	}

	map_src = get_imsm_map(dev, 1);
	if (map_src == NULL)
		goto abort;

	ndata = imsm_num_data_members(dev, 0);
	odata = imsm_num_data_members(dev, 1);

	chunk = __le16_to_cpu(map_src->blocks_per_strip) * 512;
	old_data_stripe_length = odata * chunk;

	migr_rec = super->migr_rec;

	/* initialize migration record for start condition */
	if (sra->reshape_progress == 0)
		init_migr_record_imsm(st, dev, sra);
	else {
		if (__le32_to_cpu(migr_rec->rec_status) != UNIT_SRC_NORMAL) {
			dprintf("imsm: cannot restart migration when data "
				"are present in copy area.\n");
			goto abort;
		}
	}

	/* size for data */
	buf_size = __le32_to_cpu(migr_rec->blocks_per_unit) * 512;
	/* extend  buffer size for parity disk */
	buf_size += __le32_to_cpu(migr_rec->dest_depth_per_unit) * 512;
	/* add space for stripe aligment */
	buf_size += old_data_stripe_length;
	if (posix_memalign((void **)&buf, 4096, buf_size)) {
		dprintf("imsm: Cannot allocate checpoint buffer\n");
		goto abort;
	}

	max_position = sra->component_size * ndata;
	source_layout = imsm_level_to_layout(map_src->raid_level);

	while (__le32_to_cpu(migr_rec->curr_migr_unit) <
	       __le32_to_cpu(migr_rec->num_migr_units)) {
		/* current reshape position [blocks] */
		unsigned long long current_position =
			__le32_to_cpu(migr_rec->blocks_per_unit)
			* __le32_to_cpu(migr_rec->curr_migr_unit);
		unsigned long long border;

		/* Check that array hasn't become failed.
		 */
		degraded = check_degradation_change(sra, fds, degraded);
		if (degraded > 1) {
			dprintf("imsm: Abort reshape due to degradation"
				" level (%i)\n", degraded);
			goto abort;
		}

		next_step = __le32_to_cpu(migr_rec->blocks_per_unit);

		if ((current_position + next_step) > max_position)
			next_step = max_position - current_position;

		start = current_position * 512;

		/* allign reading start to old geometry */
		start_buf_shift = start % old_data_stripe_length;
		start_src = start - start_buf_shift;

		border = (start_src / odata) - (start / ndata);
		border /= 512;
		if (border <= __le32_to_cpu(migr_rec->dest_depth_per_unit)) {
			/* save critical stripes to buf
			 * start     - start address of current unit
			 *             to backup [bytes]
			 * start_src - start address of current unit
			 *             to backup alligned to source array
			 *             [bytes]
			 */
			unsigned long long next_step_filler = 0;
			unsigned long long copy_length = next_step * 512;

			/* allign copy area length to stripe in old geometry */
			next_step_filler = ((copy_length + start_buf_shift)
					    % old_data_stripe_length);
			if (next_step_filler)
				next_step_filler = (old_data_stripe_length
						    - next_step_filler);
			dprintf("save_stripes() parameters: start = %llu,"
				"\tstart_src = %llu,\tnext_step*512 = %llu,"
				"\tstart_in_buf_shift = %llu,"
				"\tnext_step_filler = %llu\n",
				start, start_src, copy_length,
				start_buf_shift, next_step_filler);

			if (save_stripes(fds, offsets, map_src->num_members,
					 chunk, map_src->raid_level,
					 source_layout, 0, NULL, start_src,
					 copy_length +
					 next_step_filler + start_buf_shift,
					 buf)) {
				dprintf("imsm: Cannot save stripes"
					" to buffer\n");
				goto abort;
			}
			/* Convert data to destination format and store it
			 * in backup general migration area
			 */
			if (save_backup_imsm(st, dev, sra,
				buf + start_buf_shift, copy_length)) {
				dprintf("imsm: Cannot save stripes to "
					"target devices\n");
				goto abort;
			}
			if (save_checkpoint_imsm(st, sra,
						 UNIT_SRC_IN_CP_AREA)) {
				dprintf("imsm: Cannot write checkpoint to "
					"migration record (UNIT_SRC_IN_CP_AREA)\n");
				goto abort;
			}
		} else {
			/* set next step to use whole border area */
			border /= next_step;
			if (border > 1)
				next_step *= border;
		}
		/* When data backed up, checkpoint stored,
		 * kick the kernel to reshape unit of data
		 */
		next_step = next_step + sra->reshape_progress;
		/* limit next step to array max position */
		if (next_step > max_position)
			next_step = max_position;
		sysfs_set_num(sra, NULL, "suspend_lo", sra->reshape_progress);
		sysfs_set_num(sra, NULL, "suspend_hi", next_step);
		sra->reshape_progress = next_step;

		/* wait until reshape finish */
		if (wait_for_reshape_imsm(sra, ndata) < 0) {
			dprintf("wait_for_reshape_imsm returned error!\n");
			goto abort;
		}

		if (save_checkpoint_imsm(st, sra, UNIT_SRC_NORMAL) == 1) {
			/* ignore error == 2, this can mean end of reshape here
			 */
			dprintf("imsm: Cannot write checkpoint to "
				"migration record (UNIT_SRC_NORMAL)\n");
			goto abort;
		}

	}

	/* return '1' if done */
	ret_val = 1;
abort:
	free(buf);
	abort_reshape(sra);

	return ret_val;
}
#endif /* MDASSEMBLE */

struct superswitch super_imsm = {
#ifndef	MDASSEMBLE
	.examine_super	= examine_super_imsm,
	.brief_examine_super = brief_examine_super_imsm,
	.brief_examine_subarrays = brief_examine_subarrays_imsm,
	.export_examine_super = export_examine_super_imsm,
	.detail_super	= detail_super_imsm,
	.brief_detail_super = brief_detail_super_imsm,
	.write_init_super = write_init_super_imsm,
	.validate_geometry = validate_geometry_imsm,
	.add_to_super	= add_to_super_imsm,
	.remove_from_super = remove_from_super_imsm,
	.detail_platform = detail_platform_imsm,
	.kill_subarray = kill_subarray_imsm,
	.update_subarray = update_subarray_imsm,
	.load_container	= load_container_imsm,
	.default_geometry = default_geometry_imsm,
	.get_disk_controller_domain = imsm_get_disk_controller_domain,
	.reshape_super  = imsm_reshape_super,
	.manage_reshape = imsm_manage_reshape,
	.recover_backup = recover_backup_imsm,
#endif
	.match_home	= match_home_imsm,
	.uuid_from_super= uuid_from_super_imsm,
	.getinfo_super  = getinfo_super_imsm,
	.getinfo_super_disks = getinfo_super_disks_imsm,
	.update_super	= update_super_imsm,

	.avail_size	= avail_size_imsm,
	.min_acceptable_spare_size = min_acceptable_spare_size_imsm,

	.compare_super	= compare_super_imsm,

	.load_super	= load_super_imsm,
	.init_super	= init_super_imsm,
	.store_super	= store_super_imsm,
	.free_super	= free_super_imsm,
	.match_metadata_desc = match_metadata_desc_imsm,
	.container_content = container_content_imsm,


	.external	= 1,
	.name = "imsm",

#ifndef MDASSEMBLE
/* for mdmon */
	.open_new	= imsm_open_new,
	.set_array_state= imsm_set_array_state,
	.set_disk	= imsm_set_disk,
	.sync_metadata	= imsm_sync_metadata,
	.activate_spare = imsm_activate_spare,
	.process_update = imsm_process_update,
	.prepare_update = imsm_prepare_update,
#endif /* MDASSEMBLE */
};
