"""Independent, standard-library definitions for MFT v1."""

FILE_HEADER_SIZE = 80
RECORD_HEADER_SIZE = 16
MAX_RECORD_SIZE = 256
MAGIC = b"MFT1"
SCHEMA_VERSION = 1
FILE_HEADER_FORMAT = "<IIQQQIIIIIIIIQ"
RECORD_HEADER_FORMAT = "<HHIQ"

PLATFORM_IDS = {
    0: "Unknown",
    1: "MiyooMiniPlusOnionOS",
    2: "Host",
}
OUTCOMES = {
    0: "Unknown",
    1: "Success",
    2: "Failure",
    3: "Cancelled",
    4: "Dropped",
    5: "Skipped",
    6: "Unavailable",
}
ROUTE_KINDS = {0: "Unknown", 1: "Lan", 2: "Public"}
HTTP_METHODS = {0: "Unknown", 1: "Get", 2: "Post"}
FRAME_PHASES = {
    1: "FullFrame",
    2: "Input",
    3: "Update",
    4: "Transition",
    5: "ScreenRender",
    6: "FramebufferUpload",
    7: "Present",
}
STATE_KINDS = {1: "Screen", 2: "Tab", 3: "Action", 4: "PlaybackState"}
PLAYBACK_STATES = {
    0: "Unknown",
    1: "UiActive",
    2: "StartingOverlay",
    3: "ExternalPlayback",
    4: "Resuming",
}
SCREEN_IDS = {
    0: "None",
    1: "Startup",
    2: "ServerEntry",
    3: "Connect",
    4: "Login",
    5: "AuthCheck",
    6: "Home",
    7: "Series",
    8: "EpisodeBrowser",
    9: "MovieDetails",
    10: "InputDiagnostics",
    11: "Other",
}
TAB_IDS = {
    0: "NotApplicable",
    1: "Home",
    2: "Movies",
    3: "Shows",
    4: "Downloads",
    5: "Settings",
    6: "Other",
}
ACTION_IDS = {
    0: "None",
    1: "Up",
    2: "Down",
    3: "Left",
    4: "Right",
    5: "Confirm",
    6: "Back",
    7: "Search",
    8: "ActionsMenu",
    9: "PrevTab",
    10: "NextTab",
    11: "PrevPage",
    12: "NextPage",
    13: "Settings",
    14: "Menu",
    15: "Exit",
    16: "Raw",
    17: "Other",
}
UI_PHASE_IDS = {
    0: "Unknown",
    1: "IdleFrameBoundary",
    2: "EventInput",
    3: "Update",
    4: "ScreenTransition",
    5: "Render",
}
UI_SCOPES = {
    0: "Unknown",
    1: "Screen::handleAction",
    2: "Screen::update",
    3: "App::finishSavedSessionValidation",
    4: "Screen::render",
    5: "ScreenStack::push -> null",
    6: "ScreenStack::push -> MovieDetailsScreen",
    7: "ScreenStack::push -> SeriesScreen",
    8: "ScreenStack::push -> EpisodeBrowserScreen",
    9: "ScreenStack::push -> HomeScreen",
    10: "ScreenStack::push -> Screen",
    11: "ScreenStack::pop",
    12: "ScreenStack::signalLeave",
    13: "ScreenStack::retireScreen",
    14: "ScreenStack::enterPrevious",
    15: "HomeScreen::publishLibraryResult",
    16: "HomeScreen::publishResumeResult",
    17: "HomeScreen::updateArtworkWorkingSet",
    18: "HomeScreen::publishDecodedArtwork",
    19: "HomeScreen::queueSelectedArtwork",
    20: "HomeScreen::queueVisibleArtwork",
    21: "HomeScreen::publishDownloadSnapshot",
    22: "EpisodeBrowserScreen::enter",
    23: "EpisodeBrowserScreen::workerShutdown",
    24: "EpisodeBrowserScreen::publishEpisodes",
    25: "EpisodeBrowserScreen::publishArtworkResult",
    26: "MovieDetailsScreen::enter",
    27: "MovieDetailsScreen::worker cancellation",
    28: "MovieDetailsScreen::owned worker creation",
    29: "MovieDetailsScreen::publish async preparation",
    30: "MovieDetailsScreen::publish image surface preparation",
    31: "MovieDetailsScreen::publish playback/download state",
    32: "DownloadManager::planSnapshot mutex wait",
}
REQUEST_KINDS = {
    0: "Unknown",
    1: "SystemInfo",
    2: "Authentication",
    3: "TokenValidation",
    4: "Views",
    5: "LibraryItems",
    6: "ResumeItems",
    7: "LatestItems",
    8: "ChangedHierarchy",
    9: "Seasons",
    10: "Episodes",
    11: "PlaybackPosition",
    12: "PlaybackStopped",
    13: "DownloadPlaybackInfo",
    14: "HlsMaster",
    15: "HlsVariant",
    16: "Artwork",
}
ARTWORK_CONTEXTS = {
    0: "Unknown",
    1: "HomeSelected",
    2: "HomeGrid",
    3: "HomeShows",
    4: "HomePoster",
    5: "EpisodeSelected",
    6: "EpisodePrefetch",
    7: "MovieDetails",
    8: "CacheGeneric",
}
PLAYBACK_STAGES = {
    1: "RequestToFinalPresent",
    2: "SuspendPlatform",
    3: "ChildWait",
    4: "ResumePlatform",
    5: "ReturnToFirstNormalFrame",
}
PLAYBACK_SOURCES = {0: "Unknown", 1: "Jellyfin", 2: "Local"}
STALL_EDGES = {1: "Begin", 2: "End", 3: "SlowScope"}
SESSION_EVENT_KINDS = {
    1: "TelemetryStarted",
    2: "TelemetryStopped",
    3: "SamplingSuspended",
    4: "SamplingResumed",
    5: "WriterDisabledLowSpace",
    6: "WriterError",
}
WRITER_ERROR_KINDS = {0: "Unknown", 1: "Open", 2: "Write", 3: "Flush", 4: "Rotate", 5: "Close"}
SAMPLING_REASONS = {0: "Unknown", 1: "ExternalPlayback"}
CHILD_EXIT_KINDS = {0: "None", 1: "Exited", 2: "Signaled"}

RECORD_TYPES = {
    1: ("SystemSample", 88),
    2: ("FrameTimingSummary", 88),
    3: ("StateTransition", 28),
    4: ("WorkerSample", 44),
    5: ("NetworkRequest", 56),
    6: ("ArtworkSummary", 80),
    7: ("ArtworkDecode", 48),
    8: ("LibrarySync", 48),
    9: ("DownloadSample", 56),
    10: ("DownloadSegmentAttempt", 64),
    11: ("PlaybackEvent", 40),
    12: ("UiStall", 48),
    13: ("TelemetryHealth", 48),
    14: ("SessionEvent", 32),
}

RECORD_LAYOUTS = {
    1: {"name": "SystemSample", "payload_size": 72, "format": "<7Q4I"},
    2: {"name": "FrameTimingSummary", "payload_size": 72, "format": "<B3xIIQIII9II"},
    3: {"name": "StateTransition", "payload_size": 12, "format": "<BBHHHI"},
    4: {"name": "WorkerSample", "payload_size": 28, "format": "<HBB6I"},
    5: {"name": "NetworkRequest", "payload_size": 40, "format": "<HBBQIIQQBBBB"},
    6: {"name": "ArtworkSummary", "payload_size": 64, "format": "<6I2Q2IQ2I"},
    7: {"name": "ArtworkDecode", "payload_size": 32, "format": "<BBHQQQI"},
    8: {"name": "LibrarySync", "payload_size": 32, "format": "<QBBH5I"},
    9: {"name": "DownloadSample", "payload_size": 40, "format": "<IIQQ4I"},
    10: {"name": "DownloadSegmentAttempt", "payload_size": 48, "format": "<IIHHBBBBQQIIQ"},
    11: {"name": "PlaybackEvent", "payload_size": 24, "format": "<BBBBQiII"},
    12: {"name": "UiStall", "payload_size": 32, "format": "<BBHHHB3xQHHQ"},
    13: {"name": "TelemetryHealth", "payload_size": 32, "format": "<8I"},
    14: {"name": "SessionEvent", "payload_size": 16, "format": "<BBHIQ"},
}


def enum_name(values, value):
    return values.get(value, "Unknown({})".format(value))
