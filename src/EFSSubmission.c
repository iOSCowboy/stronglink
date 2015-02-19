#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <regex.h>
#include <yajl/yajl_gen.h>
#include "async/async.h"
#include "EarthFS.h"

#define FTS_MAX (1024 * 50)

struct EFSSubmission {
	EFSSessionRef session;
	str_t *type;

	str_t *tmppath;
	uv_file tmpfile;
	uint64_t size;

	EFSHasherRef hasher;
	EFSMetaFileRef meta;

	str_t **URIs;
	str_t *internalHash;
};

EFSSubmissionRef EFSSubmissionCreate(EFSSessionRef const session, strarg_t const type) {
	if(!session) return NULL;
	assertf(type, "Submission requires type");
	// TODO: Check session permissions?

	EFSSubmissionRef sub = calloc(1, sizeof(struct EFSSubmission));
	if(!sub) return NULL;
	sub->session = session;
	sub->type = strdup(type);

	sub->tmppath = EFSRepoCopyTempPath(EFSSessionGetRepo(session));
	sub->tmpfile = async_fs_open(sub->tmppath, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0400);
	if(sub->tmpfile < 0) {
		if(UV_ENOENT == sub->tmpfile) {
			async_fs_mkdirp_dirname(sub->tmppath, 0700);
			sub->tmpfile = async_fs_open(sub->tmppath, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0400);
		}
		if(sub->tmpfile < 0) {
			fprintf(stderr, "Error: couldn't create temp file %s (%s)\n", sub->tmppath, uv_err_name(sub->tmpfile));
			EFSSubmissionFree(&sub);
			return NULL;
		}
	}

	sub->hasher = EFSHasherCreate(sub->type);
	sub->meta = EFSMetaFileCreate(sub->type);

	return sub;
}
void EFSSubmissionFree(EFSSubmissionRef *const subptr) {
	EFSSubmissionRef sub = *subptr;
	if(!sub) return;

	sub->session = NULL;
	FREE(&sub->type);

	if(sub->tmppath) async_fs_unlink(sub->tmppath);
	FREE(&sub->tmppath);
	assert(sub->tmpfile < 0); sub->tmpfile = 0;
	sub->size = 0;

	EFSHasherFree(&sub->hasher);
	EFSMetaFileFree(&sub->meta);

	if(sub->URIs) for(index_t i = 0; sub->URIs[i]; ++i) FREE(&sub->URIs[i]);
	FREE(&sub->URIs);
	FREE(&sub->internalHash);

	assert_zeroed(sub, 1);
	FREE(subptr); sub = NULL;
}

EFSRepoRef EFSSubmissionGetRepo(EFSSubmissionRef const sub) {
	if(!sub) return NULL;
	return EFSSessionGetRepo(sub->session);
}

int EFSSubmissionWrite(EFSSubmissionRef const sub, byte_t const *const buf, size_t const len) {
	if(!sub) return 0;
	if(sub->tmpfile < 0) return -1;

	uv_buf_t info = uv_buf_init((char *)buf, len);
	ssize_t const result = async_fs_write(sub->tmpfile, &info, 1, sub->size);
	if(result < 0) {
		fprintf(stderr, "EFSSubmission write error %ld\n", (long)result);
		return -1;
	}

	sub->size += len;
	EFSHasherWrite(sub->hasher, buf, len);
	EFSMetaFileWrite(sub->meta, buf, len);
	return 0;
}
int EFSSubmissionEnd(EFSSubmissionRef const sub) {
	if(!sub) return 0;
	if(sub->tmpfile < 0) return -1;
	sub->URIs = EFSHasherEnd(sub->hasher);
	sub->internalHash = strdup(EFSHasherGetInternalHash(sub->hasher));
	EFSHasherFree(&sub->hasher);

	EFSMetaFileEnd(sub->meta);

	if(async_fs_fsync(sub->tmpfile) < 0) return -1;
	int err = async_fs_close(sub->tmpfile);
	sub->tmpfile = -1;
	if(err < 0) return -1;
	return 0;
}
int EFSSubmissionWriteFrom(EFSSubmissionRef const sub, ssize_t (*read)(void *, byte_t const **), void *const context) {
	if(!sub) return 0;
	assertf(read, "Read function required");
	for(;;) {
		byte_t const *buf = NULL;
		ssize_t const len = read(context, &buf);
		if(0 == len) break;
		if(len < 0) return -1;
		if(EFSSubmissionWrite(sub, buf, len) < 0) return -1;
	}
	if(EFSSubmissionEnd(sub) < 0) return -1;
	return 0;
}

strarg_t EFSSubmissionGetPrimaryURI(EFSSubmissionRef const sub) {
	if(!sub) return NULL;
	if(!sub->URIs) return NULL;
	return sub->URIs[0];
}

int EFSSubmissionAddFile(EFSSubmissionRef const sub) {
	if(!sub) return -1;
	if(!sub->tmppath) return -1;
	if(!sub->size) return -1;
	EFSRepoRef const repo = EFSSubmissionGetRepo(sub);
	str_t *internalPath = EFSRepoCopyInternalPath(repo, sub->internalHash);
	int result = 0;
	result = async_fs_link(sub->tmppath, internalPath);
	if(result < 0 && UV_EEXIST != result) {
		if(UV_ENOENT == result) {
			async_fs_mkdirp_dirname(internalPath, 0700);
			result = async_fs_link(sub->tmppath, internalPath);
		}
		if(result < 0 && UV_EEXIST != result) {
			fprintf(stderr, "Couldn't move %s to %s (%s)\n", sub->tmppath, internalPath, uv_err_name(result));
			FREE(&internalPath);
			return -1;
		}
	}
	FREE(&internalPath);
	async_fs_unlink(sub->tmppath);
	FREE(&sub->tmppath);
	return 0;
}
int EFSSubmissionStore(EFSSubmissionRef const sub, DB_txn *const txn) {
	if(!sub) return -1;
	assert(txn);
	if(sub->tmppath) return -1;
	EFSSessionRef const session = sub->session;
	EFSRepoRef const repo = EFSSubmissionGetRepo(sub);
	int64_t const userID = EFSSessionGetUserID(session);

	int64_t fileID = db_next_id(txn, EFSFileByID);
	int rc;

	DB_VAL(dupFileID_val, DB_VARINT_MAX);
	db_bind_uint64(dupFileID_val, fileID);

	DB_VAL(fileInfo_key, DB_VARINT_MAX * 1 + DB_INLINE_MAX * 2);
	db_bind_uint64(fileInfo_key, EFSFileIDByInfo);
	db_bind_string(txn, fileInfo_key, sub->internalHash);
	db_bind_string(txn, fileInfo_key, sub->type);
	rc = db_put(txn, fileInfo_key, dupFileID_val, DB_NOOVERWRITE);
	if(DB_SUCCESS == rc) {
		DB_VAL(fileID_key, DB_VARINT_MAX + DB_VARINT_MAX);
		db_bind_uint64(fileID_key, EFSFileByID);
		db_bind_uint64(fileID_key, fileID);
		DB_VAL(file_val, DB_VARINT_MAX * 1 + DB_INLINE_MAX * 2);
		db_bind_string(txn, file_val, sub->internalHash);
		db_bind_string(txn, file_val, sub->type);
		db_bind_uint64(file_val, sub->size);
		rc = db_put(txn, fileID_key, file_val, DB_NOOVERWRITE_FAST);
		if(DB_SUCCESS != rc) return -1;
	} else if(DB_KEYEXIST == rc) {
		fileID = db_read_uint64(dupFileID_val);
	} else return -1;

	for(index_t i = 0; sub->URIs[i]; ++i) {
		strarg_t const URI = sub->URIs[i];
		DB_val null = { 0, NULL };

		DB_VAL(fwd, DB_VARINT_MAX * 2 + DB_INLINE_MAX * 1);
		db_bind_uint64(fwd, EFSFileIDAndURI);
		db_bind_uint64(fwd, fileID);
		db_bind_string(txn, fwd, URI);
		rc = db_put(txn, fwd, &null, DB_NOOVERWRITE_FAST);
		assert(DB_SUCCESS == rc || DB_KEYEXIST == rc);

		DB_VAL(rev, DB_VARINT_MAX * 2 + DB_INLINE_MAX * 1);
		db_bind_uint64(rev, EFSURIAndFileID);
		db_bind_string(txn, rev, URI);
		db_bind_uint64(rev, fileID);
		rc = db_put(txn, rev, &null, DB_NOOVERWRITE_FAST);
		assert(DB_SUCCESS == rc || DB_KEYEXIST == rc);
	}

	// TODO: Store fileIDByType


	// TODO: Add permissions for other specified users too.
/*	sqlite3_stmt *insertFilePermission = QUERY(db,
		"INSERT OR IGNORE INTO file_permissions\n"
		"	(file_id, user_id, meta_file_id)\n"
		"VALUES (?, ?, ?)");
	sqlite3_bind_int64(insertFilePermission, 1, fileID);
	sqlite3_bind_int64(insertFilePermission, 2, userID);
	sqlite3_bind_int64(insertFilePermission, 3, fileID);
	EXEC(insertFilePermission); insertFilePermission = NULL;*/


	strarg_t const primaryURI = EFSSubmissionGetPrimaryURI(sub);
	if(EFSMetaFileStore(sub->meta, fileID, primaryURI, txn) < 0) {
		fprintf(stderr, "EFSMetaFileStore error\n");
		return -1;
	}

	return 0;
}

EFSSubmissionRef EFSSubmissionCreateQuick(EFSSessionRef const session, strarg_t const type, ssize_t (*read)(void *, byte_t const **), void *const context) {
	EFSSubmissionRef sub = EFSSubmissionCreate(session, type);
	if(!sub) return NULL;
	int err = 0;
	err = err < 0 ? err : EFSSubmissionWriteFrom(sub, read, context);
	err = err < 0 ? err : EFSSubmissionAddFile(sub);
	if(err < 0) EFSSubmissionFree(&sub);
	return sub;
}
int EFSSubmissionCreateQuickPair(EFSSessionRef const session, strarg_t const type, ssize_t (*read)(void *, byte_t const **), void *const context, strarg_t const title, EFSSubmissionRef *const outSub, EFSSubmissionRef *const outMeta) {
	EFSSubmissionRef sub = EFSSubmissionCreate(session, type);
	EFSSubmissionRef meta = EFSSubmissionCreate(session, "text/efs-meta+json; charset=utf-8");
	if(!sub || !meta) {
		EFSSubmissionFree(&sub);
		EFSSubmissionFree(&meta);
		return -1;
	}


	str_t *fulltext = NULL;
	size_t fulltextlen = 0;
	if(
		0 == strcasecmp(type, "text/markdown; charset=utf-8") ||
		0 == strcasecmp(type, "text/plain; charset=utf-8")
	) {
		fulltext = malloc(FTS_MAX + 1);
		// TODO
	}
	for(;;) {
		byte_t const *buf = NULL;
		ssize_t const len = read(context, &buf);
		if(0 == len) break;
		if(len < 0 || EFSSubmissionWrite(sub, buf, len) < 0) {
			FREE(&fulltext);
			EFSSubmissionFree(&sub);
			EFSSubmissionFree(&meta);
			return -1;
		}
		if(fulltext) {
			size_t const use = MIN(FTS_MAX-fulltextlen, (size_t)len);
			memcpy(fulltext+fulltextlen, buf, use);
			fulltextlen += use;
		}
	}
	if(fulltext) {
		fulltext[fulltextlen] = '\0';
	}


	if(EFSSubmissionEnd(sub) < 0 || EFSSubmissionAddFile(sub) < 0) {
		FREE(&fulltext);
		EFSSubmissionFree(&sub);
		EFSSubmissionFree(&meta);
		return -1;
	}

	strarg_t const targetURI = EFSSubmissionGetPrimaryURI(sub);
	EFSSubmissionWrite(meta, (byte_t const *)targetURI, strlen(targetURI));
	EFSSubmissionWrite(meta, (byte_t const *)"\r\n\r\n", 4);

	yajl_gen json = yajl_gen_alloc(NULL);
	yajl_gen_config(json, yajl_gen_print_callback, (void (*)())EFSSubmissionWrite, meta);
	yajl_gen_config(json, yajl_gen_beautify, (int)true);

	yajl_gen_map_open(json);

	if(title) {
		yajl_gen_string(json, (byte_t const *)"title", strlen("title"));
		yajl_gen_array_open(json);
		yajl_gen_string(json, (byte_t const *)title, strlen(title));
		if(fulltextlen) {
			// TODO: Try to determine title from content
		}
		yajl_gen_array_close(json);
	}

	if(fulltextlen) {
		yajl_gen_string(json, (byte_t const *)"fulltext", strlen("fulltext"));
		yajl_gen_string(json, (byte_t const *)fulltext, fulltextlen);


		yajl_gen_string(json, (byte_t const *)"link", strlen("link"));
		yajl_gen_array_open(json);

		regex_t linkify[1];
		// <http://daringfireball.net/2010/07/improved_regex_for_matching_urls>
		// Painstakingly ported to POSIX
		int rc = regcomp(linkify, "([a-z][a-z0-9_-]+:(/{1,3}|[a-z0-9%])|www[0-9]{0,3}[.]|[a-z0-9.-]+[.][a-z]{2,4}/)([^[:space:]()<>]+|\\(([^[:space:]()<>]+|(\\([^[:space:]()<>]+\\)))*\\))+(\\(([^[:space:]()<>]+|(\\([^[:space:]()<>]+\\)))*\\)|[^][[:space:]`!(){};:'\".,<>?«»“”‘’])", REG_ICASE | REG_EXTENDED);
		assert(0 == rc);

		strarg_t pos = fulltext;
		regmatch_t match;
		while(0 == regexec(linkify, pos, 1, &match, 0)) {
			regoff_t const loc = match.rm_so;
			regoff_t const len = match.rm_eo - match.rm_so;
			yajl_gen_string(json, (byte_t const *)pos+loc, len);
			pos += loc+len;
		}

		regfree(linkify);

		yajl_gen_array_close(json);
	}

	yajl_gen_map_close(json);
	yajl_gen_free(json); json = NULL;
	FREE(&fulltext);

	if(
		EFSSubmissionEnd(meta) < 0 ||
		EFSSubmissionAddFile(meta) < 0
	) {
		EFSSubmissionFree(&sub);
		EFSSubmissionFree(&meta);
		return -1;
	}

	*outSub = sub;
	*outMeta = meta;
	return 0;
}
int EFSSubmissionBatchStore(EFSSubmissionRef const *const list, count_t const count) {
	if(!count) return 0;
	EFSRepoRef const repo = EFSSessionGetRepo(list[0]->session);
	DB_env *db = NULL;
	int rc = EFSRepoDBOpen(repo, &db);
	if(rc < 0) {
		return -1;
	}
	DB_txn *txn = NULL;
	rc = db_txn_begin(db, NULL, DB_RDWR, &txn);
	if(DB_SUCCESS != rc) {
		EFSRepoDBClose(repo, &db);
		return -1;
	}
	int err = 0;
	uint64_t sortID = 0;
	for(index_t i = 0; i < count; ++i) {
		assert(list[i]);
		assert(repo == EFSSessionGetRepo(list[i]->session));
		err = EFSSubmissionStore(list[i], txn);
		if(err < 0) break;
		uint64_t const metaFileID = EFSMetaFileGetID(list[i]->meta);
		if(metaFileID > sortID) sortID = metaFileID;
	}
	if(err < 0) {
		db_txn_abort(txn); txn = NULL;
	} else {
		rc = db_txn_commit(txn); txn = NULL;
		if(DB_SUCCESS != rc) err = -1;
	}
	EFSRepoDBClose(repo, &db);
	if(err >= 0) EFSRepoSubmissionEmit(repo, sortID);
	return err;
}

