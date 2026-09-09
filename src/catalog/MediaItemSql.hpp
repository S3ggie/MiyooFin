#ifndef MIYOOFIN_MEDIA_ITEM_SQL_HPP
#define MIYOOFIN_MEDIA_ITEM_SQL_HPP

#include <string>

struct sqlite3_stmt;

namespace miyoofin {

struct MediaItem;

enum class MediaItemSqlError : unsigned char {
    None,
    MissingId,
    InvalidKind,
    BindFailed,
    ColumnReadFailed,
};

int mediaItemKindToSql(const std::string &type, MediaItemSqlError &error);
std::string mediaItemKindFromSql(int kind, MediaItemSqlError &error);

bool bindMediaItemScalars(sqlite3_stmt *statement, const MediaItem &item,
                          MediaItemSqlError &error);
bool readMediaItemScalars(sqlite3_stmt *statement, MediaItem &item,
                          MediaItemSqlError &error);

} // namespace miyoofin

#endif // MIYOOFIN_MEDIA_ITEM_SQL_HPP
