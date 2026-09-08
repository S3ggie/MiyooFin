#include "TelemetryWriter.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <new>
#include <sys/stat.h>

namespace miyoofin {
namespace {

constexpr const char *kTelemetryFileName = "telemetry.mft";

bool ensureDirectory(const std::string &path) noexcept
{
    if (path.empty()) {
        errno = EINVAL;
        return false;
    }

    std::size_t start = path[0] == '/' ? 1 : 0;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        if (end > start) {
            const std::string component = path.substr(0, end);
            if (::mkdir(component.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
            struct stat status{};
            if (::stat(component.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
                errno = ENOTDIR;
                return false;
            }
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

int currentErrorOr(int fallback) noexcept
{
    return errno == 0 ? fallback : errno;
}

} // namespace

TelemetryWriter::~TelemetryWriter() noexcept
{
    close();
}

void TelemetryWriter::setError(WriterErrorKind kind, int errorNumber) noexcept
{
    lastErrorKind_ = kind;
    lastErrorNumber_ = errorNumber == 0 ? EIO : errorNumber;
}

bool TelemetryWriter::open(const TelemetryConfig &config, const MftFileHeader &header)
{
    close();
    lastErrorKind_ = WriterErrorKind::Unknown;
    lastErrorNumber_ = 0;
    logicalBytesWritten_ = 0;
    lastFlushMonotonicUs_ = 0;
    flushIntervalMs_ = config.flushIntervalMs;
    rotationCount_ = 0;
    config_ = config;
    header_ = header;
    buffered_ = 0;
    buffer_.clear();
    path_.clear();

    if (config.writerBufferBytes == 0) {
        setError(WriterErrorKind::Open, EINVAL);
        return false;
    }

    try {
        buffer_.resize(config.writerBufferBytes);
    } catch (const std::bad_alloc &) {
        setError(WriterErrorKind::Open, ENOMEM);
        return false;
    }

    if (!ensureDirectory(config.targetDirectory)) {
        setError(WriterErrorKind::Open, currentErrorOr(EIO));
        buffer_.clear();
        return false;
    }

    path_ = config.targetDirectory;
    if (path_.back() != '/')
        path_ += '/';
    path_ += kTelemetryFileName;
    file_ = std::fopen(path_.c_str(), "wb");
    if (file_ == nullptr) {
        setError(WriterErrorKind::Open, currentErrorOr(EIO));
        buffer_.clear();
        path_.clear();
        return false;
    }

    std::array<uint8_t, kMftFileHeaderSize> encodedHeader{};
    if (!encodeFileHeader(header, encodedHeader.data(), encodedHeader.size())
        || std::fwrite(encodedHeader.data(), 1, encodedHeader.size(), file_) != encodedHeader.size()) {
        setError(WriterErrorKind::Write, currentErrorOr(EIO));
        std::fclose(file_);
        file_ = nullptr;
        buffer_.clear();
        path_.clear();
        return false;
    }
    if (std::fflush(file_) != 0) {
        setError(WriterErrorKind::Flush, currentErrorOr(EIO));
        std::fclose(file_);
        file_ = nullptr;
        buffer_.clear();
        path_.clear();
        return false;
    }
    logicalBytesWritten_ = kMftFileHeaderSize;
    return true;
}

bool TelemetryWriter::append(const TelemetryRecord &record)
{
    if (!isOpen()) {
        setError(WriterErrorKind::Write, EINVAL);
        return false;
    }

    std::array<uint8_t, kMftMaxRecordSize> encoded{};
    uint16_t encodedSize = 0;
    if (!encodeRecord(record, encoded.data(), encoded.size(), encodedSize)) {
        setError(WriterErrorKind::Write, EINVAL);
        return false;
    }

    if (config_.rotateBytes != 0
        && logicalBytesWritten_ + encodedSize > config_.rotateBytes) {
        const std::string previousPath = path_;
        const uint32_t previousIndex = header_.rotation_index;
        const uint32_t nextIndex = previousIndex + 1;
        if (!close())
            return false;

        const std::string rotatedPath = config_.targetDirectory + "/telemetry-"
            + std::to_string(previousIndex) + ".mft";
        std::remove(rotatedPath.c_str());
        if (std::rename(previousPath.c_str(), rotatedPath.c_str()) != 0) {
            setError(WriterErrorKind::Rotate, currentErrorOr(EIO));
            return false;
        }
        const uint32_t retainFiles = config_.retainFiles == 0 ? 1 : config_.retainFiles;
        if (nextIndex >= retainFiles) {
            const uint32_t expiredIndex = nextIndex - retainFiles;
            const std::string expiredPath = config_.targetDirectory + "/telemetry-"
                + std::to_string(expiredIndex) + ".mft";
            std::remove(expiredPath.c_str());
        }

        MftFileHeader nextHeader = header_;
        nextHeader.rotation_index = nextIndex;
        const uint32_t rotations = rotationCount_;
        if (!open(config_, nextHeader)) {
            if (lastErrorKind_ == WriterErrorKind::Unknown)
                setError(WriterErrorKind::Rotate, currentErrorOr(EIO));
            return false;
        }
        rotationCount_ = rotations + 1;
    }

    if (encodedSize > buffer_.size()) {
        if (!flush())
            return false;
        if (std::fwrite(encoded.data(), 1, encodedSize, file_) != encodedSize) {
            setError(WriterErrorKind::Write, currentErrorOr(EIO));
            return false;
        }
        logicalBytesWritten_ += encodedSize;
        return true;
    }

    if (buffered_ + encodedSize > buffer_.size() && !flush())
        return false;
    std::copy(encoded.begin(), encoded.begin() + encodedSize, buffer_.begin() + buffered_);
    buffered_ += encodedSize;
    logicalBytesWritten_ += encodedSize;
    if (buffered_ == buffer_.size())
        return flush();
    return true;
}

bool TelemetryWriter::flushIfDue(uint64_t monotonicUs) noexcept
{
    if (!isOpen())
        return false;
    const uint64_t intervalUs = static_cast<uint64_t>(flushIntervalMs_) * 1000ull;
    if (monotonicUs < lastFlushMonotonicUs_
        || monotonicUs - lastFlushMonotonicUs_ < intervalUs)
        return true;
    lastFlushMonotonicUs_ = monotonicUs;
    return flush();
}

bool TelemetryWriter::flush() noexcept
{
    if (!isOpen()) {
        setError(WriterErrorKind::Flush, EINVAL);
        return false;
    }

    if (buffered_ != 0) {
        if (std::fwrite(buffer_.data(), 1, buffered_, file_) != buffered_) {
            setError(WriterErrorKind::Write, currentErrorOr(EIO));
            return false;
        }
        buffered_ = 0;
    }
    if (std::fflush(file_) != 0) {
        setError(WriterErrorKind::Flush, currentErrorOr(EIO));
        return false;
    }
    return true;
}

bool TelemetryWriter::close() noexcept
{
    if (!isOpen()) {
        buffered_ = 0;
        buffer_.clear();
        path_.clear();
        return true;
    }

    bool success = flush();
    if (std::fclose(file_) != 0) {
        setError(WriterErrorKind::Close, currentErrorOr(EIO));
        success = false;
    }
    file_ = nullptr;
    buffered_ = 0;
    buffer_.clear();
    path_.clear();
    return success;
}

bool TelemetryWriter::isOpen() const noexcept
{
    return file_ != nullptr;
}

uint64_t TelemetryWriter::logicalBytesWritten() const noexcept
{
    return logicalBytesWritten_;
}

std::size_t TelemetryWriter::bufferedBytes() const noexcept
{
    return buffered_;
}

uint32_t TelemetryWriter::rotationCount() const noexcept
{
    return rotationCount_;
}

WriterErrorKind TelemetryWriter::lastErrorKind() const noexcept
{
    return lastErrorKind_;
}

int TelemetryWriter::lastErrorNumber() const noexcept
{
    return lastErrorNumber_;
}

} // namespace miyoofin
