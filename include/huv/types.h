#ifndef HUV_TYPES_H
#define HUV_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct huv_server huv_server_t;
typedef struct huv_request huv_request_t;
typedef struct huv_response huv_response_t;

typedef void (*huv_next_fn)(huv_request_t *req, huv_response_t *res);
typedef void (*huv_handler_fn)(huv_request_t *req, huv_response_t *res,
                               huv_next_fn next);

typedef enum
{
    HUV_LOG_DEBUG = 0,
    HUV_LOG_INFO = 1,
    HUV_LOG_WARN = 2,
    HUV_LOG_ERROR = 3
} huv_log_level_t;

typedef void (*huv_log_fn)(void *user, huv_log_level_t level, const char *msg);

void huv_log_stderr(void *user, huv_log_level_t level, const char *msg);

#ifdef __cplusplus
}
#endif

#endif
