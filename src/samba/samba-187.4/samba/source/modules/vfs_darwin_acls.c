/*
 * Darwin ACL VFS module
 *
 * Copyright (C) Jeremy Allison 1994-2000.
 * Copyright (c) 2004 - 2007 Apple Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_ACLS

#include "includes.h"
#include <membership.h>

#define MODULE_NAME "darwinacl"

/* FOREACH_ACE(acl_t acl, acl_entry_t ace)
 * Iterate over the ACL, setting ace to each entry in turn.
 *
 * NOTE that on 10.4 acl_get_entry(3) is wrong - acl_get_entry returns 0
 * on success, -1 on (any) failure.
 */
#define FOREACH_ACE(acl, ace) \
	for ((ace) = NULL; \
	     acl_get_entry((acl), \
		(ace) ? ACL_NEXT_ENTRY : ACL_FIRST_ENTRY, &(ace)) == 0; )

#define MASK_MATCH_ALL(mask1, mask2) ( ( (mask1) & (mask2) ) == (mask2) )
#define MASK_MATCH_ANY(mask1, mask2) ( (mask1) & (mask2) )

#define FILE_SPECIFIC_READ_BITS \
    (FILE_READ_DATA|FILE_READ_EA|FILE_READ_ATTRIBUTES|READ_CONTROL_ACCESS)

#define FILE_SPECIFIC_WRITE_BITS \
    (FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES)

#define FILE_SPECIFIC_EXECUTE_BITS \
    (FILE_EXECUTE)

/* Bitmask of all possible ACL permissions. We use this in an ugly workaround
 * for the lack of an API that tests whether a permset is empty or not.
 */
#define ACL_ALL_PERMISSIONS \
    ( ACL_READ_DATA | \
        ACL_LIST_DIRECTORY | \
        ACL_WRITE_DATA | \
        ACL_ADD_FILE | \
        ACL_EXECUTE | \
        ACL_SEARCH | \
        ACL_DELETE | \
        ACL_APPEND_DATA | \
        ACL_ADD_SUBDIRECTORY | \
        ACL_DELETE_CHILD | \
        ACL_READ_ATTRIBUTES | \
        ACL_WRITE_ATTRIBUTES | \
        ACL_WRITE_EXTATTRIBUTES | \
        ACL_READ_SECURITY | \
        ACL_WRITE_SECURITY | \
        ACL_CHANGE_OWNER)


static SEC_ACL empty_acl = {
	NT4_ACL_REVISION, /* revision */
	SEC_ACL_HEADER_SIZE, /* size */
	0, /* number of ACEs */
	NULL /* ACEs*/
    };

static BOOL acl_support_enabled(connection_struct *conn)
{
	return (conn->fs_capabilities & FILE_PERSISTENT_ACLS) ? True : False;
}

static filesec_t fsp_get_filesec(files_struct *fsp)
{
	int ret;
	struct stat sbuf;
	filesec_t fsec;

	fsec = filesec_init();

	if (fsp->fh->fd != -1) {
		ret = fstatx_np(fsp->fh->fd, &sbuf, fsec);
	} else {
		ret = statx_np(fsp->fsp_name, &sbuf, fsec);
	}

	if (ret == -1) {
		DEBUG(0,("%s: statx_np (%s): errno(%d) - (%s)\n",
			    MODULE_NAME, fsp->fsp_name,
			    errno, strerror(errno)));
		filesec_free(fsec);
		return NULL;
	}

	return fsec;
}

static int fsp_set_acl(const files_struct *fsp, acl_t acl)
{
	int err;

	if (fsp->fh->fd != -1) {
		err = acl_set_fd_np(fsp->fh->fd, acl, ACL_TYPE_EXTENDED);
	} else {
		err = acl_set_file(fsp->fsp_name, ACL_TYPE_EXTENDED, acl);
	}

	if (err != 0) {
		char * aclstr = acl_to_text(acl, NULL);

		DEBUG(0, ("%s: failed to set ACL on %s: %s\n",
			    MODULE_NAME, fsp->fsp_name, strerror(errno)));

		if (aclstr) {
		    DEBUGADD(0, ("%s\n", aclstr));
		    acl_free(aclstr);
		}
	}

	return err;
}

/* Number of entries to grow the list be each time. */
#define ACE_LIST_CHUNK 40

struct sec_ace_list {
	SEC_ACE *   	ace_list;
	unsigned    	ace_length;
	unsigned    	ace_count;
	void *		ace_ctx;
};

static void ace_list_free(struct sec_ace_list * acelist)
{
	void * mem_ctx = acelist->ace_ctx;

	if (acelist->ace_list) {
		talloc_free(acelist->ace_list);
	}

	/* Save the talloc context. This is a little bit of paranoia, but since
	 * NULL is a valid context, simply zeroing the whole struct could be
	 * misleading.
	 */
	ZERO_STRUCTP(acelist);
	acelist->ace_ctx = mem_ctx;
}

static BOOL ace_list_grow(struct sec_ace_list *acelist)
{
	SEC_ACE * newlist;

	newlist = talloc_realloc(acelist->ace_ctx, acelist->ace_list,
			SEC_ACE, acelist->ace_length + ACE_LIST_CHUNK);
	if (newlist == NULL) {
		ace_list_free(acelist);
		return False;
	}

	acelist->ace_list = newlist;
	acelist->ace_length += ACE_LIST_CHUNK;
	return True;
}

static BOOL ace_list_new(void * mem_ctx, struct sec_ace_list * acelist)
{
	ZERO_STRUCTP(acelist);
	acelist->ace_ctx = mem_ctx;

	return ace_list_grow(acelist);
}

static BOOL ace_list_append_ace(struct sec_ace_list * acelist,
		const DOM_SID *sid, uint8 type, SEC_ACCESS mask, uint8 flag)
{
	/* Make room for a new SEC_ACE if necessary. */
	if (acelist->ace_count == acelist->ace_length) {
		if (!ace_list_grow(acelist)) {
			return False;
		}
	}

	init_sec_ace(&acelist->ace_list[acelist->ace_count],
			sid, type, mask, flag);
	acelist->ace_count++;
	return True;
}

/****************************************************************************
 Fixed Darwin <-> NT mapping tables.
****************************************************************************/

/* XXX Should these mapping include the directory-specific rights, like serach
 * and list?
 */
#define ACL_GENERIC_READ \
		(ACL_READ_DATA | ACL_READ_ATTRIBUTES | \
		    ACL_READ_EXTATTRIBUTES | ACL_READ_SECURITY)

#define ACL_GENERIC_WRITE \
		(ACL_WRITE_DATA | ACL_WRITE_ATTRIBUTES | \
		    ACL_WRITE_EXTATTRIBUTES | ACL_WRITE_SECURITY )

#define ACL_GENERIC_EXECUTE \
		(ACL_GENERIC_READ | ACL_EXECUTE)

#define ACL_GENERIC_ALL \
		(ACL_GENERIC_READ | ACL_GENERIC_WRITE | ACL_GENERIC_EXECUTE | \
		    ACL_DELETE )

static const struct
{
	acl_perm_t aclperm;
	int ntperm;
} ntacl_perm_table[] =
{
	{ ACL_READ_DATA, FILE_READ_DATA },
	{ ACL_WRITE_DATA, FILE_WRITE_DATA },
	{ ACL_EXECUTE, FILE_EXECUTE },
	{ ACL_DELETE, DELETE_ACCESS },
	{ ACL_APPEND_DATA, FILE_APPEND_DATA },
	{ ACL_DELETE_CHILD, FILE_DELETE_CHILD },
	{ ACL_READ_ATTRIBUTES, FILE_READ_ATTRIBUTES },
	{ ACL_READ_EXTATTRIBUTES, FILE_READ_EA },
	{ ACL_WRITE_ATTRIBUTES, FILE_WRITE_ATTRIBUTES },
	{ ACL_WRITE_EXTATTRIBUTES, FILE_WRITE_EA },
	{ ACL_READ_SECURITY, READ_CONTROL_ACCESS },
	{ ACL_WRITE_SECURITY, WRITE_DAC_ACCESS },
	{ ACL_CHANGE_OWNER, WRITE_OWNER_ACCESS },

};

static const struct
{
	acl_flag_t aclflag;
	int ntflag;
} ntacl_flag_table[] =
{
	{ ACL_ENTRY_INHERITED, SEC_ACE_FLAG_INHERITED_ACE },
	{ ACL_ENTRY_FILE_INHERIT, SEC_ACE_FLAG_OBJECT_INHERIT },
	{ ACL_ENTRY_DIRECTORY_INHERIT, SEC_ACE_FLAG_CONTAINER_INHERIT },
	{ ACL_ENTRY_LIMIT_INHERIT, SEC_ACE_FLAG_NO_PROPAGATE_INHERIT },
	{ ACL_ENTRY_ONLY_INHERIT, SEC_ACE_FLAG_INHERIT_ONLY }
};

static int map_flags_darwin_to_nt (acl_flagset_t flags)
{
	uint32 darwin_flags = 0;
	int ntflags = 0;
	int i;

	/* SEC_ACE_FLAG_VALID_INHERIT - ??? - AUDIT ACE FLAG */

	if (acl_get_flag_np(flags, ACL_FLAG_DEFER_INHERIT) == 1) {
		DEBUG(0, ("%s: unable to map ACL_FLAG_DEFER_INHERIT\n",
					MODULE_NAME));
	}

	for (i = 0; i < ARRAY_SIZE(ntacl_flag_table); ++i) {
		if (acl_get_flag_np(flags, ntacl_flag_table[i].aclflag) == 1) {
			ntflags |= ntacl_flag_table[i].ntflag;
			darwin_flags |= ntacl_flag_table[i].aclflag;
		}
	}

	DEBUG(4, ("%s: mapped Darwin flags %#x to NT flags %#x\n",
		MODULE_NAME, darwin_flags, ntflags));

	return ntflags;
}

static void map_flags_nt_to_darwin(SEC_ACE *ace, acl_flagset_t flagset)
{
	int i;
	int acl_add_flag_return;
	uint32 darwin_flags = 0;

	for (i = 0; i < ARRAY_SIZE(ntacl_flag_table); ++i) {
		if (!(ace->flags & ntacl_flag_table[i].ntflag)) {
			continue;
		}

		/* This can only fail if we messed up the mapping table. Hence
		 * the assert instead of an error return.
		 */
		acl_add_flag_return =
			acl_add_flag_np(flagset, ntacl_flag_table[i].aclflag);
		SMB_ASSERT(acl_add_flag_return == 0);

		darwin_flags |= ntacl_flag_table[i].aclflag;
	}

	DEBUG(4, ("%s: mapped NT flags %#x to Darwin flags %#x\n",
		MODULE_NAME, ace->flags, darwin_flags));
}

static uint32 map_perms_darwin_to_nt(acl_permset_t perms)
{
	uint32 ntperms = 0;
	int i;
	uint32 darwin_perms = 0;

	for (i = 0; i < ARRAY_SIZE(ntacl_perm_table); ++i) {
		acl_perm_t p = ntacl_perm_table[i].aclperm;

		if (acl_get_perm_np(perms, p) == 1) {
			ntperms |= ntacl_perm_table[i].ntperm;
			darwin_perms |= p;
		}
	}

	/* Log this harder if we didn't come up with a mapping. */
	DEBUG(darwin_perms == 0 ? 0 : 4,
		("%s: mapped Darwin permset %#x to NT permissions %#x\n",
		MODULE_NAME, darwin_perms, ntperms));

	return ntperms;
}

static void map_perms_nt_to_darwin(SEC_ACCESS ntperms, acl_permset_t permset)
{
	int i;
	int acl_add_perm_return;
	uint32 darwin_perms = 0;

	for (i = 0; i < ARRAY_SIZE(ntacl_perm_table); ++i) {
		if (ntperms & ntacl_perm_table[i].ntperm) {
			acl_perm_t p = ntacl_perm_table[i].aclperm;

			/* This can only fail if we messed up the
			 * mapping table. Hence the assert instead of
			 * an error return.
			 */
			acl_add_perm_return = acl_add_perm(permset, p);
			SMB_ASSERT(acl_add_perm_return == 0);
			darwin_perms |= p;
		}
	}

	if (ntperms & GENERIC_ALL_ACCESS) {
		acl_add_perm(permset, ACL_GENERIC_ALL);
		darwin_perms |= ACL_GENERIC_ALL;
	}

	if (ntperms & GENERIC_EXECUTE_ACCESS) {
		acl_add_perm(permset, ACL_GENERIC_EXECUTE);
		darwin_perms |= ACL_GENERIC_EXECUTE;
	}

	if (ntperms & GENERIC_WRITE_ACCESS) {
		acl_add_perm(permset, ACL_GENERIC_WRITE);
		darwin_perms |= ACL_GENERIC_WRITE;
	}

	if (ntperms & GENERIC_READ_ACCESS) {
		acl_add_perm(permset, ACL_GENERIC_READ);
		darwin_perms |= ACL_GENERIC_READ;
	}

	/* Log this harder if we didn't come up with a mapping. */
	DEBUG(darwin_perms == 0 ? 0 : 4,
		("%s: mapped NT permissions %#x to Darwin permset %#x\n",
		MODULE_NAME, ntperms, darwin_perms));
}

static int map_ace_darwin_to_nt(acl_tag_t tag_type)
{
	switch(tag_type) {
		case ACL_EXTENDED_ALLOW:
			return SEC_ACE_TYPE_ACCESS_ALLOWED;
		case ACL_EXTENDED_DENY:
			return SEC_ACE_TYPE_ACCESS_DENIED;
		case ACL_UNDEFINED_TAG:
		default:
			DEBUG(0,("map_ace_darwin_to_nt: !!!! ACL_UNDEFINED_TAG !!!!\n"));
			return SEC_ACE_TYPE_ACCESS_DENIED;
	}
}

static acl_tag_t map_ace_nt_to_darwin(uint32 ace)
{
	switch(ace)
	{
		case SEC_ACE_TYPE_ACCESS_ALLOWED:
			return ACL_EXTENDED_ALLOW ;
		case SEC_ACE_TYPE_ACCESS_DENIED :
			return ACL_EXTENDED_DENY;
		default:
			DEBUG(0,("map_ace_nt_to_darwin: !!!! ACL_UNDEFINED_TAG !!!!\n"));
			return ACL_UNDEFINED_TAG;
	}
}

/****************************************************************************
 Unpack a SEC_DESC into a UNIX owner and group.
****************************************************************************/

static BOOL darwin_unpack_nt_owners(int snum,
		uid_t *puid,
		gid_t *pgid,
		uint32 security_info_sent,
		SEC_DESC *psd)
{
	BOOL ret;

	ret = unpack_nt_owners(snum, puid, pgid, security_info_sent, psd);

	/* We default to uid/gid 99 (the reflective uid). */
	if (*puid == -1) {
		*puid = 99;
	}

	if (*pgid == -1) {
		*pgid = 99;
	}

	return ret;
}

/****************************************************************************
 Try to chown a file. We will be able to chown it under the following
 conditions:

  1) If we have root privileges, then it will just work.
  2) If we have write permission to the file and dos_filemodes is set
     then allow chown to the currently authenticated user.
****************************************************************************/

static int darwin_try_chown(files_struct * fsp, uid_t uid, gid_t gid)
{
	int ret;
	extern struct current_user current_user;
	files_struct *local_fsp;
	SMB_STRUCT_STAT st;
	NTSTATUS status;

	DEBUG(3, ("%s: trying to chown %s to uid=%ld gid=%ld\n",
		MODULE_NAME, fsp->fsp_name, (long)uid, (long)gid ));

	/* try the direct way first */
	if (fsp->fh->fd != -1) {
		ret = SMB_VFS_FCHOWN(fsp, fsp->fh->fd, uid, gid);
	} else {
		ret = SMB_VFS_CHOWN(fsp->conn, fsp->fsp_name, uid, gid);
	}

	if (ret == 0) {
		return 0;
	}

	if(!CAN_WRITE(fsp->conn) || !lp_dos_filemode(SNUM(fsp->conn))) {
		return -1;
	}

	if (SMB_VFS_STAT(fsp->conn, fsp->fsp_name, &st)) {
		return -1;
	}

	status = open_file_fchmod(fsp->conn, fsp->fsp_name, &st, &local_fsp);
	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}

	/* only allow chown to the current user. This is more secure,
	   and also copes with the case where the SID in a take ownership ACL is
	   a local SID on the users workstation
	*/
	uid = current_user.ut.uid;

	become_root();
	/* Keep the current file gid the same. */
	ret = SMB_VFS_FCHOWN(local_fsp, local_fsp->fh->fd, uid, (gid_t)-1);
	unbecome_root();

	close_file_fchmod(local_fsp);

	return ret;
}

/****************************************************************************
 SID <-> UUID mapping.
****************************************************************************/

/* memberd might have faked up a UUID for us. We need to do a
 * reverse lookup of the UUID and then check it with getpwuid/getgrgid.
 */
static BOOL validate_memberd_uuid(const uuid_t uuid)
{
	id_t id;
	int id_type;

	if (mbr_uuid_to_id(uuid, &id, &id_type) != 0) {
		char uustr[40];
		uuid_unparse(uuid, uustr);
		DEBUG(0, ("%s: unable to reverse map UUID %s\n",
		    MODULE_NAME, uustr));

		smb_panic("UUID mapping is broken");
	}

	switch (id_type) {
	case ID_TYPE_UID:
		if (getpwuid(id) == NULL) {
			DEBUG(10, ("%s: failing mapping for faked uid=%ld\n",
				MODULE_NAME, (long)id));
			return False;
		}

		break;
	case ID_TYPE_GID:
		if (getgrgid(id) == NULL) {
			DEBUG(10, ("%s: failing mapping for faked gid=%ld\n",
				MODULE_NAME, (long)id));
			return False;
		}

		break;
	default:
		smb_panic("mbr_uuid_to_id() gave an invalid ID type");
	}

	return True;
}

static BOOL map_uuid_to_sid(uuid_t *uuid, DOM_SID *sid)
{
	int id_type = -1;
	uid_t id = -1;
	char uustr[40];

	uuid_unparse(*uuid, uustr);
	DEBUG(10, ("%s: mapping UUID %s\n", MODULE_NAME, uustr));

	if (!validate_memberd_uuid(*uuid)) {
		DEBUG(10, ("%s: failing mapping for faked uuid=%s\n",
				MODULE_NAME, uustr));
	}

	if (mbr_uuid_to_id(*uuid, &id, &id_type) != 0) {
		DEBUG(4, ("%s: UUID -> SID mapping failed for %s: %s\n",
			    MODULE_NAME, uustr, strerror(errno)));
		return False;
	}

	switch (id_type) {
	case ID_TYPE_UID:
		DEBUG(10, ("%s: UUID %s -> uid=%ld\n",
			MODULE_NAME, uustr, (long)id));

		if (getpwuid(id) == NULL) {
			DEBUG(10, ("%s: failing mapping for faked uid=%ld\n",
				MODULE_NAME, (long)id));
			return False;
		}

		uid_to_sid(sid, id);
		break;
	case ID_TYPE_GID:
		DEBUG(10, ("%s: UUID %s -> gid=%ld\n",
			MODULE_NAME, uustr, (long)id));

		if (getgrgid(id) == NULL) {
			DEBUG(10, ("%s: failing mapping for faked gid=%ld\n",
				MODULE_NAME, (long)id));
			return False;
		}

		gid_to_sid(sid, id);
		break;
	default:
		smb_panic("mbr_uuid_to_id() gave an invalid ID type");
	}

	DEBUG(10, ("%s: mapped UUID to SID %s\n",
		MODULE_NAME, sid_string_static(sid)));

	return True;
}

static BOOL map_sid_to_uuid(const DOM_SID *sid, uuid_t *uuid)
{
	uid_t id = -1;
	nt_sid_t mbr_sid = {0};

	DEBUG(10, ("%s: mapping SID %s\n",
		    MODULE_NAME, sid_string_static(sid)));

	/* DOM_SID and nt_sid_t are not the same binary layout, so we have to
	 * manually convert.
	 */
	mbr_sid.sid_kind = sid->sid_rev_num;
	mbr_sid.sid_authcount = sid->num_auths;
	memcpy(mbr_sid.sid_authority, sid->id_auth,
		sizeof(mbr_sid.sid_authority));
	memcpy(mbr_sid.sid_authorities, sid->sub_auths,
		sizeof(uint32) * sid->num_auths);

	/* SID -> UID/GID -> UUID */
	if (mbr_sid_to_uuid(&mbr_sid, *uuid) == 0) {
		/* The memberd mapping will practically always fail. Most of
		 * our SIDs are algorithmically generated and the memberd SID
		 * conversion only succeeds for static SIDs.
		 */
		if (validate_memberd_uuid(*uuid)) {
			goto success;
		}
	}

	DEBUG(4, ("%s: SID -> UUID mapping failed for %s: %s\n",
		MODULE_NAME, sid_string_static(sid), strerror(errno)));

	/* XXX This conversion is suspect because we don't really know what
	 * type of SID we have here. sid_to_uid() can end up doing the wrong
	 * conversion with algorithmic SID mapping.
	 */
	if (sid_to_uid(sid, &id)) {
		if (mbr_uid_to_uuid(id, *uuid) == 0) {
			goto success;
		}

		DEBUG(4, ("%s: UID -> UUID mapping failed for uid=%ld: %s\n",
			MODULE_NAME, (long)id, strerror(errno)));

	} else if (sid_to_gid(sid, &id)) {
		if (mbr_gid_to_uuid(id, *uuid) == 0) {
			goto success;
		}

		DEBUG(4, ("%s: GID -> UUID mapping failed for gid=%ld: %s\n",
			MODULE_NAME, (long)id, strerror(errno)));
	}

	DEBUG(0, ("%s: failed to map SID %s to a UUID\n",
		    MODULE_NAME, sid_string_static(sid)));

	return False;

success:
	if (DEBUGLVL(10)) {
		char uustr[40];
		uuid_unparse(*uuid, uustr);
		DEBUG(10, ("%s: mapped SID to UUID %s\n", MODULE_NAME, uustr));
	}

	return True;
}

#define UNIX_PERM_TO_ACL_ACCESS(bit) \
	unix_perms_to_acl_perms(mode, 0xffffffff, 0xffffffff, 0xffffffff)

static uint32 unix_perms_to_acl_perms(mode_t mode, int r_mask, int w_mask, int x_mask)
{
	int ret = 0;

	if (mode & r_mask)
		ret |= FILE_SPECIFIC_READ_BITS;
	if (mode & w_mask)
		ret |= FILE_SPECIFIC_WRITE_BITS;
	if (mode & x_mask)
		ret |= FILE_SPECIFIC_EXECUTE_BITS;

	return ret;
}

/* Try to figure out whether any of the ACEs can be represented in POSIX
 * mode bits. Really, this is a fool's errand and we should just give up
 * on it :(((  --jpeach
 */
static mode_t map_ntacl_to_mode(files_struct *fsp,
		    SEC_ACL *dacl,
		    DOM_SID *owner_sid,
		    DOM_SID *group_sid,
		    mode_t mode)
{

	int i;
	SEC_ACE *sec_ace = NULL;
	mode_t r_mode = 0;

	uint32 user_allowed = 0;
	uint32 group_allowed = 0;
	uint32 other_allowed = 0;

	uint32 user_denied = 0;
	uint32 group_denied = 0;
	uint32 other_denied = 0;

	uint32 unix_access;

	if (dacl == NULL) {
		return mode; /* just return the passed mode */
	}

	for (i = 0; i < dacl->num_aces; i++) {
		sec_ace = &dacl->aces[i];

		if (sec_ace->type != SEC_ACE_TYPE_ACCESS_ALLOWED &&
		    sec_ace->type != SEC_ACE_TYPE_ACCESS_DENIED) {
			DEBUG(4, ("%s: ignoring unsupported ACL type %d\n",
				    MODULE_NAME, sec_ace->type));
			continue;
		}

		if (sec_ace->type == SEC_ACE_TYPE_ACCESS_ALLOWED) {

			if (sid_equal(&sec_ace->trustee, owner_sid)) {
				user_allowed |= sec_ace->access_mask;
			} else if (sid_equal(&sec_ace->trustee, group_sid)) {
				group_allowed |= sec_ace->access_mask;
			} else if (sid_equal(&sec_ace->trustee,
						&global_sid_World)) {
				other_allowed |= sec_ace->access_mask;
			}

		} else if (sec_ace->type == SEC_ACE_TYPE_ACCESS_DENIED) {

			if (sid_equal(&sec_ace->trustee, owner_sid)) {
				user_denied |= sec_ace->access_mask;
			} else if (sid_equal(&sec_ace->trustee, group_sid)) {
				group_denied |= sec_ace->access_mask;
			} else if (sid_equal(&sec_ace->trustee,
						&global_sid_World)) {
				other_denied |= sec_ace->access_mask;
			}
		}
	}

	if (user_allowed == 0 && group_allowed == 0 && other_allowed == 0) {
		DEBUG(4, ("%s: no owner access granted, setting mode to 0\n",
			    MODULE_NAME));
		return 0;
	}

	/* Check each UNIX mode bit. If we have a cumulative ACL access mode
	 * that exactly matched the mode bit mapping, then set the bit in the
	 * final mode. We will also remove these access bits from the
	 * corresponding ACL.
	 */
	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IRUSR);
	if (MASK_MATCH_ALL(user_allowed, unix_access) &&
	    !MASK_MATCH_ANY(user_denied, unix_access)) {
		r_mode |= (S_IRUSR);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IWUSR);
	if (MASK_MATCH_ALL(user_allowed, unix_access) &&
	    !MASK_MATCH_ANY(user_denied, unix_access)) {
		r_mode |= (S_IWUSR);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IXUSR);
	if (MASK_MATCH_ALL(user_allowed, unix_access) &&
	    !MASK_MATCH_ANY(user_denied, unix_access)) {
		r_mode |= (S_IXUSR);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IRGRP);
	if (MASK_MATCH_ALL(group_allowed, unix_access) &&
	    !MASK_MATCH_ANY(group_denied, unix_access)) {
		r_mode |= (S_IRGRP);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IWGRP);
	if (MASK_MATCH_ALL(group_allowed, unix_access) &&
	    !MASK_MATCH_ANY(group_denied, unix_access)) {
		r_mode |= (S_IWGRP);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IXGRP);
	if (MASK_MATCH_ALL(group_allowed, unix_access) &&
	    !MASK_MATCH_ANY(group_denied, unix_access)) {
		r_mode |= (S_IXGRP);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IROTH);
	if (MASK_MATCH_ALL(other_allowed, unix_access) &&
	    !MASK_MATCH_ANY(other_denied, unix_access)) {
		r_mode |= (S_IROTH);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IWOTH);
	if (MASK_MATCH_ALL(other_allowed, unix_access) &&
	    !MASK_MATCH_ANY(other_denied, unix_access)) {
		r_mode |= (S_IWOTH);
	}

	unix_access = UNIX_PERM_TO_ACL_ACCESS(S_IXOTH);
	if (MASK_MATCH_ALL(other_allowed, unix_access) &&
	    !MASK_MATCH_ANY(other_denied, unix_access)) {
		r_mode |= (S_IXOTH);
	}

	DEBUG(4, ("%s: mapped owner ACEs to mode 0%o\n",
	    MODULE_NAME, r_mode));

	if (r_mode == 0) {
		return 0;
	}

	/* If we found a set of ACL permissions that can be represented in a
	 * set of mode bits, go back through the ACL and turn off all the bits
	 * that will be represented in the mode. Later on, if some of the
	 * access masks go to zero, we can simply ignore that ACE.
	 */
	for (i = 0; i < dacl->num_aces; i++) {
		sec_ace = &dacl->aces[i];

		if (sec_ace->type != SEC_ACE_TYPE_ACCESS_ALLOWED) {
			continue;
		}

		if (sid_equal(&sec_ace->trustee, owner_sid)) {
			unix_access = unix_perms_to_acl_perms(r_mode,
				S_IRUSR, S_IWUSR, S_IXUSR);
			sec_ace->access_mask &= ~unix_access;
		} else if ( sid_equal(&sec_ace->trustee, group_sid)) {
			unix_access = unix_perms_to_acl_perms(r_mode,
				S_IRGRP, S_IWGRP, S_IXGRP);
			sec_ace->access_mask &= ~unix_access;
		} else if (sid_equal(&sec_ace->trustee, &global_sid_World)) {
			unix_access = unix_perms_to_acl_perms(r_mode,
				S_IROTH, S_IWOTH, S_IXOTH);
			sec_ace->access_mask &= ~unix_access;
		}
	}

	return r_mode;
}

static int map_mode_to_ntacl(filesec_t fsect, struct sec_ace_list *acelist)
{
	DOM_SID owner_sid;
	DOM_SID group_sid;
	uid_t uid;
	gid_t gid;
	mode_t mode;
	int acl_perms = 0;
	int num_aces = 0;
	SEC_ACCESS acc = 0;

	if (filesec_get_property(fsect, FILESEC_OWNER, &uid) == -1) {
		DEBUG(0,("%s: filesec_get_property(FILESEC_OWNER): %s (%d)\n",
			    MODULE_NAME, strerror(errno), errno));
		return 0;
	}

	if (filesec_get_property(fsect, FILESEC_GROUP, &gid) == -1) {
		DEBUG(0,("%s: filesec_get_property(FILESEC_GROUP): %s (%d)\n",
			    MODULE_NAME, strerror(errno), errno));
		return 0;
	}

	if (filesec_get_property(fsect, FILESEC_MODE, &mode) == -1) {
		DEBUG(0,("%s: filesec_get_property(FILESEC_MODE): %s (%d)\n",
			    MODULE_NAME, strerror(errno), errno));
		return 0;
	}

	if (DEBUGLVL(4)) {
		DEBUGADD(4, ("%s: filesec security properties:\n",
			MODULE_NAME));
		DEBUGADD(4,("    FILESEC_OWNER[%d]\n", uid));
		DEBUGADD(4,("    FILESEC_GROUP[%d]\n", gid));
		DEBUGADD(4,("    FILESEC_MODE[0%o]\n", mode));
	}

	/* Don't add any ACEs if the mode is not set. */
	if (mode == 0) {
		return 0;
	}

	uid_to_sid(&owner_sid, uid);
	gid_to_sid(&group_sid, gid);

	/* Create enough space for user, group and everyone entries. */
	while (acelist->ace_length - acelist->ace_count < 3) {
		if (!ace_list_grow(acelist)) {
			return 0;
		}
	}

	/* user */
	acl_perms = unix_perms_to_acl_perms(mode, S_IRUSR, S_IWUSR, S_IXUSR);
	if (acl_perms) {
		++num_aces;
		init_sec_access(&acc, acl_perms);
		ace_list_append_ace(acelist, &owner_sid,
				SEC_ACE_TYPE_ACCESS_ALLOWED, acc, 0);
	}

	/* group */
	acl_perms = unix_perms_to_acl_perms(mode, S_IRGRP, S_IWGRP, S_IXGRP);
	if (acl_perms) {
		++num_aces;
		init_sec_access(&acc, acl_perms);
		ace_list_append_ace(acelist, &group_sid,
				SEC_ACE_TYPE_ACCESS_ALLOWED, acc, 0);
	}

	/* everyone */
	acl_perms = unix_perms_to_acl_perms(mode, S_IROTH, S_IWOTH, S_IXOTH);
	if (acl_perms) {
		++num_aces;
		init_sec_access(&acc, acl_perms);
		ace_list_append_ace(acelist, &global_sid_World,
				SEC_ACE_TYPE_ACCESS_ALLOWED, acc, 0);
	}

	DEBUG(4,("%s: %d ACEs created from mode 0%o\n",
		MODULE_NAME, num_aces, mode));

	return num_aces;
}

static int map_darwinacl_to_ntacl(filesec_t fsect,
		    struct sec_ace_list *acelist)
{
	static const char const func[] = "map_darwinacl_to_nt_acl";

	acl_t darwin_acl = NULL;
	acl_entry_t entry = NULL;
	acl_tag_t tag_type; /* ACL_EXTENDED_ALLOW | ACL_EXTENDED_DENY */
	acl_flagset_t flags; /* inheritance bits */
	acl_permset_t perms;
	uuid_t *qualifier = NULL; /* user or group */

	SEC_ACCESS acc;
	DOM_SID sid;

	int num_aces = 0;

	if (filesec_get_property(fsect, FILESEC_ACL, &darwin_acl) == -1) {
		DEBUG(3,("%s: filesec_get_property - FILESEC_ACL: %s (%d)\n",
			    func, strerror(errno), errno));
		return 0;
	}

	if (DEBUGLVL(8)) {
		char * aclstr = acl_to_text(darwin_acl, NULL);

		if (aclstr) {
		    DEBUG(8, ("%s: source Darwin ACL is:\n"));
		    DEBUGADD(8, ("%s\n", aclstr));
		    acl_free(aclstr);
		} else {
		    DEBUG(8, ("%s: no source ACL\n"));
		}
	}

	FOREACH_ACE(darwin_acl, entry) {
		BOOL ret;
		uint32 mask;

		if ((qualifier = acl_get_qualifier(entry)) == NULL)
			continue;
		if (acl_get_tag_type(entry, &tag_type) != 0)
			continue;
		if (acl_get_flagset_np(entry, &flags) != 0)
			continue;
		if (acl_get_permset(entry, &perms) != 0)
			continue;

		if (!map_uuid_to_sid(qualifier, &sid)) {
			acl_free(qualifier);
			continue;
		} else {
			acl_free(qualifier);
		}

		mask = map_perms_darwin_to_nt(perms);
		if (mask == 0) {
			DEBUG(4, ("%s: ignoring ACE mapped to empty permission set\n",
				func));
			continue;
		}

		mask |= SYNCHRONIZE_ACCESS;
		init_sec_access(&acc, mask);

		DEBUG(4,("%s: acc(%X) tag_type(%x), flags(%X)\n",
			    func, acc, tag_type, flags ));

		ret = ace_list_append_ace(acelist, &sid,
				map_ace_darwin_to_nt(tag_type), acc,
				map_flags_darwin_to_nt(flags));
		if (!ret) {
			return 0;
		}

		++num_aces;
	}

	DEBUG(4, ("%s: mapped %d ACEs\n", func, num_aces));
	return num_aces;
}

static size_t darwin_get_nt_acl_internals(vfs_handle_struct *handle,
		    files_struct *fsp,
		    uint32 security_info,
		    SEC_DESC **ppdesc)
{
	static const char const func[] = "darwin_get_nt_acl_internals";

	DOM_SID owner_sid;
	DOM_SID group_sid;
	uid_t owner_uid;
	gid_t owner_gid;
	SEC_ACL *sec_acl = NULL;
	SEC_DESC *psd = NULL;
	size_t sd_size = 0;
	filesec_t fsec = NULL;

	DEBUG(4,("%s: called for file %s\n", func, fsp->fsp_name));

	fsec = fsp_get_filesec(fsp);
	if (fsec == NULL) {
		goto cleanup;
	}

	filesec_get_property(fsec, FILESEC_OWNER, &owner_uid);
	filesec_get_property(fsec, FILESEC_GROUP, &owner_gid);
	uid_to_sid(&owner_sid, owner_uid);
	gid_to_sid(&group_sid, owner_gid);

	if (security_info & DACL_SECURITY_INFORMATION) {
		struct sec_ace_list acelist;

		if (!ace_list_new(main_loop_talloc_get(), &acelist)) {
			goto build_sec_desc;
		}

		map_darwinacl_to_ntacl(fsec, &acelist);
		map_mode_to_ntacl(fsec, &acelist);

		if (acelist.ace_count == 0) {
			DEBUG(4,("%s: No ACLs on file (%s)\n",
				    func, fsp->fsp_name ));
			ace_list_free(&acelist);
			goto build_sec_desc;
		}

		sec_acl = make_sec_acl(main_loop_talloc_get(), NT4_ACL_REVISION,
			acelist.ace_count, acelist.ace_list);
		if (!sec_acl) {
			DEBUG(0,("%s: Unable to malloc space for ACL\n", func));
			ace_list_free(&acelist);
			goto build_sec_desc;
		}

		ace_list_free(&acelist);

	} /* security_info & DACL_SECURITY_INFORMATION */

build_sec_desc:
	/* Build the final security descriptor. Make sure we always provide a
	 * DACL, even if it is empty. No DACL in interpreted as full access,
	 * whereas a DACL with no ACEs is interpreted as no access.
	 */
	psd = make_standard_sec_desc( main_loop_talloc_get(),
		(security_info & OWNER_SECURITY_INFORMATION) ? &owner_sid
							     : NULL,
		(security_info & GROUP_SECURITY_INFORMATION) ? &group_sid
							     : NULL,
		sec_acl ? sec_acl : &empty_acl,
		&sd_size);

	if (!psd) {
		DEBUG(0,("%s: Unable to malloc security descriptor\n", func));
		sd_size = 0;
	}

	if (psd->dacl) {
		/* Make sure that we mark this SEC_DESC as protected if only
		 * none of the ACEs were inherited.
		 */
		int i;
		psd->type |= SE_DESC_DACL_PROTECTED;
		for (i = 0; i < psd->dacl->num_aces; i++) {
			if (psd->dacl->aces[i].flags &
			    SEC_ACE_FLAG_INHERITED_ACE) {
				psd->type &= ~SE_DESC_DACL_PROTECTED;
				break;
			}
		}

		dacl_sort_into_canonical_order(psd->dacl->aces,
				    (unsigned)psd->dacl->num_aces);
	}

	*ppdesc = psd;

cleanup:
 	if (NULL != fsec) {
		filesec_free(fsec);
	}

	return sd_size;
}

/* Map a Windows DACL to a Darwin ACL, inheriting ACEs if necessary. In
 * general, we prefer to fail the entire operation than to allow it to
 * partially succeed. If we allow it to partially succeed, then the
 * resulting ACL is undefined, which might lead to unexpected access.
 */
static BOOL map_ntacl_to_darwinacl(SEC_ACL *dacl,
		acl_t original_acl,
		acl_t *darwin_acl,
		uint16 desc_flags)
{
	static const char const func[] = "map_ntacl_to_darwinacl";

	SEC_ACE *psa = NULL;
	int i = 0;

	SMB_ASSERT(darwin_acl != NULL);
	SMB_ASSERT(dacl != NULL);

	*darwin_acl = acl_init(dacl->num_aces);
	if (*darwin_acl == NULL) {
		DEBUG(0,("%s: [acl_init] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
		return False;
	}

	for(i = 0; i < dacl->num_aces; i++) {
		acl_entry_t darwin_acl_entry = NULL;
		acl_permset_t permset;
		acl_flagset_t flagset;
		uuid_t	uuid;

		psa = &dacl->aces[i];

		DEBUG(4, ("%s: entry [%d]\n", func, i));

		if((psa->type != SEC_ACE_TYPE_ACCESS_ALLOWED) &&
		   (psa->type != SEC_ACE_TYPE_ACCESS_DENIED)) {
			DEBUG(4, ("%s: unable to set anything but "
				"an ALLOW or DENY ACE\n", func));
			continue;
		}

		if (DEBUGLVL(10)) {
			DEBUG(10, ("%s: mapping ACE for SID %s\n",
			    MODULE_NAME, sid_string_static(&psa->trustee)));
		}

		/* XXX This breaks the Samba4 RAW-ACLS tests. This test expects
		 * to be able to set an ACL containing an ACE with a zero
		 * access mask and then retrieve it.
		 */
		if (psa->access_mask == 0) {
			DEBUG(4, ("%s: ignoring ACE with empty access mask\n",
				func));
			continue;
		}

		/* If we can't map the SID to a real UUID, we should fail the
		 * whole operation. We cannot allow ACLs to accumulate bogus
		 * UUIDs or SIDs.
		 */
		if (!map_sid_to_uuid(&psa->trustee, &uuid)) {
			goto failed;
		}

		if (acl_create_entry(darwin_acl, &darwin_acl_entry) != 0) {
			DEBUG(0,("%s: [acl_create_entry] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
			goto failed;
		}

		if (acl_set_tag_type(darwin_acl_entry,
				map_ace_nt_to_darwin(psa->type)) != 0) {
			DEBUG(0,("%s: [acl_set_tag_type] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
			goto failed;
		}

		if (acl_set_qualifier(darwin_acl_entry, &uuid) != 0) {
			DEBUG(0,("%s: [acl_set_qualifier] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
			goto failed;
		}

		if (acl_get_permset(darwin_acl_entry, &permset) != 0) {
			DEBUG(0,("%s: [acl_get_permset] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
			goto failed;
		}

		acl_clear_perms(permset);
		map_perms_nt_to_darwin(psa->access_mask, permset);

		/* XXX There is no ACL API to test whether the permset is
		 * clear, so we test whether any of the perm bits are set.
		 * This is an abuse of the acl_get_perm_np API, since you are
		 * not supposed to pass a bitmask to it.
		 */
		if (acl_get_perm_np(permset, ACL_ALL_PERMISSIONS) != 1) {
			DEBUG(4, ("%s: ignoring ACE mapped to empty permission set\n",
				func));

			acl_delete_entry(*darwin_acl, darwin_acl_entry);
			continue;
		}

		if (acl_set_permset(darwin_acl_entry, permset) != 0) {
			DEBUG(0, ("%s: [acl_set_permset] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
			goto failed;
		}

		if (acl_get_flagset_np(darwin_acl_entry, &flagset) != 0) {
			DEBUG(0,("%s: [acl_get_flagset_np] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
			goto failed;
		}

		map_flags_nt_to_darwin(psa, flagset);
		if (acl_set_flagset_np(darwin_acl_entry, flagset) != 0) {
			DEBUG(0,("%s: [acl_set_flagset_np] errno(%d) - (%s)\n",
				func, errno, strerror(errno)));
			goto failed;
		}
	}

	/* We have a previous ACL and the new ACL is inheriting ACEs. */
	if (original_acl && !(desc_flags & SE_DESC_DACL_PROTECTED)) {
		int entry_id = ACL_FIRST_ENTRY;
		acl_entry_t current_entry = NULL;
		acl_entry_t  new_entry = NULL;
		acl_flagset_t flagset;

		/* Iterate over the entries in the original ACL. If any
		 * of them should be inherited, copy them to the new ACL.
		 */
		FOREACH_ACE(original_acl, current_entry) {

			if (acl_get_flagset_np(current_entry, &flagset) != 0) {
				DEBUG(0,("%s: [acl_get_flagset_np] "
						"errno(%d) - (%s)\n",
					func, errno, strerror(errno)));
				goto failed;
			}

			if (!acl_get_flag_np(flagset, ACL_ENTRY_INHERITED)) {
				continue;
			}

			if (acl_create_entry(darwin_acl, &new_entry) == -1) {
				DEBUG(0,("%s: [acl_create_entry] "
						"errno(%d) - (%s)\n",
					func, errno, strerror(errno)));
				goto failed;
			}

			if (acl_copy_entry(new_entry, current_entry) == 0) {
				DEBUG(3,("%s: [acl_copy_entry] "
						"COPIED INHERITED ACE (%d)\n",
					func, entry_id));
			} else {
				DEBUG(0,("%s: [acl_copy_entry] "
						"errno(%d) - (%s)\n",
					func, errno, strerror(errno)));
				goto failed;
			}
		}
	}

	DEBUG(4,("%s succeeded\n", func));
	return True;

failed:

	acl_free(*darwin_acl);
	*darwin_acl = NULL;

	DEBUG(4,("%s failed\n", func));
	return False;
}

static BOOL darwin_set_nt_acl_internals(vfs_handle_struct *handle,
			files_struct *fsp,
			uint32 security_info_sent,
			SEC_DESC *psd)
{
	extern struct current_user current_user;

	uid_t uid = 99; /* unknown */
	gid_t gid = 99; /* unknown */
	DOM_SID owner_sid;
	DOM_SID group_sid;
	mode_t orig_mode = 0;
	mode_t new_mode = 0;
	uid_t orig_uid;
	gid_t orig_gid;
	BOOL need_chown = False;
	acl_t darwin_acl = NULL;
	acl_t original_acl = NULL;
	filesec_t fsec;

	DEBUG(4,("darwin_set_nt_acl_internals: called for file %s\n",
		fsp->fsp_name ));

	if (!CAN_WRITE(fsp->conn)) {
		DEBUG(10,("darwin_set_nt_acl_internals: set acl rejected "
			    "on read-only share\n"));
		return False;
	}

	/*
	 * Get the current state of the file.
	 */

	fsec = fsp_get_filesec(fsp);
	if (fsec == NULL) {
		return False;
	}

	/* Save the original elements we check against. */
	filesec_get_property(fsec, FILESEC_MODE, &orig_mode);
	filesec_get_property(fsec, FILESEC_OWNER, &orig_uid);
	filesec_get_property(fsec, FILESEC_GROUP, &orig_gid);

	/*
	 * Unpack the user/group/world id's.
	 */

	if (!darwin_unpack_nt_owners(SNUM(fsp->conn), &uid, &gid,
				    security_info_sent, psd)) {
		filesec_free(fsec);
		return False;
	}

	/*
	 * Do we need to chown ?
	 */

	if (((uid != (uid_t)99) && (orig_uid != uid)) ||
	    ((gid != (gid_t)99) && (orig_gid != gid))) {
		need_chown = True;
	}

	/*
	 * Chown before setting ACL only if we don't change the user, or
	 * if we change to the current user, but not if we want to give away
	 * the file.
	 */

	if (need_chown && (uid == (uid_t)99 || uid == current_user.ut.uid)) {

		filesec_free(fsec);

		if (darwin_try_chown(fsp, uid, gid) == -1) {
			DEBUG(3, ("%s: chown %s to uid=%u, gid=%u failed: %s\n",
				MODULE_NAME, fsp->fsp_name,
				(long)uid, (long)gid,
				strerror(errno) ));
			return False;
		}

		/*
		 * Recheck the current state of the file, which may have
		 * changed (suid/sgid bits, for instance).
		 */

		fsec = fsp_get_filesec(fsp);
		if (fsec == NULL) {
			return False;
		}


		/* Save the original elements we check against. */
		filesec_get_property(fsec, FILESEC_MODE, &orig_mode);
		filesec_get_property(fsec, FILESEC_OWNER, &orig_uid);
		filesec_get_property(fsec, FILESEC_GROUP, &orig_gid);

		/* We did it, don't try again */
		need_chown = False;
	}

	uid_to_sid(&owner_sid, orig_uid);
	gid_to_sid(&group_sid, orig_gid);

	if (security_info_sent == 0) {
		filesec_free(fsec);
		return False;
	}

	/*
	 * If no DACL then this is a chown only security descriptor.
	 */

	if(!(security_info_sent & DACL_SECURITY_INFORMATION) || !psd->dacl) {
		filesec_free(fsec);
		return True;
	}

	/* Figure out whether we can map any of the DACL into mode bits. */
	new_mode = map_ntacl_to_mode(fsp, psd->dacl,
		 		&owner_sid, &group_sid, orig_mode);

	if (filesec_get_property(fsec, FILESEC_ACL, &original_acl) == -1) {
		/* Most likely no ACL on this file (ENOENT). */
		original_acl = NULL;
	}

	/* Figure out the corresponding Darwin ACL. */
	if (!map_ntacl_to_darwinacl(psd->dacl, original_acl,
				    &darwin_acl, psd->type)) {
		filesec_free(fsec);
		return False;
	}

	/* Now that we have all the information we need, set the ACL and update
	 * the mode bits.
	 */
	filesec_free(fsec);
	if (fsp_set_acl(fsp, darwin_acl) != 0) {
		return False;
	}

	if (orig_mode != new_mode) {
		if (SMB_VFS_CHMOD(fsp->conn, fsp->fsp_name,
					    new_mode) == -1) {
			DEBUG(3,("%s: failed to chmod %s, "
				"from 0%o to 0%o: %s\n",
				fsp->fsp_name,
				(unsigned int)orig_mode,
				(unsigned int)new_mode,
				strerror(errno) ));
			return False;
		}
	}

	/* Any chown pending? */
	if (need_chown && darwin_try_chown(fsp, uid, gid) == -1) {
		DEBUG(3, ("%s: chown %s to uid=%u, gid=%u failed: %s\n",
			MODULE_NAME, fsp->fsp_name, (long)uid, (long)gid,
			strerror(errno) ));
		return False;
	}

	return True;
}

/****************************************************************************
 VFS entry points.
****************************************************************************/

static BOOL darwin_fset_nt_acl(vfs_handle_struct *handle,
			    files_struct *fsp,
			    int fd,
			    uint32 security_info_sent,
			    SEC_DESC *psd)
{
	BOOL acl_support = False;

	acl_support = acl_support_enabled(handle->conn);
	DEBUG(4,("darwin_fset_nt_acl: called for file %s acl_support(%d)\n",
		    fsp->fsp_name, acl_support));

	if (acl_support) {
		SMB_ASSERT(fsp->fh->fd == fd);
		return darwin_set_nt_acl_internals(handle, fsp,
				    security_info_sent, psd);
	}

	return SMB_VFS_NEXT_FSET_NT_ACL(handle, fsp, fd,
			    security_info_sent, psd);
}

static BOOL darwin_set_nt_acl(vfs_handle_struct *handle,
			    files_struct *fsp,
			    const char *name,
			    uint32 security_info_sent,
			    SEC_DESC *psd)
{
	BOOL acl_support = False;

	acl_support = acl_support_enabled(handle->conn);
	DEBUG(4,("darwin_set_nt_acl: called for file %s acl_support(%d)\n",
		    fsp->fsp_name, acl_support));

	if (acl_support) {
		return darwin_set_nt_acl_internals(handle, fsp,
				security_info_sent, psd);
	}

	return SMB_VFS_NEXT_SET_NT_ACL(handle, fsp, name,
			security_info_sent, psd);
}

static size_t darwin_fget_nt_acl(vfs_handle_struct *handle,
		    files_struct *fsp,
		    int fd,
		    uint32 security_info,
		    SEC_DESC **ppdesc)
{
	BOOL acl_support = False;

	acl_support = acl_support_enabled(handle->conn);
	DEBUG(4,("darwin_fget_nt_acl: called for file %s acl_support(%d)\n",
		    fsp->fsp_name, acl_support));

	if (acl_support) {
		SMB_ASSERT(fsp->fh->fd == fd);
		return darwin_get_nt_acl_internals(handle, fsp,
				security_info, ppdesc);
	}

	return SMB_VFS_NEXT_FGET_NT_ACL(handle, fsp, fd,
			security_info, ppdesc);
}

static size_t darwin_get_nt_acl(vfs_handle_struct *handle,
		    files_struct *fsp,
		    const char *name,
		    uint32 security_info,
		    SEC_DESC **ppdesc)
{
	BOOL acl_support = False;

	acl_support = acl_support_enabled(handle->conn);
	DEBUG(4,("darwin_get_nt_acl: called for file %s acl_support(%d)\n",
		    fsp->fsp_name, acl_support));

	if (acl_support) {
		return darwin_get_nt_acl_internals(handle, fsp,
				security_info, ppdesc);
	}

	return SMB_VFS_NEXT_GET_NT_ACL(handle, fsp, name,
			security_info, ppdesc);
}

/* VFS operations structure */

static vfs_op_tuple darwin_acls_ops[] = {
	{SMB_VFS_OP(darwin_fget_nt_acl),	SMB_VFS_OP_FGET_NT_ACL,	SMB_VFS_LAYER_TRANSPARENT},
	{SMB_VFS_OP(darwin_get_nt_acl),		SMB_VFS_OP_GET_NT_ACL,	SMB_VFS_LAYER_TRANSPARENT},
	{SMB_VFS_OP(darwin_fset_nt_acl),	SMB_VFS_OP_FSET_NT_ACL,	SMB_VFS_LAYER_TRANSPARENT},
	{SMB_VFS_OP(darwin_set_nt_acl),		SMB_VFS_OP_SET_NT_ACL,	SMB_VFS_LAYER_TRANSPARENT},

	{SMB_VFS_OP(NULL),			SMB_VFS_OP_NOOP,		SMB_VFS_LAYER_NOOP}
};

NTSTATUS vfs_darwinacl_init(void)
{
	return smb_register_vfs(SMB_VFS_INTERFACE_VERSION,
				MODULE_NAME, darwin_acls_ops);
}
