/*
 * Copyright 2000 by Hans Reiser, licensing governed by reiserfs/README
 *
 * Trivial changes by Alan Cox to add the LFS fixes
 *
 * Trivial Changes:
 * Rights granted to Hans Reiser to redistribute under other terms providing
 * he accepts all liability including but not limited to patent, fitness
 * for purpose, and direct or indirect claims arising from failure to perform.
 *
 * NO WARRANTY
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/time.h>
#include <asm/uaccess.h>
#include "reiserfs.h"
#include "acl.h"
#include "xattr.h"
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/quotaops.h>
#include <linux/vfs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/crc32.h>
#include <linux/seq_file.h>

struct file_system_type reiserfs_fs_type;

static const char reiserfs_3_5_magic_string[] = REISERFS_SUPER_MAGIC_STRING;
static const char reiserfs_3_6_magic_string[] = REISER2FS_SUPER_MAGIC_STRING;
static const char reiserfs_jr_magic_string[] = REISER2FS_JR_SUPER_MAGIC_STRING;

int is_reiserfs_3_5(struct reiserfs_super_block *rs)
{
	return !strncmp(rs->s_v1.s_magic, reiserfs_3_5_magic_string,
			strlen(reiserfs_3_5_magic_string));
}

int is_reiserfs_3_6(struct reiserfs_super_block *rs)
{
	return !strncmp(rs->s_v1.s_magic, reiserfs_3_6_magic_string,
			strlen(reiserfs_3_6_magic_string));
}

int is_reiserfs_jr(struct reiserfs_super_block *rs)
{
	return !strncmp(rs->s_v1.s_magic, reiserfs_jr_magic_string,
			strlen(reiserfs_jr_magic_string));
}

static int is_any_reiserfs_magic_string(struct reiserfs_super_block *rs)
{
	return (is_reiserfs_3_5(rs) || is_reiserfs_3_6(rs) ||
		is_reiserfs_jr(rs));
}

static int reiserfs_remount(struct super_block *s, int *flags, char *data);
static int reiserfs_statfs(struct dentry *dentry, struct kstatfs *buf);
void show_alloc_options(struct seq_file *seq, struct super_block *s);

static int reiserfs_sync_fs(struct super_block *s, int wait)
{
	struct reiserfs_transaction_handle th;

	reiserfs_write_lock(s);
	if (!journal_begin(&th, s, 1))
		if (!journal_end_sync(&th, s, 1))
			reiserfs_flush_old_commits(s);
	s->s_dirt = 0;	
	reiserfs_write_unlock(s);
	return 0;
}

static void reiserfs_write_super(struct super_block *s)
{
	reiserfs_sync_fs(s, 1);
}

static int reiserfs_freeze(struct super_block *s)
{
	struct reiserfs_transaction_handle th;
	reiserfs_write_lock(s);
	if (!(s->s_flags & MS_RDONLY)) {
		int err = journal_begin(&th, s, 1);
		if (err) {
			reiserfs_block_writes(&th);
		} else {
			reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s),
						     1);
			journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
			reiserfs_block_writes(&th);
			journal_end_sync(&th, s, 1);
		}
	}
	s->s_dirt = 0;
	reiserfs_write_unlock(s);
	return 0;
}

static int reiserfs_unfreeze(struct super_block *s)
{
	reiserfs_allow_writes(s);
	return 0;
}

extern const struct in_core_key MAX_IN_CORE_KEY;

static int remove_save_link_only(struct super_block *s,
				 struct reiserfs_key *key, int oid_free)
{
	struct reiserfs_transaction_handle th;
	int err;

	
	err = journal_begin(&th, s, JOURNAL_PER_BALANCE_CNT);
	if (err)
		return err;

	reiserfs_delete_solid_item(&th, NULL, key);
	if (oid_free)
		
		reiserfs_release_objectid(&th, le32_to_cpu(key->k_objectid));

	return journal_end(&th, s, JOURNAL_PER_BALANCE_CNT);
}

#ifdef CONFIG_QUOTA
static int reiserfs_quota_on_mount(struct super_block *, int);
#endif

static int finish_unfinished(struct super_block *s)
{
	INITIALIZE_PATH(path);
	struct cpu_key max_cpu_key, obj_key;
	struct reiserfs_key save_link_key, last_inode_key;
	int retval = 0;
	struct item_head *ih;
	struct buffer_head *bh;
	int item_pos;
	char *item;
	int done;
	struct inode *inode;
	int truncate;
#ifdef CONFIG_QUOTA
	int i;
	int ms_active_set;
	int quota_enabled[MAXQUOTAS];
#endif

	
	max_cpu_key.version = KEY_FORMAT_3_5;
	max_cpu_key.on_disk_key.k_dir_id = ~0U;
	max_cpu_key.on_disk_key.k_objectid = ~0U;
	set_cpu_key_k_offset(&max_cpu_key, ~0U);
	max_cpu_key.key_length = 3;

	memset(&last_inode_key, 0, sizeof(last_inode_key));

#ifdef CONFIG_QUOTA
	
	if (s->s_flags & MS_ACTIVE) {
		ms_active_set = 0;
	} else {
		ms_active_set = 1;
		s->s_flags |= MS_ACTIVE;
	}
	
	for (i = 0; i < MAXQUOTAS; i++) {
		quota_enabled[i] = 1;
		if (REISERFS_SB(s)->s_qf_names[i]) {
			int ret;

			if (sb_has_quota_active(s, i)) {
				quota_enabled[i] = 0;
				continue;
			}
			ret = reiserfs_quota_on_mount(s, i);
			if (ret < 0)
				reiserfs_warning(s, "reiserfs-2500",
						 "cannot turn on journaled "
						 "quota: error %d", ret);
		}
	}
#endif

	done = 0;
	REISERFS_SB(s)->s_is_unlinked_ok = 1;
	while (!retval) {
		retval = search_item(s, &max_cpu_key, &path);
		if (retval != ITEM_NOT_FOUND) {
			reiserfs_error(s, "vs-2140",
				       "search_by_key returned %d", retval);
			break;
		}

		bh = get_last_bh(&path);
		item_pos = get_item_pos(&path);
		if (item_pos != B_NR_ITEMS(bh)) {
			reiserfs_warning(s, "vs-2060",
					 "wrong position found");
			break;
		}
		item_pos--;
		ih = B_N_PITEM_HEAD(bh, item_pos);

		if (le32_to_cpu(ih->ih_key.k_dir_id) != MAX_KEY_OBJECTID)
			
			break;

		save_link_key = ih->ih_key;
		if (is_indirect_le_ih(ih))
			truncate = 1;
		else
			truncate = 0;

		
		item = B_I_PITEM(bh, ih);
		obj_key.on_disk_key.k_dir_id = le32_to_cpu(*(__le32 *) item);
		obj_key.on_disk_key.k_objectid =
		    le32_to_cpu(ih->ih_key.k_objectid);
		obj_key.on_disk_key.k_offset = 0;
		obj_key.on_disk_key.k_type = 0;

		pathrelse(&path);

		inode = reiserfs_iget(s, &obj_key);
		if (!inode) {
			reiserfs_warning(s, "vs-2180", "iget failed for %K",
					 &obj_key);
			retval = remove_save_link_only(s, &save_link_key, 1);
			continue;
		}

		if (!truncate && inode->i_nlink) {
			
			reiserfs_warning(s, "vs-2185",
					 "file %K is not unlinked",
					 &obj_key);
			retval = remove_save_link_only(s, &save_link_key, 0);
			continue;
		}
		dquot_initialize(inode);

		if (truncate && S_ISDIR(inode->i_mode)) {
			reiserfs_warning(s, "green-2101",
					 "impossible truncate on a "
					 "directory %k. Please report",
					 INODE_PKEY(inode));
			retval = remove_save_link_only(s, &save_link_key, 0);
			truncate = 0;
			iput(inode);
			continue;
		}

		if (truncate) {
			REISERFS_I(inode)->i_flags |=
			    i_link_saved_truncate_mask;
			reiserfs_info(s, "Truncating %k to %Ld ..",
				      INODE_PKEY(inode), inode->i_size);
			reiserfs_truncate_file(inode,
					       0
					       
					       );
			retval = remove_save_link(inode, truncate);
		} else {
			REISERFS_I(inode)->i_flags |= i_link_saved_unlink_mask;
			
			reiserfs_info(s, "Removing %k..", INODE_PKEY(inode));
			if (memcmp(&last_inode_key, INODE_PKEY(inode),
					sizeof(last_inode_key))){
				last_inode_key = *INODE_PKEY(inode);
				
				retval = 0;
			} else {
				reiserfs_warning(s, "super-2189", "Dead loop "
						 "in finish_unfinished "
						 "detected, just remove "
						 "save link\n");
				retval = remove_save_link_only(s,
							&save_link_key, 0);
			}
		}

		iput(inode);
		printk("done\n");
		done++;
	}
	REISERFS_SB(s)->s_is_unlinked_ok = 0;

#ifdef CONFIG_QUOTA
	
	for (i = 0; i < MAXQUOTAS; i++) {
		if (sb_dqopt(s)->files[i] && quota_enabled[i])
			dquot_quota_off(s, i);
	}
	if (ms_active_set)
		
		s->s_flags &= ~MS_ACTIVE;
#endif
	pathrelse(&path);
	if (done)
		reiserfs_info(s, "There were %d uncompleted unlinks/truncates. "
			      "Completed\n", done);
	return retval;
}

void add_save_link(struct reiserfs_transaction_handle *th,
		   struct inode *inode, int truncate)
{
	INITIALIZE_PATH(path);
	int retval;
	struct cpu_key key;
	struct item_head ih;
	__le32 link;

	BUG_ON(!th->t_trans_id);

	
	RFALSE(truncate &&
	       (REISERFS_I(inode)->i_flags & i_link_saved_truncate_mask),
	       "saved link already exists for truncated inode %lx",
	       (long)inode->i_ino);
	RFALSE(!truncate &&
	       (REISERFS_I(inode)->i_flags & i_link_saved_unlink_mask),
	       "saved link already exists for unlinked inode %lx",
	       (long)inode->i_ino);

	
	key.version = KEY_FORMAT_3_5;
	key.on_disk_key.k_dir_id = MAX_KEY_OBJECTID;
	key.on_disk_key.k_objectid = inode->i_ino;
	if (!truncate) {
		
		set_cpu_key_k_offset(&key, 1 + inode->i_sb->s_blocksize);
		set_cpu_key_k_type(&key, TYPE_DIRECT);

		
		make_le_item_head(&ih, &key, key.version,
				  1 + inode->i_sb->s_blocksize, TYPE_DIRECT,
				  4  , 0xffff  );
	} else {
		
		if (S_ISDIR(inode->i_mode))
			reiserfs_warning(inode->i_sb, "green-2102",
					 "Adding a truncate savelink for "
					 "a directory %k! Please report",
					 INODE_PKEY(inode));
		set_cpu_key_k_offset(&key, 1);
		set_cpu_key_k_type(&key, TYPE_INDIRECT);

		
		make_le_item_head(&ih, &key, key.version, 1, TYPE_INDIRECT,
				  4  , 0  );
	}
	key.key_length = 3;

	
	retval = search_item(inode->i_sb, &key, &path);
	if (retval != ITEM_NOT_FOUND) {
		if (retval != -ENOSPC)
			reiserfs_error(inode->i_sb, "vs-2100",
				       "search_by_key (%K) returned %d", &key,
				       retval);
		pathrelse(&path);
		return;
	}

	
	link = INODE_PKEY(inode)->k_dir_id;

	
	retval =
	    reiserfs_insert_item(th, &path, &key, &ih, NULL, (char *)&link);
	if (retval) {
		if (retval != -ENOSPC)
			reiserfs_error(inode->i_sb, "vs-2120",
				       "insert_item returned %d", retval);
	} else {
		if (truncate)
			REISERFS_I(inode)->i_flags |=
			    i_link_saved_truncate_mask;
		else
			REISERFS_I(inode)->i_flags |= i_link_saved_unlink_mask;
	}
}

int remove_save_link(struct inode *inode, int truncate)
{
	struct reiserfs_transaction_handle th;
	struct reiserfs_key key;
	int err;

	
	err = journal_begin(&th, inode->i_sb, JOURNAL_PER_BALANCE_CNT);
	if (err)
		return err;

	
	key.k_dir_id = cpu_to_le32(MAX_KEY_OBJECTID);
	key.k_objectid = INODE_PKEY(inode)->k_objectid;
	if (!truncate) {
		
		set_le_key_k_offset(KEY_FORMAT_3_5, &key,
				    1 + inode->i_sb->s_blocksize);
		set_le_key_k_type(KEY_FORMAT_3_5, &key, TYPE_DIRECT);
	} else {
		
		set_le_key_k_offset(KEY_FORMAT_3_5, &key, 1);
		set_le_key_k_type(KEY_FORMAT_3_5, &key, TYPE_INDIRECT);
	}

	if ((truncate &&
	     (REISERFS_I(inode)->i_flags & i_link_saved_truncate_mask)) ||
	    (!truncate &&
	     (REISERFS_I(inode)->i_flags & i_link_saved_unlink_mask)))
		
		reiserfs_delete_solid_item(&th, NULL, &key);
	if (!truncate) {
		reiserfs_release_objectid(&th, inode->i_ino);
		REISERFS_I(inode)->i_flags &= ~i_link_saved_unlink_mask;
	} else
		REISERFS_I(inode)->i_flags &= ~i_link_saved_truncate_mask;

	return journal_end(&th, inode->i_sb, JOURNAL_PER_BALANCE_CNT);
}

static void reiserfs_kill_sb(struct super_block *s)
{
	if (REISERFS_SB(s)) {
		shrink_dcache_sb(s);

		dput(REISERFS_SB(s)->xattr_root);
		REISERFS_SB(s)->xattr_root = NULL;
		dput(REISERFS_SB(s)->priv_root);
		REISERFS_SB(s)->priv_root = NULL;
	}

	kill_block_super(s);
}

static void reiserfs_put_super(struct super_block *s)
{
	struct reiserfs_transaction_handle th;
	th.t_trans_id = 0;

	dquot_disable(s, -1, DQUOT_USAGE_ENABLED | DQUOT_LIMITS_ENABLED);

	reiserfs_write_lock(s);

	if (s->s_dirt)
		reiserfs_write_super(s);

	
	if (!(s->s_flags & MS_RDONLY)) {
		if (!journal_begin(&th, s, 10)) {
			reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s),
						     1);
			set_sb_umount_state(SB_DISK_SUPER_BLOCK(s),
					    REISERFS_SB(s)->s_mount_state);
			journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
		}
	}

	journal_release(&th, s);

	reiserfs_free_bitmap_cache(s);

	brelse(SB_BUFFER_WITH_SB(s));

	print_statistics(s);

	if (REISERFS_SB(s)->reserved_blocks != 0) {
		reiserfs_warning(s, "green-2005", "reserved blocks left %d",
				 REISERFS_SB(s)->reserved_blocks);
	}

	reiserfs_proc_info_done(s);

	reiserfs_write_unlock(s);
	mutex_destroy(&REISERFS_SB(s)->lock);
	kfree(s->s_fs_info);
	s->s_fs_info = NULL;
}

static struct kmem_cache *reiserfs_inode_cachep;

static struct inode *reiserfs_alloc_inode(struct super_block *sb)
{
	struct reiserfs_inode_info *ei;
	ei = (struct reiserfs_inode_info *)
	    kmem_cache_alloc(reiserfs_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;
	atomic_set(&ei->openers, 0);
	mutex_init(&ei->tailpack);
	return &ei->vfs_inode;
}

static void reiserfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(reiserfs_inode_cachep, REISERFS_I(inode));
}

static void reiserfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, reiserfs_i_callback);
}

static void init_once(void *foo)
{
	struct reiserfs_inode_info *ei = (struct reiserfs_inode_info *)foo;

	INIT_LIST_HEAD(&ei->i_prealloc_list);
	inode_init_once(&ei->vfs_inode);
}

static int init_inodecache(void)
{
	reiserfs_inode_cachep = kmem_cache_create("reiser_inode_cache",
						  sizeof(struct
							 reiserfs_inode_info),
						  0, (SLAB_RECLAIM_ACCOUNT|
							SLAB_MEM_SPREAD),
						  init_once);
	if (reiserfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(reiserfs_inode_cachep);
}

static void reiserfs_dirty_inode(struct inode *inode, int flags)
{
	struct reiserfs_transaction_handle th;

	int err = 0;
	int lock_depth;

	if (inode->i_sb->s_flags & MS_RDONLY) {
		reiserfs_warning(inode->i_sb, "clm-6006",
				 "writing inode %lu on readonly FS",
				 inode->i_ino);
		return;
	}
	lock_depth = reiserfs_write_lock_once(inode->i_sb);

	err = journal_begin(&th, inode->i_sb, 1);
	if (err)
		goto out;

	reiserfs_update_sd(&th, inode);
	journal_end(&th, inode->i_sb, 1);

out:
	reiserfs_write_unlock_once(inode->i_sb, lock_depth);
}

static int reiserfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct super_block *s = root->d_sb;
	struct reiserfs_journal *journal = SB_JOURNAL(s);
	long opts = REISERFS_SB(s)->s_mount_opt;

	if (opts & (1 << REISERFS_LARGETAIL))
		seq_puts(seq, ",tails=on");
	else if (!(opts & (1 << REISERFS_SMALLTAIL)))
		seq_puts(seq, ",notail");
	

	if (!(opts & (1 << REISERFS_BARRIER_FLUSH)))
		seq_puts(seq, ",barrier=none");
	

	if (opts & (1 << REISERFS_ERROR_CONTINUE))
		seq_puts(seq, ",errors=continue");
	else if (opts & (1 << REISERFS_ERROR_PANIC))
		seq_puts(seq, ",errors=panic");
	

	if (opts & (1 << REISERFS_DATA_LOG))
		seq_puts(seq, ",data=journal");
	else if (opts & (1 << REISERFS_DATA_WRITEBACK))
		seq_puts(seq, ",data=writeback");
	

	if (opts & (1 << REISERFS_ATTRS))
		seq_puts(seq, ",attrs");

	if (opts & (1 << REISERFS_XATTRS_USER))
		seq_puts(seq, ",user_xattr");

	if (opts & (1 << REISERFS_EXPOSE_PRIVROOT))
		seq_puts(seq, ",expose_privroot");

	if (opts & (1 << REISERFS_POSIXACL))
		seq_puts(seq, ",acl");

	if (REISERFS_SB(s)->s_jdev)
		seq_printf(seq, ",jdev=%s", REISERFS_SB(s)->s_jdev);

	if (journal->j_max_commit_age != journal->j_default_max_commit_age)
		seq_printf(seq, ",commit=%d", journal->j_max_commit_age);

#ifdef CONFIG_QUOTA
	if (REISERFS_SB(s)->s_qf_names[USRQUOTA])
		seq_printf(seq, ",usrjquota=%s", REISERFS_SB(s)->s_qf_names[USRQUOTA]);
	else if (opts & (1 << REISERFS_USRQUOTA))
		seq_puts(seq, ",usrquota");
	if (REISERFS_SB(s)->s_qf_names[GRPQUOTA])
		seq_printf(seq, ",grpjquota=%s", REISERFS_SB(s)->s_qf_names[GRPQUOTA]);
	else if (opts & (1 << REISERFS_GRPQUOTA))
		seq_puts(seq, ",grpquota");
	if (REISERFS_SB(s)->s_jquota_fmt) {
		if (REISERFS_SB(s)->s_jquota_fmt == QFMT_VFS_OLD)
			seq_puts(seq, ",jqfmt=vfsold");
		else if (REISERFS_SB(s)->s_jquota_fmt == QFMT_VFS_V0)
			seq_puts(seq, ",jqfmt=vfsv0");
	}
#endif

	
	if (opts & (1 << REISERFS_NO_BORDER))
		seq_puts(seq, ",block-allocator=noborder");
	if (opts & (1 << REISERFS_NO_UNHASHED_RELOCATION))
		seq_puts(seq, ",block-allocator=no_unhashed_relocation");
	if (opts & (1 << REISERFS_HASHED_RELOCATION))
		seq_puts(seq, ",block-allocator=hashed_relocation");
	if (opts & (1 << REISERFS_TEST4))
		seq_puts(seq, ",block-allocator=test4");
	show_alloc_options(seq, s);
	return 0;
}

#ifdef CONFIG_QUOTA
static ssize_t reiserfs_quota_write(struct super_block *, int, const char *,
				    size_t, loff_t);
static ssize_t reiserfs_quota_read(struct super_block *, int, char *, size_t,
				   loff_t);
#endif

static const struct super_operations reiserfs_sops = {
	.alloc_inode = reiserfs_alloc_inode,
	.destroy_inode = reiserfs_destroy_inode,
	.write_inode = reiserfs_write_inode,
	.dirty_inode = reiserfs_dirty_inode,
	.evict_inode = reiserfs_evict_inode,
	.put_super = reiserfs_put_super,
	.write_super = reiserfs_write_super,
	.sync_fs = reiserfs_sync_fs,
	.freeze_fs = reiserfs_freeze,
	.unfreeze_fs = reiserfs_unfreeze,
	.statfs = reiserfs_statfs,
	.remount_fs = reiserfs_remount,
	.show_options = reiserfs_show_options,
#ifdef CONFIG_QUOTA
	.quota_read = reiserfs_quota_read,
	.quota_write = reiserfs_quota_write,
#endif
};

#ifdef CONFIG_QUOTA
#define QTYPE2NAME(t) ((t)==USRQUOTA?"user":"group")

static int reiserfs_write_dquot(struct dquot *);
static int reiserfs_acquire_dquot(struct dquot *);
static int reiserfs_release_dquot(struct dquot *);
static int reiserfs_mark_dquot_dirty(struct dquot *);
static int reiserfs_write_info(struct super_block *, int);
static int reiserfs_quota_on(struct super_block *, int, int, struct path *);

static const struct dquot_operations reiserfs_quota_operations = {
	.write_dquot = reiserfs_write_dquot,
	.acquire_dquot = reiserfs_acquire_dquot,
	.release_dquot = reiserfs_release_dquot,
	.mark_dirty = reiserfs_mark_dquot_dirty,
	.write_info = reiserfs_write_info,
	.alloc_dquot	= dquot_alloc,
	.destroy_dquot	= dquot_destroy,
};

static const struct quotactl_ops reiserfs_qctl_operations = {
	.quota_on = reiserfs_quota_on,
	.quota_off = dquot_quota_off,
	.quota_sync = dquot_quota_sync,
	.get_info = dquot_get_dqinfo,
	.set_info = dquot_set_dqinfo,
	.get_dqblk = dquot_get_dqblk,
	.set_dqblk = dquot_set_dqblk,
};
#endif

static const struct export_operations reiserfs_export_ops = {
	.encode_fh = reiserfs_encode_fh,
	.fh_to_dentry = reiserfs_fh_to_dentry,
	.fh_to_parent = reiserfs_fh_to_parent,
	.get_parent = reiserfs_get_parent,
};

typedef struct {
	char *value;
	int setmask;		
	int clrmask;		
} arg_desc_t;

#define REISERFS_OPT_ALLOWEMPTY 31

typedef struct {
	char *option_name;
	int arg_required;	
	const arg_desc_t *values;	
	int setmask;		
	int clrmask;		
} opt_desc_t;

static const arg_desc_t logging_mode[] = {
	{"ordered", 1 << REISERFS_DATA_ORDERED,
	 (1 << REISERFS_DATA_LOG | 1 << REISERFS_DATA_WRITEBACK)},
	{"journal", 1 << REISERFS_DATA_LOG,
	 (1 << REISERFS_DATA_ORDERED | 1 << REISERFS_DATA_WRITEBACK)},
	{"writeback", 1 << REISERFS_DATA_WRITEBACK,
	 (1 << REISERFS_DATA_ORDERED | 1 << REISERFS_DATA_LOG)},
	{.value = NULL}
};

static const arg_desc_t barrier_mode[] = {
	{"none", 1 << REISERFS_BARRIER_NONE, 1 << REISERFS_BARRIER_FLUSH},
	{"flush", 1 << REISERFS_BARRIER_FLUSH, 1 << REISERFS_BARRIER_NONE},
	{.value = NULL}
};

static const arg_desc_t balloc[] = {
	{"noborder", 1 << REISERFS_NO_BORDER, 0},
	{"border", 0, 1 << REISERFS_NO_BORDER},
	{"no_unhashed_relocation", 1 << REISERFS_NO_UNHASHED_RELOCATION, 0},
	{"hashed_relocation", 1 << REISERFS_HASHED_RELOCATION, 0},
	{"test4", 1 << REISERFS_TEST4, 0},
	{"notest4", 0, 1 << REISERFS_TEST4},
	{NULL, 0, 0}
};

static const arg_desc_t tails[] = {
	{"on", 1 << REISERFS_LARGETAIL, 1 << REISERFS_SMALLTAIL},
	{"off", 0, (1 << REISERFS_LARGETAIL) | (1 << REISERFS_SMALLTAIL)},
	{"small", 1 << REISERFS_SMALLTAIL, 1 << REISERFS_LARGETAIL},
	{NULL, 0, 0}
};

static const arg_desc_t error_actions[] = {
	{"panic", 1 << REISERFS_ERROR_PANIC,
	 (1 << REISERFS_ERROR_RO | 1 << REISERFS_ERROR_CONTINUE)},
	{"ro-remount", 1 << REISERFS_ERROR_RO,
	 (1 << REISERFS_ERROR_PANIC | 1 << REISERFS_ERROR_CONTINUE)},
#ifdef REISERFS_JOURNAL_ERROR_ALLOWS_NO_LOG
	{"continue", 1 << REISERFS_ERROR_CONTINUE,
	 (1 << REISERFS_ERROR_PANIC | 1 << REISERFS_ERROR_RO)},
#endif
	{NULL, 0, 0},
};

static int reiserfs_getopt(struct super_block *s, char **cur, opt_desc_t * opts,
			   char **opt_arg, unsigned long *bit_flags)
{
	char *p;
	const opt_desc_t *opt;
	const arg_desc_t *arg;

	p = *cur;

	
	*cur = strchr(p, ',');
	if (*cur) {
		*(*cur) = '\0';
		(*cur)++;
	}

	if (!strncmp(p, "alloc=", 6)) {
		if (reiserfs_parse_alloc_options(s, p + 6)) {
			return -1;
		} else {
			return 0;
		}
	}

	
	for (opt = opts; opt->option_name; opt++) {
		if (!strncmp(p, opt->option_name, strlen(opt->option_name))) {
			if (bit_flags) {
				if (opt->clrmask ==
				    (1 << REISERFS_UNSUPPORTED_OPT))
					reiserfs_warning(s, "super-6500",
							 "%s not supported.\n",
							 p);
				else
					*bit_flags &= ~opt->clrmask;
				if (opt->setmask ==
				    (1 << REISERFS_UNSUPPORTED_OPT))
					reiserfs_warning(s, "super-6501",
							 "%s not supported.\n",
							 p);
				else
					*bit_flags |= opt->setmask;
			}
			break;
		}
	}
	if (!opt->option_name) {
		reiserfs_warning(s, "super-6502",
				 "unknown mount option \"%s\"", p);
		return -1;
	}

	p += strlen(opt->option_name);
	switch (*p) {
	case '=':
		if (!opt->arg_required) {
			reiserfs_warning(s, "super-6503",
					 "the option \"%s\" does not "
					 "require an argument\n",
					 opt->option_name);
			return -1;
		}
		break;

	case 0:
		if (opt->arg_required) {
			reiserfs_warning(s, "super-6504",
					 "the option \"%s\" requires an "
					 "argument\n", opt->option_name);
			return -1;
		}
		break;
	default:
		reiserfs_warning(s, "super-6505",
				 "head of option \"%s\" is only correct\n",
				 opt->option_name);
		return -1;
	}

	
	p++;

	if (opt->arg_required
	    && !(opt->arg_required & (1 << REISERFS_OPT_ALLOWEMPTY))
	    && !strlen(p)) {
		
		reiserfs_warning(s, "super-6506",
				 "empty argument for \"%s\"\n",
				 opt->option_name);
		return -1;
	}

	if (!opt->values) {
		
		*opt_arg = p;
		return opt->arg_required & ~(1 << REISERFS_OPT_ALLOWEMPTY);
	}

	
	for (arg = opt->values; arg->value; arg++) {
		if (!strcmp(p, arg->value)) {
			if (bit_flags) {
				*bit_flags &= ~arg->clrmask;
				*bit_flags |= arg->setmask;
			}
			return opt->arg_required;
		}
	}

	reiserfs_warning(s, "super-6506",
			 "bad value \"%s\" for option \"%s\"\n", p,
			 opt->option_name);
	return -1;
}

static int reiserfs_parse_options(struct super_block *s, char *options,	
				  unsigned long *mount_options,
				  unsigned long *blocks,	
				  char **jdev_name,
				  unsigned int *commit_max_age,
				  char **qf_names,
				  unsigned int *qfmt)
{
	int c;
	char *arg = NULL;
	char *pos;
	opt_desc_t opts[] = {
		
		{"tails",.arg_required = 't',.values = tails},
		{"notail",.clrmask =
		 (1 << REISERFS_LARGETAIL) | (1 << REISERFS_SMALLTAIL)},
		{"conv",.setmask = 1 << REISERFS_CONVERT},
		{"attrs",.setmask = 1 << REISERFS_ATTRS},
		{"noattrs",.clrmask = 1 << REISERFS_ATTRS},
		{"expose_privroot", .setmask = 1 << REISERFS_EXPOSE_PRIVROOT},
#ifdef CONFIG_REISERFS_FS_XATTR
		{"user_xattr",.setmask = 1 << REISERFS_XATTRS_USER},
		{"nouser_xattr",.clrmask = 1 << REISERFS_XATTRS_USER},
#else
		{"user_xattr",.setmask = 1 << REISERFS_UNSUPPORTED_OPT},
		{"nouser_xattr",.clrmask = 1 << REISERFS_UNSUPPORTED_OPT},
#endif
#ifdef CONFIG_REISERFS_FS_POSIX_ACL
		{"acl",.setmask = 1 << REISERFS_POSIXACL},
		{"noacl",.clrmask = 1 << REISERFS_POSIXACL},
#else
		{"acl",.setmask = 1 << REISERFS_UNSUPPORTED_OPT},
		{"noacl",.clrmask = 1 << REISERFS_UNSUPPORTED_OPT},
#endif
		{.option_name = "nolog"},
		{"replayonly",.setmask = 1 << REPLAYONLY},
		{"block-allocator",.arg_required = 'a',.values = balloc},
		{"data",.arg_required = 'd',.values = logging_mode},
		{"barrier",.arg_required = 'b',.values = barrier_mode},
		{"resize",.arg_required = 'r',.values = NULL},
		{"jdev",.arg_required = 'j',.values = NULL},
		{"nolargeio",.arg_required = 'w',.values = NULL},
		{"commit",.arg_required = 'c',.values = NULL},
		{"usrquota",.setmask = 1 << REISERFS_USRQUOTA},
		{"grpquota",.setmask = 1 << REISERFS_GRPQUOTA},
		{"noquota",.clrmask = 1 << REISERFS_USRQUOTA | 1 << REISERFS_GRPQUOTA},
		{"errors",.arg_required = 'e',.values = error_actions},
		{"usrjquota",.arg_required =
		 'u' | (1 << REISERFS_OPT_ALLOWEMPTY),.values = NULL},
		{"grpjquota",.arg_required =
		 'g' | (1 << REISERFS_OPT_ALLOWEMPTY),.values = NULL},
		{"jqfmt",.arg_required = 'f',.values = NULL},
		{.option_name = NULL}
	};

	*blocks = 0;
	if (!options || !*options)
		return 1;

	for (pos = options; pos;) {
		c = reiserfs_getopt(s, &pos, opts, &arg, mount_options);
		if (c == -1)
			
			return 0;

		if (c == 'r') {
			char *p;

			p = NULL;
			

			if (!strcmp(arg, "auto")) {
				
				*blocks =
				    s->s_bdev->bd_inode->i_size >> s->
				    s_blocksize_bits;
			} else {
				*blocks = simple_strtoul(arg, &p, 0);
				if (*p != '\0') {
					
					reiserfs_warning(s, "super-6507",
							 "bad value %s for "
							 "-oresize\n", arg);
					return 0;
				}
			}
		}

		if (c == 'c') {
			char *p = NULL;
			unsigned long val = simple_strtoul(arg, &p, 0);
			
			if (*p != '\0' || val >= (unsigned int)-1) {
				reiserfs_warning(s, "super-6508",
						 "bad value %s for -ocommit\n",
						 arg);
				return 0;
			}
			*commit_max_age = (unsigned int)val;
		}

		if (c == 'w') {
			reiserfs_warning(s, "super-6509", "nolargeio option "
					 "is no longer supported");
			return 0;
		}

		if (c == 'j') {
			if (arg && *arg && jdev_name) {
				if (*jdev_name) {	
					reiserfs_warning(s, "super-6510",
							 "journal device was "
							 "already specified to "
							 "be %s", *jdev_name);
					return 0;
				}
				*jdev_name = arg;
			}
		}
#ifdef CONFIG_QUOTA
		if (c == 'u' || c == 'g') {
			int qtype = c == 'u' ? USRQUOTA : GRPQUOTA;

			if (sb_any_quota_loaded(s) &&
			    (!*arg != !REISERFS_SB(s)->s_qf_names[qtype])) {
				reiserfs_warning(s, "super-6511",
						 "cannot change journaled "
						 "quota options when quota "
						 "turned on.");
				return 0;
			}
			if (*arg) {	
				if (REISERFS_SB(s)->s_qf_names[qtype]
				    && strcmp(REISERFS_SB(s)->s_qf_names[qtype],
					      arg)) {
					reiserfs_warning(s, "super-6512",
							 "%s quota file "
							 "already specified.",
							 QTYPE2NAME(qtype));
					return 0;
				}
				if (strchr(arg, '/')) {
					reiserfs_warning(s, "super-6513",
							 "quotafile must be "
							 "on filesystem root.");
					return 0;
				}
				qf_names[qtype] =
				    kmalloc(strlen(arg) + 1, GFP_KERNEL);
				if (!qf_names[qtype]) {
					reiserfs_warning(s, "reiserfs-2502",
							 "not enough memory "
							 "for storing "
							 "quotafile name.");
					return 0;
				}
				strcpy(qf_names[qtype], arg);
				if (qtype == USRQUOTA)
					*mount_options |= 1 << REISERFS_USRQUOTA;
				else
					*mount_options |= 1 << REISERFS_GRPQUOTA;
			} else {
				if (qf_names[qtype] !=
				    REISERFS_SB(s)->s_qf_names[qtype])
					kfree(qf_names[qtype]);
				qf_names[qtype] = NULL;
				if (qtype == USRQUOTA)
					*mount_options &= ~(1 << REISERFS_USRQUOTA);
				else
					*mount_options &= ~(1 << REISERFS_GRPQUOTA);
			}
		}
		if (c == 'f') {
			if (!strcmp(arg, "vfsold"))
				*qfmt = QFMT_VFS_OLD;
			else if (!strcmp(arg, "vfsv0"))
				*qfmt = QFMT_VFS_V0;
			else {
				reiserfs_warning(s, "super-6514",
						 "unknown quota format "
						 "specified.");
				return 0;
			}
			if (sb_any_quota_loaded(s) &&
			    *qfmt != REISERFS_SB(s)->s_jquota_fmt) {
				reiserfs_warning(s, "super-6515",
						 "cannot change journaled "
						 "quota options when quota "
						 "turned on.");
				return 0;
			}
		}
#else
		if (c == 'u' || c == 'g' || c == 'f') {
			reiserfs_warning(s, "reiserfs-2503", "journaled "
					 "quota options not supported.");
			return 0;
		}
#endif
	}

#ifdef CONFIG_QUOTA
	if (!REISERFS_SB(s)->s_jquota_fmt && !*qfmt
	    && (qf_names[USRQUOTA] || qf_names[GRPQUOTA])) {
		reiserfs_warning(s, "super-6515",
				 "journaled quota format not specified.");
		return 0;
	}
	if ((!(*mount_options & (1 << REISERFS_USRQUOTA)) &&
	       sb_has_quota_loaded(s, USRQUOTA)) ||
	    (!(*mount_options & (1 << REISERFS_GRPQUOTA)) &&
	       sb_has_quota_loaded(s, GRPQUOTA))) {
		reiserfs_warning(s, "super-6516", "quota options must "
				 "be present when quota is turned on.");
		return 0;
	}
#endif

	return 1;
}

static void switch_data_mode(struct super_block *s, unsigned long mode)
{
	REISERFS_SB(s)->s_mount_opt &= ~((1 << REISERFS_DATA_LOG) |
					 (1 << REISERFS_DATA_ORDERED) |
					 (1 << REISERFS_DATA_WRITEBACK));
	REISERFS_SB(s)->s_mount_opt |= (1 << mode);
}

static void handle_data_mode(struct super_block *s, unsigned long mount_options)
{
	if (mount_options & (1 << REISERFS_DATA_LOG)) {
		if (!reiserfs_data_log(s)) {
			switch_data_mode(s, REISERFS_DATA_LOG);
			reiserfs_info(s, "switching to journaled data mode\n");
		}
	} else if (mount_options & (1 << REISERFS_DATA_ORDERED)) {
		if (!reiserfs_data_ordered(s)) {
			switch_data_mode(s, REISERFS_DATA_ORDERED);
			reiserfs_info(s, "switching to ordered data mode\n");
		}
	} else if (mount_options & (1 << REISERFS_DATA_WRITEBACK)) {
		if (!reiserfs_data_writeback(s)) {
			switch_data_mode(s, REISERFS_DATA_WRITEBACK);
			reiserfs_info(s, "switching to writeback data mode\n");
		}
	}
}

static void handle_barrier_mode(struct super_block *s, unsigned long bits)
{
	int flush = (1 << REISERFS_BARRIER_FLUSH);
	int none = (1 << REISERFS_BARRIER_NONE);
	int all_barrier = flush | none;

	if (bits & all_barrier) {
		REISERFS_SB(s)->s_mount_opt &= ~all_barrier;
		if (bits & flush) {
			REISERFS_SB(s)->s_mount_opt |= flush;
			printk("reiserfs: enabling write barrier flush mode\n");
		} else if (bits & none) {
			REISERFS_SB(s)->s_mount_opt |= none;
			printk("reiserfs: write barriers turned off\n");
		}
	}
}

static void handle_attrs(struct super_block *s)
{
	struct reiserfs_super_block *rs = SB_DISK_SUPER_BLOCK(s);

	if (reiserfs_attrs(s)) {
		if (old_format_only(s)) {
			reiserfs_warning(s, "super-6517", "cannot support "
					 "attributes on 3.5.x disk format");
			REISERFS_SB(s)->s_mount_opt &= ~(1 << REISERFS_ATTRS);
			return;
		}
		if (!(le32_to_cpu(rs->s_flags) & reiserfs_attrs_cleared)) {
			reiserfs_warning(s, "super-6518", "cannot support "
					 "attributes until flag is set in "
					 "super-block");
			REISERFS_SB(s)->s_mount_opt &= ~(1 << REISERFS_ATTRS);
		}
	}
}

#ifdef CONFIG_QUOTA
static void handle_quota_files(struct super_block *s, char **qf_names,
			       unsigned int *qfmt)
{
	int i;

	for (i = 0; i < MAXQUOTAS; i++) {
		if (qf_names[i] != REISERFS_SB(s)->s_qf_names[i])
			kfree(REISERFS_SB(s)->s_qf_names[i]);
		REISERFS_SB(s)->s_qf_names[i] = qf_names[i];
	}
	if (*qfmt)
		REISERFS_SB(s)->s_jquota_fmt = *qfmt;
}
#endif

static int reiserfs_remount(struct super_block *s, int *mount_flags, char *arg)
{
	struct reiserfs_super_block *rs;
	struct reiserfs_transaction_handle th;
	unsigned long blocks;
	unsigned long mount_options = REISERFS_SB(s)->s_mount_opt;
	unsigned long safe_mask = 0;
	unsigned int commit_max_age = (unsigned int)-1;
	struct reiserfs_journal *journal = SB_JOURNAL(s);
	char *new_opts = kstrdup(arg, GFP_KERNEL);
	int err;
	char *qf_names[MAXQUOTAS];
	unsigned int qfmt = 0;
#ifdef CONFIG_QUOTA
	int i;
#endif

	reiserfs_write_lock(s);

#ifdef CONFIG_QUOTA
	memcpy(qf_names, REISERFS_SB(s)->s_qf_names, sizeof(qf_names));
#endif

	rs = SB_DISK_SUPER_BLOCK(s);

	if (!reiserfs_parse_options
	    (s, arg, &mount_options, &blocks, NULL, &commit_max_age,
	    qf_names, &qfmt)) {
#ifdef CONFIG_QUOTA
		for (i = 0; i < MAXQUOTAS; i++)
			if (qf_names[i] != REISERFS_SB(s)->s_qf_names[i])
				kfree(qf_names[i]);
#endif
		err = -EINVAL;
		goto out_err;
	}
#ifdef CONFIG_QUOTA
	handle_quota_files(s, qf_names, &qfmt);
#endif

	handle_attrs(s);

	
	safe_mask |= 1 << REISERFS_SMALLTAIL;
	safe_mask |= 1 << REISERFS_LARGETAIL;
	safe_mask |= 1 << REISERFS_NO_BORDER;
	safe_mask |= 1 << REISERFS_NO_UNHASHED_RELOCATION;
	safe_mask |= 1 << REISERFS_HASHED_RELOCATION;
	safe_mask |= 1 << REISERFS_TEST4;
	safe_mask |= 1 << REISERFS_ATTRS;
	safe_mask |= 1 << REISERFS_XATTRS_USER;
	safe_mask |= 1 << REISERFS_POSIXACL;
	safe_mask |= 1 << REISERFS_BARRIER_FLUSH;
	safe_mask |= 1 << REISERFS_BARRIER_NONE;
	safe_mask |= 1 << REISERFS_ERROR_RO;
	safe_mask |= 1 << REISERFS_ERROR_CONTINUE;
	safe_mask |= 1 << REISERFS_ERROR_PANIC;
	safe_mask |= 1 << REISERFS_USRQUOTA;
	safe_mask |= 1 << REISERFS_GRPQUOTA;

	REISERFS_SB(s)->s_mount_opt =
	    (REISERFS_SB(s)->
	     s_mount_opt & ~safe_mask) | (mount_options & safe_mask);

	if (commit_max_age != 0 && commit_max_age != (unsigned int)-1) {
		journal->j_max_commit_age = commit_max_age;
		journal->j_max_trans_age = commit_max_age;
	} else if (commit_max_age == 0) {
		
		journal->j_max_commit_age = journal->j_default_max_commit_age;
		journal->j_max_trans_age = JOURNAL_MAX_TRANS_AGE;
	}

	if (blocks) {
		err = reiserfs_resize(s, blocks);
		if (err != 0)
			goto out_err;
	}

	if (*mount_flags & MS_RDONLY) {
		reiserfs_xattr_init(s, *mount_flags);
		
		if (s->s_flags & MS_RDONLY)
			
			goto out_ok;

		err = dquot_suspend(s, -1);
		if (err < 0)
			goto out_err;

		
		if (sb_umount_state(rs) == REISERFS_VALID_FS
		    || REISERFS_SB(s)->s_mount_state != REISERFS_VALID_FS) {
			goto out_ok;
		}

		err = journal_begin(&th, s, 10);
		if (err)
			goto out_err;

		
		reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s), 1);
		set_sb_umount_state(rs, REISERFS_SB(s)->s_mount_state);
		journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
	} else {
		
		if (!(s->s_flags & MS_RDONLY)) {
			reiserfs_xattr_init(s, *mount_flags);
			goto out_ok;	
		}

		if (reiserfs_is_journal_aborted(journal)) {
			err = journal->j_errno;
			goto out_err;
		}

		handle_data_mode(s, mount_options);
		handle_barrier_mode(s, mount_options);
		REISERFS_SB(s)->s_mount_state = sb_umount_state(rs);
		s->s_flags &= ~MS_RDONLY;	
		err = journal_begin(&th, s, 10);
		if (err)
			goto out_err;

		
		reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s), 1);
		REISERFS_SB(s)->s_mount_state = sb_umount_state(rs);
		s->s_flags &= ~MS_RDONLY;
		set_sb_umount_state(rs, REISERFS_ERROR_FS);
		if (!old_format_only(s))
			set_sb_mnt_count(rs, sb_mnt_count(rs) + 1);
		
		journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
		REISERFS_SB(s)->s_mount_state = REISERFS_VALID_FS;
	}
	
	SB_JOURNAL(s)->j_must_wait = 1;
	err = journal_end(&th, s, 10);
	if (err)
		goto out_err;
	s->s_dirt = 0;

	if (!(*mount_flags & MS_RDONLY)) {
		dquot_resume(s, -1);
		finish_unfinished(s);
		reiserfs_xattr_init(s, *mount_flags);
	}

out_ok:
	replace_mount_options(s, new_opts);
	reiserfs_write_unlock(s);
	return 0;

out_err:
	kfree(new_opts);
	reiserfs_write_unlock(s);
	return err;
}

static int read_super_block(struct super_block *s, int offset)
{
	struct buffer_head *bh;
	struct reiserfs_super_block *rs;
	int fs_blocksize;

	bh = sb_bread(s, offset / s->s_blocksize);
	if (!bh) {
		reiserfs_warning(s, "sh-2006",
				 "bread failed (dev %s, block %lu, size %lu)",
				 reiserfs_bdevname(s), offset / s->s_blocksize,
				 s->s_blocksize);
		return 1;
	}

	rs = (struct reiserfs_super_block *)bh->b_data;
	if (!is_any_reiserfs_magic_string(rs)) {
		brelse(bh);
		return 1;
	}
	
	
	
	fs_blocksize = sb_blocksize(rs);
	brelse(bh);
	sb_set_blocksize(s, fs_blocksize);

	bh = sb_bread(s, offset / s->s_blocksize);
	if (!bh) {
		reiserfs_warning(s, "sh-2007",
				 "bread failed (dev %s, block %lu, size %lu)",
				 reiserfs_bdevname(s), offset / s->s_blocksize,
				 s->s_blocksize);
		return 1;
	}

	rs = (struct reiserfs_super_block *)bh->b_data;
	if (sb_blocksize(rs) != s->s_blocksize) {
		reiserfs_warning(s, "sh-2011", "can't find a reiserfs "
				 "filesystem on (dev %s, block %Lu, size %lu)",
				 reiserfs_bdevname(s),
				 (unsigned long long)bh->b_blocknr,
				 s->s_blocksize);
		brelse(bh);
		return 1;
	}

	if (rs->s_v1.s_root_block == cpu_to_le32(-1)) {
		brelse(bh);
		reiserfs_warning(s, "super-6519", "Unfinished reiserfsck "
				 "--rebuild-tree run detected. Please run\n"
				 "reiserfsck --rebuild-tree and wait for a "
				 "completion. If that fails\n"
				 "get newer reiserfsprogs package");
		return 1;
	}

	SB_BUFFER_WITH_SB(s) = bh;
	SB_DISK_SUPER_BLOCK(s) = rs;

	if (is_reiserfs_jr(rs)) {
		if (sb_version(rs) == REISERFS_VERSION_2)
			reiserfs_info(s, "found reiserfs format \"3.6\""
				      " with non-standard journal\n");
		else if (sb_version(rs) == REISERFS_VERSION_1)
			reiserfs_info(s, "found reiserfs format \"3.5\""
				      " with non-standard journal\n");
		else {
			reiserfs_warning(s, "sh-2012", "found unknown "
					 "format \"%u\" of reiserfs with "
					 "non-standard magic", sb_version(rs));
			return 1;
		}
	} else
		reiserfs_info(s,
			      "found reiserfs format \"%s\" with standard journal\n",
			      is_reiserfs_3_5(rs) ? "3.5" : "3.6");

	s->s_op = &reiserfs_sops;
	s->s_export_op = &reiserfs_export_ops;
#ifdef CONFIG_QUOTA
	s->s_qcop = &reiserfs_qctl_operations;
	s->dq_op = &reiserfs_quota_operations;
#endif

	s->s_maxbytes = (512LL << 32) - s->s_blocksize;
	return 0;
}

static int reread_meta_blocks(struct super_block *s)
{
	ll_rw_block(READ, 1, &(SB_BUFFER_WITH_SB(s)));
	wait_on_buffer(SB_BUFFER_WITH_SB(s));
	if (!buffer_uptodate(SB_BUFFER_WITH_SB(s))) {
		reiserfs_warning(s, "reiserfs-2504", "error reading the super");
		return 1;
	}

	return 0;
}


static __u32 find_hash_out(struct super_block *s)
{
	int retval;
	struct inode *inode;
	struct cpu_key key;
	INITIALIZE_PATH(path);
	struct reiserfs_dir_entry de;
	__u32 hash = DEFAULT_HASH;

	inode = s->s_root->d_inode;

	do {			
		u32 teahash, r5hash, yurahash;

		make_cpu_key(&key, inode, ~0, TYPE_DIRENTRY, 3);
		retval = search_by_entry_key(s, &key, &path, &de);
		if (retval == IO_ERROR) {
			pathrelse(&path);
			return UNSET_HASH;
		}
		if (retval == NAME_NOT_FOUND)
			de.de_entry_num--;
		set_de_name_and_namelen(&de);
		if (deh_offset(&(de.de_deh[de.de_entry_num])) == DOT_DOT_OFFSET) {
			
			if (reiserfs_rupasov_hash(s)) {
				hash = YURA_HASH;
			}
			reiserfs_info(s, "FS seems to be empty, autodetect "
					 "is using the default hash\n");
			break;
		}
		r5hash = GET_HASH_VALUE(r5_hash(de.de_name, de.de_namelen));
		teahash = GET_HASH_VALUE(keyed_hash(de.de_name, de.de_namelen));
		yurahash = GET_HASH_VALUE(yura_hash(de.de_name, de.de_namelen));
		if (((teahash == r5hash)
		     &&
		     (GET_HASH_VALUE(deh_offset(&(de.de_deh[de.de_entry_num])))
		      == r5hash)) || ((teahash == yurahash)
				      && (yurahash ==
					  GET_HASH_VALUE(deh_offset
							 (&
							  (de.
							   de_deh[de.
								  de_entry_num])))))
		    || ((r5hash == yurahash)
			&& (yurahash ==
			    GET_HASH_VALUE(deh_offset
					   (&(de.de_deh[de.de_entry_num])))))) {
			reiserfs_warning(s, "reiserfs-2506", "Unable to "
					 "automatically detect hash function. "
					 "Please mount with -o "
					 "hash={tea,rupasov,r5}");
			hash = UNSET_HASH;
			break;
		}
		if (GET_HASH_VALUE(deh_offset(&(de.de_deh[de.de_entry_num]))) ==
		    yurahash)
			hash = YURA_HASH;
		else if (GET_HASH_VALUE
			 (deh_offset(&(de.de_deh[de.de_entry_num]))) == teahash)
			hash = TEA_HASH;
		else if (GET_HASH_VALUE
			 (deh_offset(&(de.de_deh[de.de_entry_num]))) == r5hash)
			hash = R5_HASH;
		else {
			reiserfs_warning(s, "reiserfs-2506",
					 "Unrecognised hash function");
			hash = UNSET_HASH;
		}
	} while (0);

	pathrelse(&path);
	return hash;
}

static int what_hash(struct super_block *s)
{
	__u32 code;

	code = sb_hash_function_code(SB_DISK_SUPER_BLOCK(s));

	if (code == UNSET_HASH || reiserfs_hash_detect(s))
		code = find_hash_out(s);

	if (code != UNSET_HASH && reiserfs_hash_detect(s)) {
		if (reiserfs_rupasov_hash(s) && code != YURA_HASH) {
			reiserfs_warning(s, "reiserfs-2507",
					 "Error, %s hash detected, "
					 "unable to force rupasov hash",
					 reiserfs_hashname(code));
			code = UNSET_HASH;
		} else if (reiserfs_tea_hash(s) && code != TEA_HASH) {
			reiserfs_warning(s, "reiserfs-2508",
					 "Error, %s hash detected, "
					 "unable to force tea hash",
					 reiserfs_hashname(code));
			code = UNSET_HASH;
		} else if (reiserfs_r5_hash(s) && code != R5_HASH) {
			reiserfs_warning(s, "reiserfs-2509",
					 "Error, %s hash detected, "
					 "unable to force r5 hash",
					 reiserfs_hashname(code));
			code = UNSET_HASH;
		}
	} else {
		
		if (reiserfs_rupasov_hash(s)) {
			code = YURA_HASH;
		} else if (reiserfs_tea_hash(s)) {
			code = TEA_HASH;
		} else if (reiserfs_r5_hash(s)) {
			code = R5_HASH;
		}
	}

	if (code != UNSET_HASH &&
	    !(s->s_flags & MS_RDONLY) &&
	    code != sb_hash_function_code(SB_DISK_SUPER_BLOCK(s))) {
		set_sb_hash_function_code(SB_DISK_SUPER_BLOCK(s), code);
	}
	return code;
}

static hashf_t hash_function(struct super_block *s)
{
	switch (what_hash(s)) {
	case TEA_HASH:
		reiserfs_info(s, "Using tea hash to sort names\n");
		return keyed_hash;
	case YURA_HASH:
		reiserfs_info(s, "Using rupasov hash to sort names\n");
		return yura_hash;
	case R5_HASH:
		reiserfs_info(s, "Using r5 hash to sort names\n");
		return r5_hash;
	}
	return NULL;
}

static int function2code(hashf_t func)
{
	if (func == keyed_hash)
		return TEA_HASH;
	if (func == yura_hash)
		return YURA_HASH;
	if (func == r5_hash)
		return R5_HASH;

	BUG();			

	return 0;
}

#define SWARN(silent, s, id, ...)			\
	if (!(silent))				\
		reiserfs_warning(s, id, __VA_ARGS__)

static int reiserfs_fill_super(struct super_block *s, void *data, int silent)
{
	struct inode *root_inode;
	struct reiserfs_transaction_handle th;
	int old_format = 0;
	unsigned long blocks;
	unsigned int commit_max_age = 0;
	int jinit_done = 0;
	struct reiserfs_iget_args args;
	struct reiserfs_super_block *rs;
	char *jdev_name;
	struct reiserfs_sb_info *sbi;
	int errval = -EINVAL;
	char *qf_names[MAXQUOTAS] = {};
	unsigned int qfmt = 0;

	save_mount_options(s, data);

	sbi = kzalloc(sizeof(struct reiserfs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	s->s_fs_info = sbi;
	
	REISERFS_SB(s)->s_mount_opt |= (1 << REISERFS_SMALLTAIL);
	REISERFS_SB(s)->s_mount_opt |= (1 << REISERFS_ERROR_RO);
	REISERFS_SB(s)->s_mount_opt |= (1 << REISERFS_BARRIER_FLUSH);
	REISERFS_SB(s)->s_alloc_options.preallocmin = 0;
	
	REISERFS_SB(s)->s_alloc_options.preallocsize = 17;
	
	reiserfs_init_alloc_options(s);

	mutex_init(&REISERFS_SB(s)->lock);
	REISERFS_SB(s)->lock_depth = -1;

	jdev_name = NULL;
	if (reiserfs_parse_options
	    (s, (char *)data, &(sbi->s_mount_opt), &blocks, &jdev_name,
	     &commit_max_age, qf_names, &qfmt) == 0) {
		goto error_unlocked;
	}
	if (jdev_name && jdev_name[0]) {
		REISERFS_SB(s)->s_jdev = kstrdup(jdev_name, GFP_KERNEL);
		if (!REISERFS_SB(s)->s_jdev) {
			SWARN(silent, s, "", "Cannot allocate memory for "
				"journal device name");
			goto error;
		}
	}
#ifdef CONFIG_QUOTA
	handle_quota_files(s, qf_names, &qfmt);
#endif

	if (blocks) {
		SWARN(silent, s, "jmacd-7", "resize option for remount only");
		goto error_unlocked;
	}

	
	if (!read_super_block(s, REISERFS_OLD_DISK_OFFSET_IN_BYTES))
		old_format = 1;
	
	else if (read_super_block(s, REISERFS_DISK_OFFSET_IN_BYTES)) {
		SWARN(silent, s, "sh-2021", "can not find reiserfs on %s",
		      reiserfs_bdevname(s));
		goto error_unlocked;
	}

	rs = SB_DISK_SUPER_BLOCK(s);
	if (s->s_bdev && s->s_bdev->bd_inode
	    && i_size_read(s->s_bdev->bd_inode) <
	    sb_block_count(rs) * sb_blocksize(rs)) {
		SWARN(silent, s, "", "Filesystem cannot be "
		      "mounted because it is bigger than the device");
		SWARN(silent, s, "", "You may need to run fsck "
		      "or increase size of your LVM partition");
		SWARN(silent, s, "", "Or may be you forgot to "
		      "reboot after fdisk when it told you to");
		goto error_unlocked;
	}

	sbi->s_mount_state = SB_REISERFS_STATE(s);
	sbi->s_mount_state = REISERFS_VALID_FS;

	if ((errval = reiserfs_init_bitmap_cache(s))) {
		SWARN(silent, s, "jmacd-8", "unable to read bitmap");
		goto error_unlocked;
	}

	errval = -EINVAL;
#ifdef CONFIG_REISERFS_CHECK
	SWARN(silent, s, "", "CONFIG_REISERFS_CHECK is set ON");
	SWARN(silent, s, "", "- it is slow mode for debugging.");
#endif

	
	if (!reiserfs_data_log(s) && !reiserfs_data_ordered(s) &&
	    !reiserfs_data_writeback(s)) {
		REISERFS_SB(s)->s_mount_opt |= (1 << REISERFS_DATA_ORDERED);
	}

	if (reiserfs_data_log(s)) {
		reiserfs_info(s, "using journaled data mode\n");
	} else if (reiserfs_data_ordered(s)) {
		reiserfs_info(s, "using ordered data mode\n");
	} else {
		reiserfs_info(s, "using writeback data mode\n");
	}
	if (reiserfs_barrier_flush(s)) {
		printk("reiserfs: using flush barriers\n");
	}

	
	if (journal_init(s, jdev_name, old_format, commit_max_age)) {
		SWARN(silent, s, "sh-2022",
		      "unable to initialize journal space");
		goto error_unlocked;
	} else {
		jinit_done = 1;	
	}

	if (reread_meta_blocks(s)) {
		SWARN(silent, s, "jmacd-9",
		      "unable to reread meta blocks after journal init");
		goto error_unlocked;
	}

	if (replay_only(s))
		goto error_unlocked;

	if (bdev_read_only(s->s_bdev) && !(s->s_flags & MS_RDONLY)) {
		SWARN(silent, s, "clm-7000",
		      "Detected readonly device, marking FS readonly");
		s->s_flags |= MS_RDONLY;
	}
	args.objectid = REISERFS_ROOT_OBJECTID;
	args.dirid = REISERFS_ROOT_PARENT_OBJECTID;
	root_inode =
	    iget5_locked(s, REISERFS_ROOT_OBJECTID, reiserfs_find_actor,
			 reiserfs_init_locked_inode, (void *)(&args));
	if (!root_inode) {
		SWARN(silent, s, "jmacd-10", "get root inode failed");
		goto error_unlocked;
	}

	reiserfs_write_lock(s);

	if (root_inode->i_state & I_NEW) {
		reiserfs_read_locked_inode(root_inode, &args);
		unlock_new_inode(root_inode);
	}

	s->s_root = d_make_root(root_inode);
	if (!s->s_root)
		goto error;
	
	sbi->s_hash_function = hash_function(s);
	if (sbi->s_hash_function == NULL) {
		dput(s->s_root);
		s->s_root = NULL;
		goto error;
	}

	if (is_reiserfs_3_5(rs)
	    || (is_reiserfs_jr(rs) && SB_VERSION(s) == REISERFS_VERSION_1))
		set_bit(REISERFS_3_5, &(sbi->s_properties));
	else if (old_format)
		set_bit(REISERFS_OLD_FORMAT, &(sbi->s_properties));
	else
		set_bit(REISERFS_3_6, &(sbi->s_properties));

	if (!(s->s_flags & MS_RDONLY)) {

		errval = journal_begin(&th, s, 1);
		if (errval) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}
		reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s), 1);

		set_sb_umount_state(rs, REISERFS_ERROR_FS);
		set_sb_fs_state(rs, 0);

		if (bmap_would_wrap(reiserfs_bmap_count(s)) &&
		    sb_bmap_nr(rs) != 0) {
			reiserfs_warning(s, "super-2030", "This file system "
					"claims to use %u bitmap blocks in "
					"its super block, but requires %u. "
					"Clearing to zero.", sb_bmap_nr(rs),
					reiserfs_bmap_count(s));

			set_sb_bmap_nr(rs, 0);
		}

		if (old_format_only(s)) {
			if (convert_reiserfs(s)) {
				
				if (!silent)
					reiserfs_info(s,
						      "converting 3.5 filesystem to the 3.6 format");

				if (is_reiserfs_3_5(rs))
					memcpy(rs->s_v1.s_magic,
					       reiserfs_3_6_magic_string,
					       sizeof
					       (reiserfs_3_6_magic_string));

				set_sb_version(rs, REISERFS_VERSION_2);
				reiserfs_convert_objectid_map_v1(s);
				set_bit(REISERFS_3_6, &(sbi->s_properties));
				clear_bit(REISERFS_3_5, &(sbi->s_properties));
			} else if (!silent) {
				reiserfs_info(s, "using 3.5.x disk format\n");
			}
		} else
			set_sb_mnt_count(rs, sb_mnt_count(rs) + 1);


		journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
		errval = journal_end(&th, s, 1);
		if (errval) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}

		if ((errval = reiserfs_lookup_privroot(s)) ||
		    (errval = reiserfs_xattr_init(s, s->s_flags))) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}

		
		finish_unfinished(s);
	} else {
		if (old_format_only(s) && !silent) {
			reiserfs_info(s, "using 3.5.x disk format\n");
		}

		if ((errval = reiserfs_lookup_privroot(s)) ||
		    (errval = reiserfs_xattr_init(s, s->s_flags))) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}
	}
	
	set_sb_hash_function_code(rs, function2code(sbi->s_hash_function));

	handle_attrs(s);

	reiserfs_proc_info_init(s);

	init_waitqueue_head(&(sbi->s_wait));
	spin_lock_init(&sbi->bitmap_lock);

	reiserfs_write_unlock(s);

	return (0);

error:
	reiserfs_write_unlock(s);

error_unlocked:
	
	if (jinit_done) {
		reiserfs_write_lock(s);
		journal_release_error(NULL, s);
		reiserfs_write_unlock(s);
	}

	reiserfs_free_bitmap_cache(s);
	if (SB_BUFFER_WITH_SB(s))
		brelse(SB_BUFFER_WITH_SB(s));
#ifdef CONFIG_QUOTA
	{
		int j;
		for (j = 0; j < MAXQUOTAS; j++)
			kfree(qf_names[j]);
	}
#endif
	kfree(sbi);

	s->s_fs_info = NULL;
	return errval;
}

static int reiserfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct reiserfs_super_block *rs = SB_DISK_SUPER_BLOCK(dentry->d_sb);

	buf->f_namelen = (REISERFS_MAX_NAME(s->s_blocksize));
	buf->f_bfree = sb_free_blocks(rs);
	buf->f_bavail = buf->f_bfree;
	buf->f_blocks = sb_block_count(rs) - sb_bmap_nr(rs) - 1;
	buf->f_bsize = dentry->d_sb->s_blocksize;
	
	buf->f_type = REISERFS_SUPER_MAGIC;
	buf->f_fsid.val[0] = (u32)crc32_le(0, rs->s_uuid, sizeof(rs->s_uuid)/2);
	buf->f_fsid.val[1] = (u32)crc32_le(0, rs->s_uuid + sizeof(rs->s_uuid)/2,
				sizeof(rs->s_uuid)/2);

	return 0;
}

#ifdef CONFIG_QUOTA
static int reiserfs_write_dquot(struct dquot *dquot)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	reiserfs_write_lock(dquot->dq_sb);
	ret =
	    journal_begin(&th, dquot->dq_sb,
			  REISERFS_QUOTA_TRANS_BLOCKS(dquot->dq_sb));
	if (ret)
		goto out;
	ret = dquot_commit(dquot);
	err =
	    journal_end(&th, dquot->dq_sb,
			REISERFS_QUOTA_TRANS_BLOCKS(dquot->dq_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(dquot->dq_sb);
	return ret;
}

static int reiserfs_acquire_dquot(struct dquot *dquot)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	reiserfs_write_lock(dquot->dq_sb);
	ret =
	    journal_begin(&th, dquot->dq_sb,
			  REISERFS_QUOTA_INIT_BLOCKS(dquot->dq_sb));
	if (ret)
		goto out;
	ret = dquot_acquire(dquot);
	err =
	    journal_end(&th, dquot->dq_sb,
			REISERFS_QUOTA_INIT_BLOCKS(dquot->dq_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(dquot->dq_sb);
	return ret;
}

static int reiserfs_release_dquot(struct dquot *dquot)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	reiserfs_write_lock(dquot->dq_sb);
	ret =
	    journal_begin(&th, dquot->dq_sb,
			  REISERFS_QUOTA_DEL_BLOCKS(dquot->dq_sb));
	if (ret) {
		
		dquot_release(dquot);
		goto out;
	}
	ret = dquot_release(dquot);
	err =
	    journal_end(&th, dquot->dq_sb,
			REISERFS_QUOTA_DEL_BLOCKS(dquot->dq_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(dquot->dq_sb);
	return ret;
}

static int reiserfs_mark_dquot_dirty(struct dquot *dquot)
{
	
	if (REISERFS_SB(dquot->dq_sb)->s_qf_names[USRQUOTA] ||
	    REISERFS_SB(dquot->dq_sb)->s_qf_names[GRPQUOTA]) {
		dquot_mark_dquot_dirty(dquot);
		return reiserfs_write_dquot(dquot);
	} else
		return dquot_mark_dquot_dirty(dquot);
}

static int reiserfs_write_info(struct super_block *sb, int type)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	
	reiserfs_write_lock(sb);
	ret = journal_begin(&th, sb, 2);
	if (ret)
		goto out;
	ret = dquot_commit_info(sb, type);
	err = journal_end(&th, sb, 2);
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(sb);
	return ret;
}

static int reiserfs_quota_on_mount(struct super_block *sb, int type)
{
	return dquot_quota_on_mount(sb, REISERFS_SB(sb)->s_qf_names[type],
					REISERFS_SB(sb)->s_jquota_fmt, type);
}

static int reiserfs_quota_on(struct super_block *sb, int type, int format_id,
			     struct path *path)
{
	int err;
	struct inode *inode;
	struct reiserfs_transaction_handle th;
	int opt = type == USRQUOTA ? REISERFS_USRQUOTA : REISERFS_GRPQUOTA;

	if (!(REISERFS_SB(sb)->s_mount_opt & (1 << opt)))
		return -EINVAL;

	
	if (path->dentry->d_sb != sb) {
		err = -EXDEV;
		goto out;
	}
	inode = path->dentry->d_inode;
	
	if (!(REISERFS_I(inode)->i_flags & i_nopack_mask)) {
		err = reiserfs_unpack(inode, NULL);
		if (err) {
			reiserfs_warning(sb, "super-6520",
				"Unpacking tail of quota file failed"
				" (%d). Cannot turn on quotas.", err);
			err = -EINVAL;
			goto out;
		}
		mark_inode_dirty(inode);
	}
	
	if (REISERFS_SB(sb)->s_qf_names[type]) {
		
		if (path->dentry->d_parent != sb->s_root)
			reiserfs_warning(sb, "super-6521",
				 "Quota file not on filesystem root. "
				 "Journalled quota will not work.");
	}

	if (reiserfs_file_data_log(inode)) {
		
		err = journal_begin(&th, sb, 1);
		if (err)
			goto out;
		err = journal_end_sync(&th, sb, 1);
		if (err)
			goto out;
	}
	err = dquot_quota_on(sb, type, format_id, path);
out:
	return err;
}

static ssize_t reiserfs_quota_read(struct super_block *sb, int type, char *data,
				   size_t len, loff_t off)
{
	struct inode *inode = sb_dqopt(sb)->files[type];
	unsigned long blk = off >> sb->s_blocksize_bits;
	int err = 0, offset = off & (sb->s_blocksize - 1), tocopy;
	size_t toread;
	struct buffer_head tmp_bh, *bh;
	loff_t i_size = i_size_read(inode);

	if (off > i_size)
		return 0;
	if (off + len > i_size)
		len = i_size - off;
	toread = len;
	while (toread > 0) {
		tocopy =
		    sb->s_blocksize - offset <
		    toread ? sb->s_blocksize - offset : toread;
		tmp_bh.b_state = 0;
		
		reiserfs_write_lock(sb);
		err = reiserfs_get_block(inode, blk, &tmp_bh, 0);
		reiserfs_write_unlock(sb);
		if (err)
			return err;
		if (!buffer_mapped(&tmp_bh))	
			memset(data, 0, tocopy);
		else {
			bh = sb_bread(sb, tmp_bh.b_blocknr);
			if (!bh)
				return -EIO;
			memcpy(data, bh->b_data + offset, tocopy);
			brelse(bh);
		}
		offset = 0;
		toread -= tocopy;
		data += tocopy;
		blk++;
	}
	return len;
}

static ssize_t reiserfs_quota_write(struct super_block *sb, int type,
				    const char *data, size_t len, loff_t off)
{
	struct inode *inode = sb_dqopt(sb)->files[type];
	unsigned long blk = off >> sb->s_blocksize_bits;
	int err = 0, offset = off & (sb->s_blocksize - 1), tocopy;
	int journal_quota = REISERFS_SB(sb)->s_qf_names[type] != NULL;
	size_t towrite = len;
	struct buffer_head tmp_bh, *bh;

	if (!current->journal_info) {
		printk(KERN_WARNING "reiserfs: Quota write (off=%Lu, len=%Lu)"
			" cancelled because transaction is not started.\n",
			(unsigned long long)off, (unsigned long long)len);
		return -EIO;
	}
	mutex_lock_nested(&inode->i_mutex, I_MUTEX_QUOTA);
	while (towrite > 0) {
		tocopy = sb->s_blocksize - offset < towrite ?
		    sb->s_blocksize - offset : towrite;
		tmp_bh.b_state = 0;
		err = reiserfs_get_block(inode, blk, &tmp_bh, GET_BLOCK_CREATE);
		if (err)
			goto out;
		if (offset || tocopy != sb->s_blocksize)
			bh = sb_bread(sb, tmp_bh.b_blocknr);
		else
			bh = sb_getblk(sb, tmp_bh.b_blocknr);
		if (!bh) {
			err = -EIO;
			goto out;
		}
		lock_buffer(bh);
		memcpy(bh->b_data + offset, data, tocopy);
		flush_dcache_page(bh->b_page);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		reiserfs_prepare_for_journal(sb, bh, 1);
		journal_mark_dirty(current->journal_info, sb, bh);
		if (!journal_quota)
			reiserfs_add_ordered_list(inode, bh);
		brelse(bh);
		offset = 0;
		towrite -= tocopy;
		data += tocopy;
		blk++;
	}
out:
	if (len == towrite) {
		mutex_unlock(&inode->i_mutex);
		return err;
	}
	if (inode->i_size < off + len - towrite)
		i_size_write(inode, off + len - towrite);
	inode->i_version++;
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);
	mutex_unlock(&inode->i_mutex);
	return len - towrite;
}

#endif

static struct dentry *get_super_block(struct file_system_type *fs_type,
			   int flags, const char *dev_name,
			   void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, reiserfs_fill_super);
}

static int __init init_reiserfs_fs(void)
{
	int ret;

	if ((ret = init_inodecache())) {
		return ret;
	}

	reiserfs_proc_info_global_init();

	ret = register_filesystem(&reiserfs_fs_type);

	if (ret == 0) {
		return 0;
	}

	reiserfs_proc_info_global_done();
	destroy_inodecache();

	return ret;
}

static void __exit exit_reiserfs_fs(void)
{
	reiserfs_proc_info_global_done();
	unregister_filesystem(&reiserfs_fs_type);
	destroy_inodecache();
}

struct file_system_type reiserfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "reiserfs",
	.mount = get_super_block,
	.kill_sb = reiserfs_kill_sb,
	.fs_flags = FS_REQUIRES_DEV,
};

MODULE_DESCRIPTION("ReiserFS journaled filesystem");
MODULE_AUTHOR("Hans Reiser <reiser@namesys.com>");
MODULE_LICENSE("GPL");

module_init(init_reiserfs_fs);
module_exit(exit_reiserfs_fs);