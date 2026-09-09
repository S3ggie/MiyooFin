#include "MediaItemSql.hpp"

#include "../data/MediaItem.hpp"
#include "../../vendor/sqlite/sqlite3.h"

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

} // namespace miyoofin
