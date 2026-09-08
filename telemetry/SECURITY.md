# Telemetry Security and Privacy Contract

MFT v1 is allowlist-only and has no generic string record or string table.

The trace must NEVER contain access tokens, authenticated URLs, query strings, HTTP headers/bodies, usernames, passwords, server URLs/hostnames, server IDs, user IDs, device IDs, Jellyfin item IDs, media source IDs, series/season/episode IDs, titles, image tags, cache paths, or download scopes. Do not hash persistent identifiers as a workaround.

Allowed: fixed enums, booleans, counts, durations, byte counts, queue depths, numeric HTTP/CURL codes, build metadata, and ephemeral per-process sequence IDs/nonces.

Layer rules: worker/pipeline code sets enum context and gauges only; ImageCache owns cache aggregates; ImageDecoder owns decode metrics; JellyfinApi owns RequestKind; RouteRequest owns route context; HttpClient owns normal transport metrics; HLS owns direct segment metrics.

Task 034 must create fake secrets including:

```text
SECRET_ACCESS_TOKEN_123
https://private.example/Items/SECRET_ITEM?api_key=SECRET_ACCESS_TOKEN_123
SECRET_USERNAME_456
SECRET_PASSWORD_789
SECRET_SERVER_ID_ABC
SECRET_USER_ID_DEF
SECRET_DEVICE_ID_GHI
SECRET_ITEM_ID_JKL
SECRET_MEDIA_SOURCE_MNO
SECRET_SERIES_ID_PQR
SECRET_SEASON_ID_STU
SECRET_EPISODE_ID_VWX
SECRET_TITLE_YZA
SECRET_IMAGE_TAG_BCD
SECRET_CACHE_PATH_EFG
SECRET_DOWNLOAD_SCOPE_HIJ
```

It must generate a real `.mft` and prove all full strings plus `SECRET_`, `private.example`, and `api_key=` are absent from raw bytes.
