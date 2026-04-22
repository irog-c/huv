#ifndef HUV_REQUEST_H
#define HUV_REQUEST_H

#include "huv/types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *huv_request_method(const huv_request_t *req);
const char *huv_request_path(const huv_request_t *req);
const char *huv_request_query(const huv_request_t *req);
const char *huv_request_body(const huv_request_t *req, size_t *len);

/* Look up a path parameter captured by a parameterized route. Returns a
 * pointer to the NUL-terminated value, or NULL if no parameter by that name
 * was captured. The returned pointer is valid for the lifetime of the
 * request. */
const char *huv_request_param(const huv_request_t *req, const char *name);

/* Look up a query string parameter by name (case-sensitive). Returns a
 * pointer to the NUL-terminated, URL-decoded value (%XX and '+' both
 * decoded), or NULL if the key is not present. Keys with no '=' decode
 * to "". If the key appears multiple times, the first is returned. The
 * returned pointer is valid for the lifetime of the request. */
const char *huv_request_query_param(const huv_request_t *req,
                                     const char *name);

/* Look up a request header by name (case-insensitive). Returns a pointer to
 * the NUL-terminated value, or NULL if the header is not present. If a
 * header appears multiple times, the first occurrence is returned. The
 * returned pointer is valid for the lifetime of the request. */
const char *huv_request_header(const huv_request_t *req, const char *name);

/* Iterate all request headers. Returns the number of headers. Use with
 * huv_request_header_at() to walk them. */
size_t huv_request_header_count(const huv_request_t *req);
void huv_request_header_at(const huv_request_t *req, size_t index,
                            const char **name, const char **value);

#ifdef __cplusplus
}
#endif

#endif
