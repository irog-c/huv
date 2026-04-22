# Configuration

All server options live on `huv_server_config_t`
(`include/http/server.h`). Start from `HUV_SERVER_CONFIG_DEFAULT` and
override the fields you care about — the defaults are picked to be safe for
small services, not maxed out for benchmarks.

```c
huv_server_config_t cfg = HUV_SERVER_CONFIG_DEFAULT;
cfg.port = 8080;
cfg.log_cb = huv_log_stderr;

huv_server_t *app = huv_server_new(&cfg);
```

## Listeners

| Field           | Default      | Meaning                                                                                        |
| --------------- | ------------ | ---------------------------------------------------------------------------------------------- |
| `port`          | `8080`       | Plain-HTTP listen port. Set `0` to disable the plain listener (HTTPS-only).                    |
| `bind_addr`     | `"0.0.0.0"`  | IPv4 bind address. Literal form only (e.g., `"127.0.0.1"`).                                    |
| `tls_port`      | `8443`       | HTTPS listen port. Only used if both `tls_cert_path` and `tls_key_path` are set.               |
| `tls_cert_path` | `NULL`       | PEM cert file. Setting this + `tls_key_path` enables the TLS listener.                         |
| `tls_key_path`  | `NULL`       | PEM private key file.                                                                          |

If `port == 0` and TLS is not configured, `huv_server_run` returns `-1` —
you'd have no listeners.

## Timeouts

| Field                  | Default (ms) | Meaning                                                                                                  |
| ---------------------- | -----------: | -------------------------------------------------------------------------------------------------------- |
| `idle_timeout_ms`      | `30000`      | Max time a keep-alive conn sits between requests (`PHASE_IDLE`). Hits → close.                           |
| `request_timeout_ms`   | `30000`      | Max time to finish reading one request (`PHASE_PARSING_REQUEST`). Mitigates slowloris-style attacks.     |
| `shutdown_timeout_ms`  | `10000`      | On SIGINT/SIGTERM, give in-flight conns this long to drain before force-closing.                         |

## Resource caps

| Field                    | Default       | Meaning                                                                                                                                             |
| ------------------------ | ------------: | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `max_connections`        | `1024`        | New accepts beyond this are dropped immediately. Protects the loop from FD exhaustion.                                                              |
| `max_body_bytes`         | `1 MiB`       | Request bodies larger than this return `413 Payload Too Large`. `0` is treated as 1 MiB (sentinel — use a large positive value if you want huge).   |
| `max_write_queue_bytes`  | `16 MiB`      | If a conn's outbound queue would exceed this, the submit fails and the conn closes. Backpressure guard against slow clients holding memory hostage. |

## Multi-worker

| Field              | Default | Meaning                                                                                                  |
| ------------------ | ------: | -------------------------------------------------------------------------------------------------------- |
| `workers`          | `1`     | `0` or `1` → single process. Values `> 1` → `fork()` N children; each runs its own loop. See [workers.md](workers.md). |
| `respawn_workers`  | `true`  | When `workers > 1`, re-fork a worker that exits abnormally. Crash-loop guard retires a slot after 10 respawns in any 60s window. Set to `false` if systemd/k8s manages worker lifetime for you. |

Register routes and middleware **before** calling `huv_server_run`, so all
children inherit the same configuration.

## Logging

| Field       | Default | Meaning                                                                                              |
| ----------- | ------- | ---------------------------------------------------------------------------------------------------- |
| `log_cb`    | `NULL`  | `void (*)(void *user, huv_log_level_t level, const char *msg)`. `NULL` silences library logs.       |
| `log_user`  | `NULL`  | Opaque pointer passed to `log_cb`.                                                                   |

`huv_log_stderr` is provided as a ready-made sink; the examples use it.

## Example: HTTPS-only on port 443

```c
huv_server_config_t cfg = HUV_SERVER_CONFIG_DEFAULT;
cfg.port = 0;                          /* disable plain listener */
cfg.tls_port = 443;
cfg.tls_cert_path = "/etc/certs/server.crt";
cfg.tls_key_path  = "/etc/certs/server.key";
cfg.workers = 4;
```

## Example: tight slowloris protection

```c
cfg.idle_timeout_ms   = 5000;   /* idle conns close fast */
cfg.request_timeout_ms = 3000;  /* drip-feeders get closed */
cfg.max_body_bytes    = 256 * 1024;
```
