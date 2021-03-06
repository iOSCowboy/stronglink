// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#include "converter.h"

TYPE_LIST(plaintext,
	"text/plain; charset=utf-8",
	"text/plain",
	SLN_META_TYPE,
	"text/x-sln-meta+json; charset=utf-8", // TODO: Get rid of these obsolete types.
	"text/efs-meta+json; charset=utf-8")
CONVERTER(plaintext) {
	if(size > LIMIT_DEFAULT) return UV_EFBIG;

	yajl_gen_string(json, (unsigned char const *)STR_LEN("link"));
	yajl_gen_map_open(json);

	regex_t linkify[1];
	int rc = regcomp(linkify, LINKIFY_RE, REG_ICASE | REG_EXTENDED);
	if(0 != rc) return UV_ENOMEM;

	rc = write_html(html, STR_LEN("<pre>"));
	if(rc < 0) goto cleanup;

	char const *pos = buf;
	regmatch_t match;
	while(0 == regexec(linkify, pos, 1, &match, 0)) {
		regoff_t const loc = match.rm_so;
		regoff_t const len = match.rm_eo - match.rm_so;

		rc = write_text(html, pos, loc);
		if(rc < 0) goto cleanup;
		rc = write_link(html, pos+loc, len);
		if(rc < 0) goto cleanup;

		yajl_gen_string(json, (unsigned char const *)pos+loc, len);
		yajl_gen_map_open(json);
		yajl_gen_map_close(json);

		pos += loc+len;
	}
	rc = write_text(html, pos, size-(pos-buf));
	if(rc < 0) goto cleanup;
	rc = write_html(html, STR_LEN("</pre>"));
	if(rc < 0) goto cleanup;

	yajl_gen_map_close(json);

	yajl_gen_string(json, (unsigned char const *)STR_LEN("fulltext"));
	yajl_gen_string(json, (unsigned char const *)buf, size);

cleanup:
	regfree(linkify);
	return rc;
}

