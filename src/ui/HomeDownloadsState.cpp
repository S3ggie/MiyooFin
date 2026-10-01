#include "HomeDownloadsState.hpp"

#include <utility>

namespace miyoofin {

void HomeDownloadsState::publish(DownloadSnapshot&& newSnapshot,
                                 std::vector<OfflinePlaybackEntry>&& newMissing)
{
    snapshot = std::move(newSnapshot);
    missingJournal = std::move(newMissing);
    hierarchy = buildDownloadHierarchy(snapshot, expanded);
    const auto& rows = hierarchy.visible;
    selected = downloadHierarchySelection(rows, selectedId, selected);
    scroll = clampDownloadScroll(selected, static_cast<int>(rows.size()), scroll, kVisibleRows);
    selectedId = rows.empty() ? "" : rows[selected].id;
    if (!confirmId.empty() && confirmId != selectedId)
        clearConfirm();
    if (!journalDiscardConfirmId.empty() &&
        (missingJournal.empty() || missingJournal.front().itemId != journalDiscardConfirmId))
        journalDiscardConfirmId.clear();
}

void HomeDownloadsState::moveSelection(int delta)
{
    const auto& rows = hierarchy.visible;
    selected += delta;
    selected = clampDownloadSelection(selected, static_cast<int>(rows.size()));
    scroll = clampDownloadScroll(selected, static_cast<int>(rows.size()), scroll, kVisibleRows);
    selectedId = rows.empty() ? "" : rows[selected].id;
    clearConfirm();
}

void HomeDownloadsState::clearConfirm()
{
    confirmId.clear();
    confirmItemIds.clear();
}

const DownloadHierarchyRow* HomeDownloadsState::selectedRow() const
{
    const auto& rows = hierarchy.visible;
    if (selected < 0 || selected >= static_cast<int>(rows.size()))
        return nullptr;
    return &rows[selected];
}

bool HomeDownloadsState::toggleSelectedParent()
{
    const DownloadHierarchyRow* row = selectedRow();
    if (!row || (row->kind != DownloadHierarchyRowKind::Series &&
                 row->kind != DownloadHierarchyRowKind::Season))
        return false;
    // Copy before rebuilding: assigning the new hierarchy frees the storage
    // `row` points into.
    const std::string rowId = row->id;
    if (row->expanded)
        expanded.erase(rowId);
    else
        expanded.insert(rowId);
    hierarchy = buildDownloadHierarchy(snapshot, expanded);
    selected = downloadHierarchySelection(hierarchy.visible, rowId, selected);
    scroll = clampDownloadScroll(selected, static_cast<int>(hierarchy.visible.size()), scroll,
                                 kVisibleRows);
    selectedId = rowId;
    clearConfirm();
    return true;
}

std::vector<std::string> HomeDownloadsState::pressRemoval()
{
    const DownloadHierarchyRow* row = selectedRow();
    if (!row)
        return {};
    if (row->item && downloadCanRemove(*row->item)) {
        if (confirmId == row->id) {
            std::vector<std::string> ids{row->item->itemId};
            clearConfirm();
            return ids;
        }
        confirmId = row->id;
        confirmItemIds = {row->item->itemId};
        return {};
    }
    if (downloadHierarchyIsBulkRemovalParent(*row)) {
        if (downloadHierarchyBulkRemovalConfirmed(confirmId, *row)) {
            std::vector<std::string> ids = std::move(confirmItemIds);
            clearConfirm();
            return ids;
        }
        confirmItemIds = downloadHierarchyBulkRemovalItemIds(*row, snapshot);
        if (!confirmItemIds.empty())
            confirmId = row->id;
    }
    return {};
}

std::string HomeDownloadsState::pressJournalDiscard()
{
    if (missingJournal.empty())
        return {};
    const std::string id = missingJournal.front().itemId;
    if (journalDiscardConfirmId == id) {
        journalDiscardConfirmId.clear();
        return id;
    }
    journalDiscardConfirmId = id;
    return {};
}

} // namespace miyoofin
