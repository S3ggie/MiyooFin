#pragma once

#include <cstdint>

namespace miyoofin {

enum class PlatformId : uint32_t {
    Unknown = 0,
    MiyooMiniPlusOnionOS = 1,
    Host = 2,
};

enum class Outcome : uint8_t {
    Unknown = 0,
    Success = 1,
    Failure = 2,
    Cancelled = 3,
    Dropped = 4,
    Skipped = 5,
    Unavailable = 6,
};

enum class RouteKind : uint8_t {
    Unknown = 0,
    Lan = 1,
    Public = 2,
};

enum class HttpMethod : uint8_t {
    Unknown = 0,
    Get = 1,
    Post = 2,
};

enum class ScreenId : uint16_t {
    None = 0,
    Startup = 1,
    ServerEntry = 2,
    Connect = 3,
    Login = 4,
    AuthCheck = 5,
    Home = 6,
    Series = 7,
    EpisodeBrowser = 8,
    MovieDetails = 9,
    InputDiagnostics = 10,
    Other = 11,
};

enum class TabId : uint16_t {
    NotApplicable = 0,
    Home = 1,
    Movies = 2,
    Shows = 3,
    Downloads = 4,
    Settings = 5,
    Other = 6,
};

enum class ActionId : uint16_t {
    None = 0,
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
    Confirm = 5,
    Back = 6,
    Search = 7,
    ActionsMenu = 8,
    PrevTab = 9,
    NextTab = 10,
    PrevPage = 11,
    NextPage = 12,
    Settings = 13,
    Menu = 14,
    Exit = 15,
    Raw = 16,
    Other = 17,
};

enum class FramePhase : uint8_t {
    FullFrame = 1,
    Input = 2,
    Update = 3,
    Transition = 4,
    ScreenRender = 5,
    FramebufferUpload = 6,
    Present = 7,
};

enum class StateKind : uint8_t {
    Screen = 1,
    Tab = 2,
    Action = 3,
    PlaybackState = 4,
};

enum class PlaybackState : uint16_t {
    Unknown = 0,
    UiActive = 1,
    StartingOverlay = 2,
    ExternalPlayback = 3,
    Resuming = 4,
};

enum class UiPhaseId : uint8_t {
    Unknown = 0,
    IdleFrameBoundary = 1,
    EventInput = 2,
    Update = 3,
    ScreenTransition = 4,
    Render = 5,
};

enum class UiScopeId : uint16_t {
    Unknown = 0,
    ScreenHandleAction = 1,
    ScreenUpdate = 2,
    AppFinishSavedSessionValidation = 3,
    ScreenRender = 4,
    ScreenStackPushNull = 5,
    ScreenStackPushMovieDetailsScreen = 6,
    ScreenStackPushSeriesScreen = 7,
    ScreenStackPushEpisodeBrowserScreen = 8,
    ScreenStackPushHomeScreen = 9,
    ScreenStackPushScreen = 10,
    ScreenStackPop = 11,
    ScreenStackSignalLeave = 12,
    ScreenStackRetireScreen = 13,
    ScreenStackEnterPrevious = 14,
    HomeScreenPublishLibraryResult = 15,
    HomeScreenPublishResumeResult = 16,
    HomeScreenUpdateArtworkWorkingSet = 17,
    HomeScreenPublishDecodedArtwork = 18,
    HomeScreenQueueSelectedArtwork = 19,
    HomeScreenQueueVisibleArtwork = 20,
    HomeScreenPublishDownloadSnapshot = 21,
    EpisodeBrowserScreenEnter = 22,
    EpisodeBrowserScreenWorkerShutdown = 23,
    EpisodeBrowserScreenPublishEpisodes = 24,
    EpisodeBrowserScreenPublishArtworkResult = 25,
    MovieDetailsScreenEnter = 26,
    MovieDetailsScreenWorkerCancellation = 27,
    MovieDetailsScreenOwnedWorkerCreation = 28,
    MovieDetailsScreenPublishAsyncPreparation = 29,
    MovieDetailsScreenPublishImageSurfacePreparation = 30,
    MovieDetailsScreenPublishPlaybackDownloadState = 31,
    DownloadManagerPlanSnapshotMutexWait = 32,
};

enum class WorkerId : uint16_t {
    HomeLibraryFetch = 1,
    HomeHierarchy = 2,
    HomePoster = 3,
    HomeDecode = 4,
    HomeResumeRefresh = 5,
    HomeDownloadRefresh = 6,
    EpisodeFetch = 7,
    EpisodeArtwork = 8,
    DownloadTransfer = 9,
    DownloadPlanner = 10,
    DownloadReconcile = 11,
    ScreenRetirement = 12,
    SavedSessionValidation = 13,
    PlaybackJournalSync = 14,
};

constexpr uint16_t kWorkerMaskHomeLibraryFetch = uint16_t{1} << 0;
constexpr uint16_t kWorkerMaskHomeHierarchy = uint16_t{1} << 1;
constexpr uint16_t kWorkerMaskHomePoster = uint16_t{1} << 2;
constexpr uint16_t kWorkerMaskHomeDecode = uint16_t{1} << 3;
constexpr uint16_t kWorkerMaskHomeResumeRefresh = uint16_t{1} << 4;
constexpr uint16_t kWorkerMaskHomeDownloadRefresh = uint16_t{1} << 5;
constexpr uint16_t kWorkerMaskEpisodeFetch = uint16_t{1} << 6;
constexpr uint16_t kWorkerMaskEpisodeArtwork = uint16_t{1} << 7;
constexpr uint16_t kWorkerMaskDownloadTransfer = uint16_t{1} << 8;
constexpr uint16_t kWorkerMaskDownloadPlanner = uint16_t{1} << 9;
constexpr uint16_t kWorkerMaskDownloadReconcile = uint16_t{1} << 10;
constexpr uint16_t kWorkerMaskScreenRetirement = uint16_t{1} << 11;
constexpr uint16_t kWorkerMaskSavedSessionValidation = uint16_t{1} << 12;
constexpr uint16_t kWorkerMaskPlaybackJournalSync = uint16_t{1} << 13;
constexpr uint16_t kWorkerMaskAll = uint16_t{0x3fff};

enum class RequestKind : uint16_t {
    Unknown = 0,
    SystemInfo = 1,
    Authentication = 2,
    TokenValidation = 3,
    Views = 4,
    LibraryItems = 5,
    ResumeItems = 6,
    LatestItems = 7,
    ChangedHierarchy = 8,
    Seasons = 9,
    Episodes = 10,
    PlaybackPosition = 11,
    PlaybackStopped = 12,
    DownloadPlaybackInfo = 13,
    HlsMaster = 14,
    HlsVariant = 15,
    Artwork = 16,
};

enum class ArtworkContext : uint8_t {
    Unknown = 0,
    HomeSelected = 1,
    HomeGrid = 2,
    HomeShows = 3,
    HomePoster = 4,
    EpisodeSelected = 5,
    EpisodePrefetch = 6,
    MovieDetails = 7,
    CacheGeneric = 8,
};

enum class PlaybackStage : uint8_t {
    RequestToFinalPresent = 1,
    SuspendPlatform = 2,
    ChildWait = 3,
    ResumePlatform = 4,
    ReturnToFirstNormalFrame = 5,
};

enum class PlaybackSourceKind : uint8_t {
    Unknown = 0,
    Jellyfin = 1,
    Local = 2,
};

enum class StallEdge : uint8_t {
    Begin = 1,
    End = 2,
    SlowScope = 3,
};

enum class SessionEventKind : uint8_t {
    TelemetryStarted = 1,
    TelemetryStopped = 2,
    SamplingSuspended = 3,
    SamplingResumed = 4,
    WriterDisabledLowSpace = 5,
    WriterError = 6,
};

enum class WriterErrorKind : uint32_t {
    Unknown = 0,
    Open = 1,
    Write = 2,
    Flush = 3,
    Rotate = 4,
    Close = 5,
};

enum class SamplingReason : uint32_t {
    Unknown = 0,
    ExternalPlayback = 1,
};

enum class RecordType : uint16_t {
    SystemSample = 1,
    FrameTimingSummary = 2,
    StateTransition = 3,
    WorkerSample = 4,
    NetworkRequest = 5,
    ArtworkSummary = 6,
    ArtworkDecode = 7,
    LibrarySync = 8,
    DownloadSample = 9,
    DownloadSegmentAttempt = 10,
    PlaybackEvent = 11,
    UiStall = 12,
    TelemetryHealth = 13,
    SessionEvent = 14,
};

} // namespace miyoofin
