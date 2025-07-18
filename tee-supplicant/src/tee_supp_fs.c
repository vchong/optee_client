/*
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <handle.h>
#include <libgen.h>
#include <optee_msg_supplicant.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <teec_trace.h>
#include <tee_supp_fs.h>
#include <tee_supplicant.h>
#include <unistd.h>

#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#include <linux/tee.h>

#ifndef PATH_MAX
#define PATH_MAX 255
#endif

/* Path to all secure storage files. */
static int tee_fs_fd = -1;

static pthread_mutex_t dir_handle_db_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct handle_db dir_handle_db =
		HANDLE_DB_INITIALIZER_WITH_MUTEX(&dir_handle_db_mutex);

static TEEC_Result errno_to_teec(int err)
{
	switch (err) {
	case ENOSPC:
		return TEEC_ERROR_STORAGE_NO_SPACE;
	case ENOENT:
		return TEEC_ERROR_ITEM_NOT_FOUND;
	default:
		break;
	}
	return TEEC_ERROR_GENERIC;
}

static size_t tee_fs_get_relative_filename(char *file, char *out,
					   size_t out_size)
{
	int s = 0;

	if (!file || !out || (out_size <= strlen(file) + 3))
		return 0;

	s = snprintf(out, out_size, "./%s", file);
	if (s < 0 || (size_t)s >= out_size)
		return 0;

	/* Safe to cast since we have checked that sizes are OK */
	return (size_t)s;
}

static void fs_fsync(void)
{
	if (tee_fs_fd > 0)
		fsync(tee_fs_fd);
}

static int do_mkdir(const char *path, mode_t mode)
{
	struct stat st;

	memset(&st, 0, sizeof(st));

	if (mkdir(path, mode) != 0 && errno != EEXIST)
		return -1;

	if (stat(path, &st) != 0 && !S_ISDIR(st.st_mode))
		return -1;

	return 0;
}

static int mkpath(const char *path, mode_t mode)
{
	int status = 0;
	char *subpath = strdup(path);
	char *prev = subpath;
	char *curr = NULL;

	while (status == 0 && (curr = strchr(prev, '/')) != 0) {
		/*
		 * Check for root or double slash
		 */
		if (curr != prev) {
			*curr = '\0';
			status = do_mkdir(subpath, mode);
			*curr = '/';
		}
		prev = curr + 1;
	}
	if (status == 0)
		status = do_mkdir(path, mode);

	free(subpath);
	return status;
}

static int tee_supp_fs_init(void)
{
	mode_t mode = 0700;

	if (mkpath(supplicant_params.fs_parent_path, mode) != 0)
		return -1;

	tee_fs_fd = open(supplicant_params.fs_parent_path, O_RDONLY);
	if (tee_fs_fd < 0)
		return -1;
	fs_fsync();

	return 0;
}

static int openat_wrapper(const char *fname, int flags)
{
	int fd = 0;

	while (true) {
		fd = openat(tee_fs_fd, fname, flags | O_SYNC, 0600);
		if (fd >= 0 || errno != EINTR)
			return fd;
	}
}

static TEEC_Result ree_fs_new_open(size_t num_params,
				   struct tee_ioctl_param *params)
{
	char rel_filename[PATH_MAX] = { 0 };
	char *fname = NULL;
	int fd = 0;

	if (num_params != 3 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT ||
	    (params[2].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fname = tee_supp_param_to_va(params + 1);
	if (!fname)
		return TEEC_ERROR_BAD_PARAMETERS;

	if (!tee_fs_get_relative_filename(fname, rel_filename,
					  sizeof(rel_filename)))
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = openat_wrapper(rel_filename, O_RDWR);
	if (fd < 0) {
		/*
		 * In case the problem is the filesystem is RO, retry with the
		 * open flags restricted to RO.
		 */
		fd = openat_wrapper(rel_filename, O_RDONLY);
		if (fd < 0)
			return TEEC_ERROR_ITEM_NOT_FOUND;
	}

	params[2].a = fd;
	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_create(size_t num_params,
				     struct tee_ioctl_param *params)
{
	char rel_filename[PATH_MAX] = { 0 };
	char rel_dir[PATH_MAX] = { 0 };
	char *fname = NULL;
	char *d = NULL;
	int fd = 0;
	const int flags = O_RDWR | O_CREAT | O_TRUNC;

	if (num_params != 3 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT ||
	    (params[2].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fname = tee_supp_param_to_va(params + 1);
	if (!fname)
		return TEEC_ERROR_BAD_PARAMETERS;

	if (!tee_fs_get_relative_filename(fname, rel_filename,
					  sizeof(rel_filename)))
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = openat_wrapper(rel_filename, flags);
	if (fd >= 0)
		goto out;
	if (errno != ENOENT)
		return errno_to_teec(errno);

	/* Directory for file missing, try to make it */
	strncpy(rel_dir, rel_filename, sizeof(rel_dir));
	rel_dir[sizeof(rel_dir) - 1] = '\0';
	d = dirname(rel_dir);
	if (!mkdirat(tee_fs_fd, d, 0700)) {
		int err = 0;

		fd = openat_wrapper(rel_filename, flags);
		if (fd >= 0)
			goto out;
		/*
		 * The directory was made but the file could still not be
		 * created.
		 */
		err = errno;
		unlinkat(tee_fs_fd, d, AT_REMOVEDIR);
		return errno_to_teec(err);
	}
	if (errno != ENOENT)
		return errno_to_teec(errno);

	/* Parent directory for file missing, try to make it */
	d = dirname(d);
	if (mkdirat(tee_fs_fd, d, 0700))
		return errno_to_teec(errno);

	/* Try to make directory for file again */
	d = dirname(rel_dir);
	if (mkdirat(tee_fs_fd, d, 0700)) {
		int err = errno;

		d = dirname(d);
		unlinkat(tee_fs_fd, d, AT_REMOVEDIR);
		return errno_to_teec(err);
	}

	fd = openat_wrapper(rel_filename, flags);
	if (fd < 0) {
		int err = errno;

		unlinkat(tee_fs_fd, d, AT_REMOVEDIR);
		d = dirname(d);
		unlinkat(tee_fs_fd, d, AT_REMOVEDIR);
		return errno_to_teec(err);
	}

out:
	fs_fsync();
	params[2].a = fd;
	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_close(size_t num_params,
				    struct tee_ioctl_param *params)
{
	int fd = 0;

	if (num_params != 1 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = params[0].b;
	while (close(fd)) {
		if (errno != EINTR)
			return errno_to_teec(errno);
	}
	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_read(size_t num_params,
				   struct tee_ioctl_param *params)
{
	uint8_t *buf = NULL;
	size_t len = 0;
	off_t offs = 0;
	int fd = 0;
	ssize_t r = 0;
	size_t s = 0;

	if (num_params != 2 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = params[0].b;
	offs = params[0].c;

	buf = tee_supp_param_to_va(params + 1);
	if (!buf)
		return TEEC_ERROR_BAD_PARAMETERS;
	len = MEMREF_SIZE(params + 1);

	s = 0;
	r = -1;
	while (r && len) {
		r = pread(fd, buf, len, offs);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return errno_to_teec(errno);
		}
		assert((size_t)r <= len);
		buf += r;
		len -= r;
		offs += r;
		s += r;
	}

	MEMREF_SIZE(params + 1) = s;
	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_write(size_t num_params,
				    struct tee_ioctl_param *params)
{
	uint8_t *buf = NULL;
	size_t len = 0;
	off_t offs = 0;
	int fd = 0;
	ssize_t r = 0;

	if (num_params != 2 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = params[0].b;
	offs = params[0].c;

	buf = tee_supp_param_to_va(params + 1);
	if (!buf)
		return TEEC_ERROR_BAD_PARAMETERS;
	len = MEMREF_SIZE(params + 1);

	while (len) {
		r = pwrite(fd, buf, len, offs);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return errno_to_teec(errno);
		}
		assert((size_t)r <= len);
		buf += r;
		len -= r;
		offs += r;
	}

	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_truncate(size_t num_params,
				       struct tee_ioctl_param *params)
{
	size_t len = 0;
	int fd = 0;

	if (num_params != 1 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fd = params[0].b;
	len = params[0].c;

	while (ftruncate(fd, len)) {
		if (errno != EINTR)
			return errno_to_teec(errno);
	}

	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_remove(size_t num_params,
				     struct tee_ioctl_param *params)
{
	char rel_filename[PATH_MAX] = { 0 };
	char *fname = NULL;
	char *d = NULL;

	if (num_params != 2 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fname = tee_supp_param_to_va(params + 1);
	if (!fname)
		return TEEC_ERROR_BAD_PARAMETERS;

	if (!tee_fs_get_relative_filename(fname, rel_filename,
					  sizeof(rel_filename)))
		return TEEC_ERROR_BAD_PARAMETERS;

	if (unlinkat(tee_fs_fd, rel_filename, 0))
		return errno_to_teec(errno);

	/* If a file is removed, maybe the directory can be removed to? */
	d = dirname(rel_filename);
	if (!unlinkat(tee_fs_fd, d, AT_REMOVEDIR)) {
		/*
		 * If the directory was removed, maybe the parent directory
		 * can be removed too?
		 */
		d = dirname(d);
		unlinkat(tee_fs_fd, d, AT_REMOVEDIR);
	}

	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_rename(size_t num_params,
				     struct tee_ioctl_param *params)
{
	char old_rel_filename[PATH_MAX] = { 0 };
	char new_rel_filename[PATH_MAX] = { 0 };
	char *old_fname = NULL;
	char *new_fname = NULL;
	bool overwrite = false;
	int flags = 0;

	if (num_params != 3 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT ||
	    (params[2].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	overwrite = !!params[0].b;

	old_fname = tee_supp_param_to_va(params + 1);
	if (!old_fname)
		return TEEC_ERROR_BAD_PARAMETERS;

	new_fname = tee_supp_param_to_va(params + 2);
	if (!new_fname)
		return TEEC_ERROR_BAD_PARAMETERS;

	if (!tee_fs_get_relative_filename(old_fname, old_rel_filename,
					  sizeof(old_rel_filename)))
		return TEEC_ERROR_BAD_PARAMETERS;

	if (!tee_fs_get_relative_filename(new_fname, new_rel_filename,
					  sizeof(new_rel_filename)))
		return TEEC_ERROR_BAD_PARAMETERS;

	if (!overwrite)
		flags = RENAME_NOREPLACE;

	if (renameat2(tee_fs_fd, old_rel_filename, tee_fs_fd, new_rel_filename, flags)) {
		if (errno == EEXIST)
			return TEEC_ERROR_ACCESS_CONFLICT;
		if (errno == ENOENT)
			return TEEC_ERROR_ITEM_NOT_FOUND;
	}

	fs_fsync();
	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_opendir(size_t num_params,
				      struct tee_ioctl_param *params)
{
	char rel_filename[PATH_MAX] = { 0 };
	char *fname = NULL;
	int dir_fd = -1;
	DIR *dir = NULL;
	int handle = 0;
	struct dirent *dent = NULL;
	bool empty = true;

	if (num_params != 3 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT ||
	    (params[2].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	fname = tee_supp_param_to_va(params + 1);
	if (!fname)
		return TEEC_ERROR_BAD_PARAMETERS;

	if (!tee_fs_get_relative_filename(fname, rel_filename,
					  sizeof(rel_filename)))
		return TEEC_ERROR_BAD_PARAMETERS;

	dir_fd = openat_wrapper(rel_filename, O_DIRECTORY);
	if (dir_fd < 0)
		return TEEC_ERROR_ITEM_NOT_FOUND;
	dir = fdopendir(dir_fd);
	if (!dir)
		return TEEC_ERROR_ITEM_NOT_FOUND;

	/*
	 * Ignore empty directories. Works around an issue when the
	 * data path is mounted over NFS. Due to the way OP-TEE implements
	 * TEE_CloseAndDeletePersistentObject1() currently, tee-supplicant
	 * still has a file descriptor open to the file when it's removed in
	 * ree_fs_new_remove(). In this case the NFS server may create a
	 * temporary reference called .nfs????, and the rmdir() call fails
	 * so that the TA directory is left over. Checking this special case
	 * here avoids that TEE_StartPersistentObjectEnumerator() returns
	 * TEE_SUCCESS when it should return TEEC_ERROR_ITEM_NOT_FOUND.
	 * Test case: "xtest 6009 6010".
	 */
	while ((dent = readdir(dir))) {
		if (dent->d_name[0] == '.')
			continue;
		empty = false;
		break;
	}
	if (empty) {
		closedir(dir);
		return TEEC_ERROR_ITEM_NOT_FOUND;
	}
	rewinddir(dir);

	handle = handle_get(&dir_handle_db, dir);
	if (handle < 0) {
		closedir(dir);
		return TEEC_ERROR_OUT_OF_MEMORY;
	}

	params[2].a = handle;
	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_closedir(size_t num_params,
				       struct tee_ioctl_param *params)
{
	DIR *dir = NULL;

	if (num_params != 1 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT)
		return TEEC_ERROR_BAD_PARAMETERS;

	dir = handle_put(&dir_handle_db, params[0].b);
	if (!dir)
		return TEEC_ERROR_BAD_PARAMETERS;

	closedir(dir);

	return TEEC_SUCCESS;
}

static TEEC_Result ree_fs_new_readdir(size_t num_params,
				      struct tee_ioctl_param *params)
{
	DIR *dir = NULL;
	struct dirent *dirent = NULL;
	char *buf = NULL;
	size_t len = 0;
	size_t fname_len = 0;

	if (num_params != 2 ||
	    (params[0].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT ||
	    (params[1].attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) !=
			TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT)
		return TEEC_ERROR_BAD_PARAMETERS;


	buf = tee_supp_param_to_va(params + 1);
	if (!buf)
		return TEEC_ERROR_BAD_PARAMETERS;
	len = MEMREF_SIZE(params + 1);

	dir = handle_lookup(&dir_handle_db, params[0].b);
	if (!dir)
		return TEEC_ERROR_BAD_PARAMETERS;

	while (true) {
		dirent = readdir(dir);
		if (!dirent)
			return TEEC_ERROR_ITEM_NOT_FOUND;
		if (dirent->d_name[0] != '.')
			break;
	}

	fname_len = strlen(dirent->d_name) + 1;
	MEMREF_SIZE(params + 1) = fname_len;
	if (fname_len > len)
		return TEEC_ERROR_SHORT_BUFFER;

	memcpy(buf, dirent->d_name, fname_len);

	return TEEC_SUCCESS;
}

TEEC_Result tee_supp_fs_process(size_t num_params,
				struct tee_ioctl_param *params)
{
	if (!num_params || !tee_supp_param_is_value(params))
		return TEEC_ERROR_BAD_PARAMETERS;

	if (tee_fs_fd == -1) {
		if (tee_supp_fs_init() != 0) {
			EMSG("error tee_supp_fs_init: failed to create %s/",
				supplicant_params.fs_parent_path);
			tee_fs_fd = -1;
			return TEEC_ERROR_STORAGE_NOT_AVAILABLE;
		}
	}

	switch (params->a) {
	case OPTEE_MRF_OPEN:
		return ree_fs_new_open(num_params, params);
	case OPTEE_MRF_CREATE:
		return ree_fs_new_create(num_params, params);
	case OPTEE_MRF_CLOSE:
		return ree_fs_new_close(num_params, params);
	case OPTEE_MRF_READ:
		return ree_fs_new_read(num_params, params);
	case OPTEE_MRF_WRITE:
		return ree_fs_new_write(num_params, params);
	case OPTEE_MRF_TRUNCATE:
		return ree_fs_new_truncate(num_params, params);
	case OPTEE_MRF_REMOVE:
		return ree_fs_new_remove(num_params, params);
	case OPTEE_MRF_RENAME:
		return ree_fs_new_rename(num_params, params);
	case OPTEE_MRF_OPENDIR:
		return ree_fs_new_opendir(num_params, params);
	case OPTEE_MRF_CLOSEDIR:
		return ree_fs_new_closedir(num_params, params);
	case OPTEE_MRF_READDIR:
		return ree_fs_new_readdir(num_params, params);
	default:
		return TEEC_ERROR_BAD_PARAMETERS;
	}
}
