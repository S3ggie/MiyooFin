# MFT v1 — Normative Binary Schema

Normative before Task 000. Little-endian. Encode fields explicitly; never raw structs. Common record header 16 bytes. Maximum record size 256 bytes; largest current v1 record 88 bytes. No string record/table.

## File header — 80 bytes

|Off|Size|Type|Field|Meaning|
|---:|---:|---|---|---|
|0|4|bytes|magic|`MFT1`|
|4|2|u16|schema_version|1|
|6|2|u16|header_size|80|
|8|4|u32|flags|bit0 wall valid; bit1 runtime enabled|
|12|4|u32|pid|process-local|
|16|8|u64|session_nonce|ephemeral, never derived from persistent identity|
|24|8|u64|start_monotonic_us|CLOCK_MONOTONIC|
|32|8|u64|optional_wall_time_s|UNIX seconds if valid|
|40|4|u32|sample_interval_ms|default 1000|
|44|4|u32|free_space_interval_ms|default 10000|
|48|4|u32|rotation_index|0-based|
|52|4|u32|writer_buffer_bytes|32768 default|
|56|4|u32|app_version_packed|major<<16\|minor<<8\|patch|
|60|4|u32|build_commit32|build metadata; zero allowed|
|64|4|u32|platform_id|PlatformId|
|68|4|u32|reserved0|0|
|72|8|u64|reserved1|0|

## Common record header — 16 bytes

u16 record_type; u16 record_size including header; u32 sequence; u64 monotonic_us.

## Record types
1 SystemSample; 2 FrameTimingSummary; 3 StateTransition; 4 WorkerSample; 5 NetworkRequest; 6 ArtworkSummary; 7 ArtworkDecode; 8 LibrarySync; 9 DownloadSample; 10 DownloadSegmentAttempt; 11 PlaybackEvent; 12 UiStall; 13 TelemetryHealth; 14 SessionEvent.

## Exact enums

### PlatformId u32
0 Unknown; 1 MiyooMiniPlusOnionOS; 2 Host.

### Outcome u8
0 Unknown; 1 Success; 2 Failure; 3 Cancelled; 4 Dropped; 5 Skipped; 6 Unavailable.

### RouteKind u8
0 Unknown; 1 Lan; 2 Public.

### HttpMethod u8
0 Unknown; 1 Get; 2 Post.

### ScreenId u16
0 None; 1 Startup; 2 ServerEntry; 3 Connect; 4 Login; 5 AuthCheck; 6 Home; 7 Series; 8 EpisodeBrowser; 9 MovieDetails; 10 InputDiagnostics; 11 Other.

### TabId u16
0 NotApplicable; 1 Home; 2 Movies; 3 Shows; 4 Downloads; 5 Settings; 6 Other.

### ActionId u16
0 None; 1 Up; 2 Down; 3 Left; 4 Right; 5 Confirm; 6 Back; 7 Search; 8 ActionsMenu; 9 PrevTab; 10 NextTab; 11 PrevPage; 12 NextPage; 13 Settings; 14 Menu; 15 Exit; 16 Raw; 17 Other.

### FramePhase u8
1 FullFrame; 2 Input; 3 Update; 4 Transition; 5 ScreenRender; 6 FramebufferUpload; 7 Present.

### StateKind u8
1 Screen; 2 Tab; 3 Action; 4 PlaybackState.

**There is no ConnectivityMode in MFT v1.** It is removed because this roadmap has no single authoritative connectivity-transition owner.

### PlaybackState u16
0 Unknown; 1 UiActive; 2 StartingOverlay; 3 ExternalPlayback; 4 Resuming.

### UiPhaseId u8
0 Unknown; 1 IdleFrameBoundary=`idle/frame boundary`; 2 EventInput=`event/input`; 3 Update=`update`; 4 ScreenTransition=`screen transition`; 5 Render=`render`.

### UiScopeId u16
Only allowlisted strings serialize; all others map 0.

0 Unknown; 1 `Screen::handleAction`; 2 `Screen::update`; 3 `App::finishSavedSessionValidation`; 4 `Screen::render`; 5 `ScreenStack::push -> null`; 6 `ScreenStack::push -> MovieDetailsScreen`; 7 `ScreenStack::push -> SeriesScreen`; 8 `ScreenStack::push -> EpisodeBrowserScreen`; 9 `ScreenStack::push -> HomeScreen`; 10 `ScreenStack::push -> Screen`; 11 `ScreenStack::pop`; 12 `ScreenStack::signalLeave`; 13 `ScreenStack::retireScreen`; 14 `ScreenStack::enterPrevious`; 15 `HomeScreen::publishLibraryResult`; 16 `HomeScreen::publishResumeResult`; 17 `HomeScreen::updateArtworkWorkingSet`; 18 `HomeScreen::publishDecodedArtwork`; 19 `HomeScreen::queueSelectedArtwork`; 20 `HomeScreen::queueVisibleArtwork`; 21 `HomeScreen::publishDownloadSnapshot`; 22 `EpisodeBrowserScreen::enter`; 23 `EpisodeBrowserScreen::workerShutdown`; 24 `EpisodeBrowserScreen::publishEpisodes`; 25 `EpisodeBrowserScreen::publishArtworkResult`; 26 `MovieDetailsScreen::enter`; 27 `MovieDetailsScreen::worker cancellation`; 28 `MovieDetailsScreen::owned worker creation`; 29 `MovieDetailsScreen::publish async preparation`; 30 `MovieDetailsScreen::publish image surface preparation`; 31 `MovieDetailsScreen::publish playback/download state`; 32 `DownloadManager::planSnapshot mutex wait`.

### WorkerId u16
1 HomeLibraryFetch; 2 HomeHierarchy; 3 HomePoster; 4 HomeDecode; 5 HomeResumeRefresh; 6 HomeDownloadRefresh; 7 EpisodeFetch; 8 EpisodeArtwork; 9 DownloadTransfer; 10 DownloadPlanner; 11 DownloadReconcile; 12 ScreenRetirement; 13 SavedSessionValidation; 14 PlaybackJournalSync.

### worker_mask mapping — u16
bit0 HomeLibraryFetch; bit1 HomeHierarchy; bit2 HomePoster; bit3 HomeDecode; bit4 HomeResumeRefresh; bit5 HomeDownloadRefresh; bit6 EpisodeFetch; bit7 EpisodeArtwork; bit8 DownloadTransfer; bit9 DownloadPlanner; bit10 DownloadReconcile; bit11 ScreenRetirement; bit12 SavedSessionValidation; bit13 PlaybackJournalSync; bits14–15 zero. Bit=1 iff facade active gauge is true at stall snapshot.

### RequestKind u16
0 Unknown; 1 SystemInfo; 2 Authentication; 3 TokenValidation; 4 Views; 5 LibraryItems; 6 ResumeItems; 7 LatestItems; 8 ChangedHierarchy; 9 Seasons; 10 Episodes; 11 PlaybackPosition; 12 PlaybackStopped; 13 DownloadPlaybackInfo; 14 HlsMaster; 15 HlsVariant; 16 Artwork.

### ArtworkContext u8
0 Unknown; 1 HomeSelected; 2 HomeGrid; 3 HomeShows; 4 HomePoster; 5 EpisodeSelected; 6 EpisodePrefetch; 7 MovieDetails; 8 CacheGeneric.

### PlaybackStage u8
1 RequestToFinalPresent; 2 SuspendPlatform; 3 ChildWait; 4 ResumePlatform; 5 ReturnToFirstNormalFrame.

### PlaybackSourceKind u8
0 Unknown; 1 Jellyfin; 2 Local. If determining source would require reading sensitive request content, use Unknown.

### StallEdge u8
1 Begin; 2 End; 3 SlowScope.

### SessionEventKind u8
1 TelemetryStarted; 2 TelemetryStopped; 3 SamplingSuspended; 4 SamplingResumed; 5 WriterDisabledLowSpace; 6 WriterError.

### WriterErrorKind u32
0 Unknown; 1 Open; 2 Write; 3 Flush; 4 Rotate; 5 Close.

### SamplingReason u32
0 Unknown; 1 ExternalPlayback.

## Records

### 1 SystemSample — payload 72, total 88
u64 process_cpu_us_cumulative; u64 rss_kib; u64 peak_rss_kib; u64 process_read_bytes_cumulative; u64 process_write_bytes_cumulative; u64 free_storage_bytes; u64 telemetry_logical_bytes_written; u32 dropped_records_cumulative; u32 free_storage_sample_age_ms; u32 validity_flags; u32 reserved=0.
Validity bits: 0 CPU,1 RSS,2 peak,3 read,4 write,5 free.

### 2 FrameTimingSummary — payload 72, total 88
u8 phase; u8 reserved[3]; u32 interval_us; u32 sample_count; u64 total_us; u32 max_us; u32 over_50ms_count; u32 over_100ms_count; u32 histogram[9]; u32 reserved.
Bins: <=8333, <=16667, <=25000, <=33333, <=50000, <=100000, <=250000, <=500000, >500000 us.

### 3 StateTransition — payload 12, total 28
u8 state_kind; u8 reserved; u16 previous_id; u16 current_id; u16 reserved; u32 transition_seq. IDs use corresponding Screen/Tab/Action/PlaybackState enum. Emit only changes.

### 4 WorkerSample — payload 28, total 44
u16 worker_id; u8 active; u8 reserved; u32 queue_depth; u32 queue_highwater; u32 completed_delta; u32 failed_delta; u32 cancelled_delta; u32 reserved.
Service emits one per WorkerId every 1000ms. Depth/active instantaneous; highwater/deltas interval-only and reset after emission.

### 5 NetworkRequest — payload 40, total 56
u16 request_kind; u8 route_kind; u8 method; u64 duration_us; u32 http_status; u32 curl_code; u64 rx_payload_bytes; u64 tx_body_bytes; u8 attempt; u8 cancelled; u8 truncated; u8 fallback_attempt. Payload bytes are application payload, not TLS/IP wire bytes.

### 6 ArtworkSummary — payload 64, total 80
u32 cache_probe_hits, cache_probe_misses, cache_read_success, cache_read_failure, cache_write_success, cache_write_failure; u64 compressed_read_bytes, compressed_written_bytes; u32 decode_count, decode_failures; u64 decode_total_us; u32 decode_max_us; u32 reserved.
Service emits exactly once every 1000ms. All fields interval-only; reset after emission. Only ImageCache/ImageDecoder feed it.

### 7 ArtworkDecode — payload 32, total 48
u8 context; u8 outcome; u16 reserved; u64 duration_us; u64 compressed_input_bytes; u64 decoded_rgba_bytes; u32 reserved. Emitted only by ImageDecoder.

### 8 LibrarySync — payload 32, total 48
u64 duration_us; u8 outcome; u8 cache_saved; u16 reserved; u32 views_count; u32 media_count; u32 changed_hierarchy_count; u32 request_count; u32 reserved.

### 9 DownloadSample — payload 40, total 56
u32 active_downloads; u32 queued_downloads; u64 bytes_delta; u64 bytes_per_sec; u32 segments_completed_delta; u32 segment_retries_delta; u32 planner_queue_depth; u32 reserved.
Service emits exactly once every 1000ms. Gauges instantaneous; deltas reset. bytes_per_sec = bytes_delta*1,000,000/actual_interval_us.

### 10 DownloadSegmentAttempt — payload 48, total 64
u32 telemetry_job_seq; u32 segment_ordinal; u16 attempt_number; u16 retry_delay_ms; u8 route_kind; u8 outcome; u8 retry_planned; u8 reserved; u64 duration_us; u64 payload_bytes; u32 http_status; u32 curl_code; u64 reserved.

### 11 PlaybackEvent — payload 24, total 40
u8 stage; u8 source; u8 child_exit_kind (0 none,1 exited,2 signaled); u8 reserved; u64 duration_us; i32 child_exit_code; u32 reserved; u32 playback_seq.

### 12 UiStall — payload 32, total 48
u8 edge; u8 reserved; u16 screen; u16 tab; u16 action; u8 phase; u8 reserved[3]; u64 duration_us; u16 scope_id; u16 worker_mask; u64 reserved.

### 13 TelemetryHealth — payload 32, total 48
u32 queue_depth; u32 queue_highwater; u32 dropped_records_cumulative; u32 writer_errors_cumulative; u32 rotation_count; u32 sampling_late_count; u32 buffered_bytes; u32 reserved.
Service emits exactly once every 1000ms. Depth/buffer current; highwater interval and reset to current; other counters cumulative.

### 14 SessionEvent — payload 16, total 32

`SessionEvent.value0` and `value1` semantics are fully defined below; unused values are zero.
u8 kind; u8 outcome; u16 reserved; u32 value0; u64 value1.

Exact semantics:
- TelemetryStarted: outcome Success; value0=sample_interval_ms; value1=min_free_storage_bytes.
- TelemetryStopped: Success; value0=0; value1=logical_bytes_written.
- SamplingSuspended: Success; value0=SamplingReason; value1=0.
- SamplingResumed: Success; value0=SamplingReason; value1=0.
- WriterDisabledLowSpace: Skipped; value0=threshold MiB; value1=observed free_storage_bytes.
- WriterError: Failure; value0=WriterErrorKind; value1=OS error/errno numeric or 0.
Unused values are zero.

## Forward compatibility / partial tail

Decoder validates magic/header; decodes schema 1; may skip unknown type only when 16<=record_size<=256; stops safely on impossible size; ignores trailing partial record while preserving earlier complete records; assumes no alignment.

## Privacy

No strings, tokens, URLs, headers, bodies, credentials, persistent Jellyfin IDs, titles, image tags, cache paths, scopes, or hashed persistent identifiers.
