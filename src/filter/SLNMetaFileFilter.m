// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#include "SLNFilter.h"

@implementation SLNMetaFileFilter
- (void)free {
	db_cursor_close(metafiles); metafiles = NULL;
	[super free];
}

- (SLNFilterType)type {
	return SLNMetaFileFilterType;
}
- (SLNFilter *)unwrap {
	return self;
}
- (void)print:(size_t const)depth {
	indent(depth);
	fprintf(stderr, "(meta)\n");
}
- (size_t)getUserFilter:(str_t *const)data :(size_t const)size :(size_t const)depth {
	assert(0);
	return wr(data, size, "");
}

- (int)prepare:(DB_txn *const)txn {
	int rc = [super prepare:txn];
	if(rc < 0) return rc;
	db_cursor_renew(txn, &metafiles); // SLNMetaFileByID
	return 0;
}

- (void)seek:(int const)dir :(uint64_t const)sortID :(uint64_t const)fileID {
	DB_range range[1];
	DB_val key[1], val[1];
	SLNMetaFileByIDRange0(range, NULL);
	SLNMetaFileByIDKeyPack(key, NULL, sortID);
	int rc = db_cursor_seekr(metafiles, range, key, val, dir);
	if(DB_NOTFOUND == rc) return;
	db_assertf(rc >= 0, "Database error %s", sln_strerror(rc));

	uint64_t actualSortID, actualFileID;
	SLNMetaFileByIDKeyUnpack(key, NULL, &actualSortID);
	//SLNMetaFileByIDValUnpack(val, NULL, &actualFileID, &ignore);
	actualFileID = db_read_uint64(val);
	if(sortID != actualSortID) return;
	if(dir > 0 && actualFileID >= fileID) return;
	if(dir < 0 && actualFileID <= fileID) return;
	[self step:dir];
}
- (void)current:(int const)dir :(uint64_t *const)sortID :(uint64_t *const)fileID {
	DB_val key[1], val[1];
	int rc = db_cursor_current(metafiles, key, val);
	if(rc >= 0) {
		uint64_t s;
		SLNMetaFileByIDKeyUnpack(key, NULL, &s);
		uint64_t f;
		strarg_t ignore;
		f = db_read_uint64(val);
		//SLNMetaFileByIDValUnpack(val, NULL, &f, &ignore);
		if(sortID) *sortID = s;
		if(fileID) *fileID = f;
	} else {
		if(sortID) *sortID = invalid(dir);
		if(fileID) *fileID = invalid(dir);
	}
}
- (void)step:(int const)dir {
	DB_range range[1];
	SLNMetaFileByIDRange0(range, NULL);
	int rc = db_cursor_nextr(metafiles, range, NULL, NULL, dir);
	db_assertf(rc >= 0 || DB_NOTFOUND == rc, "Database error %s", sln_strerror(rc));
}
- (SLNAgeRange)fullAge:(uint64_t const)fileID {
	// TODO: Check that fileID is a meta-file.
	return (SLNAgeRange){fileID, UINT64_MAX};
}
- (uint64_t)fastAge:(uint64_t const)fileID :(uint64_t const)sortID {
	return [self fullAge:fileID].min;
}
@end

