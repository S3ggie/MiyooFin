#ifndef MIYOOFIN_HOME_DOWNLOADS_STATE_HPP
#define MIYOOFIN_HOME_DOWNLOADS_STATE_HPP

#include "../download/DownloadHierarchy.hpp"
#include "../download/DownloadTypes.hpp"
#include "../playback/OfflinePlaybackJournal.hpp"
#include <set>
#include <string>
#include <vector>

namespace miyoofin {

// UI-thread state and pure transitions for Home's Downloads tab. It holds the
// copied manager snapshot, the expanded/selected hierarchy view and the
// two-press confirmation state. It performs no I/O: operations that must
// erase downloads or discard journal entries return the ids and the caller
// (HomeScreen) carries them out.
class HomeDownloadsState
{
  public:
    static constexpr int kVisibleRows = 5;

    DownloadSnapshot snapshot;
    DownloadHierarchy hierarchy;
    std::vector<OfflinePlaybackEntry> missingJournal;
    int selected = 0;
    int scroll = 0;
    std::string selectedId;
    std::string confirmId;
    std::vector<std::string> confirmItemIds;
    std::string journalDiscardConfirmId;
    std::set<std::string> expanded;

    // Replaces the published data and re-derives hierarchy, selection, scroll
    // and any confirmation that no longer applies.
    void publish(DownloadSnapshot&& newSnapshot, std::vector<OfflinePlaybackEntry>&& newMissing);

    // Up/Down: clamps, tracks the selected id and drops a pending confirm.
    void moveSelection(int delta);

    void clearConfirm();

    // Confirm on a Series/Season row toggles it. Returns false (no change)
    // when the selected row is not a collapsible parent.
    bool toggleSelectedParent();

    // Y on the selected row. Returns the item ids to erase NOW when this
    // press confirms a pending removal; returns empty when it only arms (or
    // the row has nothing removable).
    std::vector<std::string> pressRemoval();

    // Search with a missing-journal entry pending. Returns the entry id to
    // discard when this press confirms it; empty when it only arms.
    std::string pressJournalDiscard();

    const DownloadHierarchyRow* selectedRow() const;
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_DOWNLOADS_STATE_HPP
