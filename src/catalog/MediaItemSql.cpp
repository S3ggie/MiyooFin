#include "MediaItemSql.hpp"

#include "../data/MediaItem.hpp"
#include "../../vendor/sqlite/sqlite3.h"

#include <cmath>
#include <limits>

namespace miyoofin {

namespace {

bool bindResult(int rc, MediaItemSqlError &error)
{
    if (rc != SQLITE_OK) {
        error = MediaItemSqlError::BindFailed;
        return false;
    }
    return true;
}

int bindOptionalText(sqlite3_stmt *statement, int parameter,
                    const std::string &value)
{
    if (value.empty()) {
        return sqlite3_bind_null(statement, parameter);
    }
    return sqlite3_bind_text(statement, parameter, value.c_str(), -1,
                             SQLITE_TRANSIENT);
}

bool textColumn(sqlite3_stmt *statement, int column, std::string &value)
{
    const unsigned char *text = sqlite3_column_text(statement, column);
    value = text ? reinterpret_cast<const char *>(text) : "";
    return true;
}

void resetStatement(sqlite3_stmt *statement)
{
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
}

bool bindItemId(sqlite3_stmt *statement, const MediaItem &item,
                MediaItemSqlError &error)
{
    if (item.id.empty()) {
        error = MediaItemSqlError::MissingId;
        return false;
    }
    return bindResult(sqlite3_bind_text(statement, 1, item.id.c_str(), -1,
                                        SQLITE_TRANSIENT), error);
}

bool stepDone(sqlite3_stmt *statement, MediaItemSqlError &error)
{
    if (sqlite3_step(statement) != SQLITE_DONE) {
        error = MediaItemSqlError::StepFailed;
        return false;
    }
    return true;
}

} // namespace

int mediaItemKindToSql(const std::string &type, MediaItemSqlError &error)
{
    error = MediaItemSqlError::None;
    if (type == "movie") {
        return 1;
    }
    if (type == "show") {
        return 2;
    }
    if (type == "season") {
        return 3;
    }
    if (type == "episode") {
        return 4;
    }
    error = MediaItemSqlError::InvalidKind;
    return 0;
}

std::string mediaItemKindFromSql(int kind, MediaItemSqlError &error)
{
    error = MediaItemSqlError::None;
    switch (kind) {
    case 1:
        return "movie";
    case 2:
        return "show";
    case 3:
        return "season";
    case 4:
        return "episode";
    default:
        error = MediaItemSqlError::InvalidKind;
        return {};
    }
}

bool bindMediaItemScalars(sqlite3_stmt *statement, const MediaItem &item,
                          MediaItemSqlError &error)
{
    error = MediaItemSqlError::None;
    if (item.id.empty()) {
        error = MediaItemSqlError::MissingId;
        return false;
    }
    const int kind = mediaItemKindToSql(item.type, error);
    if (error != MediaItemSqlError::None) {
        return false;
    }

    return bindResult(sqlite3_bind_text(statement, 1, item.id.c_str(), -1,
                                        SQLITE_TRANSIENT), error)
        && bindResult(sqlite3_bind_int(statement, 2, kind), error)
        && bindResult(sqlite3_bind_text(statement, 3, item.title.c_str(), -1,
                                        SQLITE_TRANSIENT), error)
        && bindResult(sqlite3_bind_text(statement, 4, item.overview.c_str(),
                                        -1, SQLITE_TRANSIENT), error)
        && bindResult(sqlite3_bind_int(statement, 5, item.year), error)
        && bindResult(sqlite3_bind_double(statement, 6, item.rating), error)
        && bindResult(sqlite3_bind_text(statement, 7, item.etag.c_str(), -1,
                                        SQLITE_TRANSIENT), error)
        && bindResult(sqlite3_bind_int(statement, 8, item.played ? 1 : 0),
                      error)
        && bindResult(sqlite3_bind_double(statement, 9, item.progress), error)
        && bindResult(sqlite3_bind_int64(statement, 10,
                                         item.playbackPositionTicks), error)
        && bindResult(sqlite3_bind_int(statement, 11, item.indexNumber), error)
        && bindResult(sqlite3_bind_int(statement, 12, item.parentIndexNumber),
                      error)
        && bindResult(sqlite3_bind_int64(statement, 13, item.runTimeTicks),
                      error)
        && bindResult(sqlite3_bind_text(statement, 14, item.seriesName.c_str(),
                                        -1, SQLITE_TRANSIENT), error)
        && bindResult(bindOptionalText(statement, 15, item.seriesId), error)
        && bindResult(bindOptionalText(statement, 16, item.seasonId), error)
        && bindResult(sqlite3_bind_int(statement, 17, item.artR), error)
        && bindResult(sqlite3_bind_int(statement, 18, item.artG), error)
        && bindResult(sqlite3_bind_int(statement, 19, item.artB), error);
}

bool readMediaItemScalars(sqlite3_stmt *statement, MediaItem &item,
                          MediaItemSqlError &error)
{
    error = MediaItemSqlError::None;
    MediaItemSqlError kindError = MediaItemSqlError::None;
    item.id.clear();
    textColumn(statement, 0, item.id);
    item.type = mediaItemKindFromSql(sqlite3_column_int(statement, 1),
                                     kindError);
    if (kindError != MediaItemSqlError::None) {
        error = kindError;
        return false;
    }
    textColumn(statement, 2, item.title);
    textColumn(statement, 3, item.overview);
    item.year = sqlite3_column_int(statement, 4);
    item.rating = static_cast<float>(sqlite3_column_double(statement, 5));
    textColumn(statement, 6, item.etag);
    item.played = sqlite3_column_int(statement, 7) != 0;
    item.progress = static_cast<float>(sqlite3_column_double(statement, 8));
    item.playbackPositionTicks = sqlite3_column_int64(statement, 9);
    item.indexNumber = sqlite3_column_int(statement, 10);
    item.parentIndexNumber = sqlite3_column_int(statement, 11);
    item.runTimeTicks = sqlite3_column_int64(statement, 12);
    textColumn(statement, 13, item.seriesName);
    textColumn(statement, 14, item.seriesId);
    textColumn(statement, 15, item.seasonId);
    item.artR = static_cast<Uint8>(sqlite3_column_int(statement, 16));
    item.artG = static_cast<Uint8>(sqlite3_column_int(statement, 17));
    item.artB = static_cast<Uint8>(sqlite3_column_int(statement, 18));
    return true;
}

bool replaceMediaItemCollections(const MediaItemCollectionStatements &statements,
                                 const MediaItem &item,
                                 MediaItemSqlError &error)
{
    error = MediaItemSqlError::None;
    if (item.id.empty()) {
        error = MediaItemSqlError::MissingId;
        return false;
    }
    if (item.genres.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = MediaItemSqlError::InvalidOrdinal;
        return false;
    }

    resetStatement(statements.deleteGenres);
    if (!bindItemId(statements.deleteGenres, item, error)
        || !stepDone(statements.deleteGenres, error)) {
        resetStatement(statements.deleteGenres);
        return false;
    }
    resetStatement(statements.deleteGenres);

    for (std::size_t ordinal = 0; ordinal < item.genres.size(); ++ordinal) {
        resetStatement(statements.insertGenre);
        if (!bindResult(sqlite3_bind_text(statements.insertGenre, 1,
                                          item.id.c_str(), -1,
                                          SQLITE_TRANSIENT), error)
            || !bindResult(sqlite3_bind_int(
                               statements.insertGenre, 2,
                               static_cast<int>(ordinal)), error)
            || !bindResult(sqlite3_bind_text(statements.insertGenre, 3,
                                             item.genres[ordinal].c_str(), -1,
                                             SQLITE_TRANSIENT), error)
            || !stepDone(statements.insertGenre, error)) {
            resetStatement(statements.insertGenre);
            return false;
        }
        resetStatement(statements.insertGenre);
    }

    resetStatement(statements.deleteImageTags);
    if (!bindItemId(statements.deleteImageTags, item, error)
        || !stepDone(statements.deleteImageTags, error)) {
        resetStatement(statements.deleteImageTags);
        return false;
    }
    resetStatement(statements.deleteImageTags);

    for (const auto &tag : item.imageTags) {
        resetStatement(statements.insertImageTag);
        if (!bindResult(sqlite3_bind_text(statements.insertImageTag, 1,
                                          item.id.c_str(), -1,
                                          SQLITE_TRANSIENT), error)
            || !bindResult(sqlite3_bind_text(statements.insertImageTag, 2,
                                             tag.first.c_str(), -1,
                                             SQLITE_TRANSIENT), error)
            || !bindResult(sqlite3_bind_text(statements.insertImageTag, 3,
                                             tag.second.c_str(), -1,
                                             SQLITE_TRANSIENT), error)
            || !stepDone(statements.insertImageTag, error)) {
            resetStatement(statements.insertImageTag);
            return false;
        }
        resetStatement(statements.insertImageTag);
    }
    return true;
}

bool readMediaItemCollections(const MediaItemCollectionStatements &statements,
                              MediaItem &item, MediaItemSqlError &error)
{
    error = MediaItemSqlError::None;
    if (item.id.empty()) {
        error = MediaItemSqlError::MissingId;
        return false;
    }
    item.genre.clear();
    item.genres.clear();
    item.imageTags.clear();

    resetStatement(statements.selectGenres);
    if (!bindItemId(statements.selectGenres, item, error)) {
        resetStatement(statements.selectGenres);
        return false;
    }
    for (;;) {
        const int rc = sqlite3_step(statements.selectGenres);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            error = MediaItemSqlError::StepFailed;
            resetStatement(statements.selectGenres);
            return false;
        }
        const int ordinal = sqlite3_column_int(statements.selectGenres, 0);
        if (ordinal != static_cast<int>(item.genres.size())) {
            error = MediaItemSqlError::InvalidOrdinal;
            resetStatement(statements.selectGenres);
            return false;
        }
        std::string genre;
        textColumn(statements.selectGenres, 1, genre);
        item.genres.push_back(std::move(genre));
    }
    resetStatement(statements.selectGenres);
    if (!item.genres.empty()) {
        item.genre = item.genres.front();
    }

    resetStatement(statements.selectImageTags);
    if (!bindItemId(statements.selectImageTags, item, error)) {
        resetStatement(statements.selectImageTags);
        return false;
    }
    for (;;) {
        const int rc = sqlite3_step(statements.selectImageTags);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            error = MediaItemSqlError::StepFailed;
            resetStatement(statements.selectImageTags);
            return false;
        }
        std::string imageType;
        std::string tag;
        textColumn(statements.selectImageTags, 0, imageType);
        textColumn(statements.selectImageTags, 1, tag);
        item.imageTags[imageType] = tag;
    }
    resetStatement(statements.selectImageTags);
    return true;
}

bool mediaItemsEquivalentForCatalog(const MediaItem &expected,
                                    const MediaItem &actual)
{
    return expected.id == actual.id && expected.title == actual.title
        && expected.overview == actual.overview && expected.year == actual.year
        && std::fabs(expected.rating - actual.rating) < 0.000001f
        && expected.genre == actual.genre && expected.type == actual.type
        && expected.etag == actual.etag && expected.genres == actual.genres
        && expected.played == actual.played
        && std::fabs(expected.progress - actual.progress) < 0.000001f
        && expected.playbackPositionTicks == actual.playbackPositionTicks
        && expected.imageTags == actual.imageTags
        && expected.indexNumber == actual.indexNumber
        && expected.parentIndexNumber == actual.parentIndexNumber
        && expected.runTimeTicks == actual.runTimeTicks
        && expected.seriesName == actual.seriesName
        && expected.seriesId == actual.seriesId
        && expected.seasonId == actual.seasonId && expected.artR == actual.artR
        && expected.artG == actual.artG && expected.artB == actual.artB;
}

} // namespace miyoofin
