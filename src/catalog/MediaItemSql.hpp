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
    StepFailed,
    InvalidOrdinal,
};

struct MediaItemCollectionStatements {
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteImageTags = nullptr;
    sqlite3_stmt *insertImageTag = nullptr;
    sqlite3_stmt *selectGenres = nullptr;
    sqlite3_stmt *selectImageTags = nullptr;
};

int mediaItemKindToSql(const std::string &type, MediaItemSqlError &error);
std::string mediaItemKindFromSql(int kind, MediaItemSqlError &error);

bool bindMediaItemScalars(sqlite3_stmt *statement, const MediaItem &item,
                          MediaItemSqlError &error);
bool readMediaItemScalars(sqlite3_stmt *statement, MediaItem &item,
                          MediaItemSqlError &error);
bool replaceMediaItemCollections(const MediaItemCollectionStatements &statements,
                                 const MediaItem &item,
                                 MediaItemSqlError &error);
bool readMediaItemCollections(const MediaItemCollectionStatements &statements,
                              MediaItem &item, MediaItemSqlError &error);
bool mediaItemsEquivalentForCatalog(const MediaItem &expected,
                                    const MediaItem &actual);

} // namespace miyoofin

#endif // MIYOOFIN_MEDIA_ITEM_SQL_HPP
