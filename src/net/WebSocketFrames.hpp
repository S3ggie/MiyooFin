#ifndef MIYOOFIN_WEBSOCKET_FRAMES_HPP
#define MIYOOFIN_WEBSOCKET_FRAMES_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace miyoofin {

/// Reads server-to-client WebSocket frames (RFC 6455) out of a byte buffer, with the limits a
/// 128 MB handheld needs: a frame that declares more than `maxMessageBytes` is an error the
/// moment its header is read (nothing waits for, or buffers, the body), and a fragmented
/// message is reassembled only up to the same limit.
class WebSocketFrameReader
{
  public:
    enum class Status
    {
        NeedMore, // not a whole frame yet
        Message,  // a complete text message is in `payload`
        Ping,     // a ping: answer with a pong carrying `payload`
        Close,    // the server closed the connection
        Error     // protocol violation or limit exceeded: `error` says which; drop the connection
    };
    struct Result
    {
        Status status = Status::NeedMore;
        std::string payload;
        std::string error;
    };

    explicit WebSocketFrameReader(std::size_t maxMessageBytes) : m_max(maxMessageBytes) {}

    /// Consumes bytes from the front of `bytes` and reports the next event that matters (pongs
    /// and binary messages are skipped). Call again until NeedMore.
    Result next(std::vector<unsigned char>& bytes);

  private:
    std::size_t m_max;
    std::string m_message;    // the data message being reassembled
    bool m_inMessage = false; // a fragmented message is in progress
    bool m_messageIsText = false;
};

/// A client-to-server frame. Clients must mask what they send, so `mask` (4 bytes) is applied.
std::string buildMaskedFrame(unsigned char opcode, const std::string& payload,
                             const unsigned char mask[4]);
/// A pong answering `pingPayload` (control payloads are at most 125 bytes), masked with random
/// bytes.
std::string buildPong(const std::string& pingPayload);

} // namespace miyoofin

#endif
