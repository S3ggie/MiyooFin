#include "WebSocketFrames.hpp"
#include <random>

namespace miyoofin {

WebSocketFrameReader::Result WebSocketFrameReader::next(std::vector<unsigned char>& bytes)
{
    Result result;
    for (;;) {
        if (bytes.size() < 2)
            return result; // NeedMore
        const unsigned char first = bytes[0], second = bytes[1];
        const bool fin = (first & 0x80) != 0;
        const unsigned opcode = first & 0x0f;
        if ((first & 0x70) != 0) {
            result.status = Status::Error;
            result.error = "WebSocket frame uses reserved bits";
            return result;
        }
        if ((second & 0x80) != 0) {
            result.status = Status::Error;
            result.error = "WebSocket server frame is masked";
            return result;
        }
        std::uint64_t length = second & 0x7f;
        std::size_t header = 2;
        if (length == 126) {
            if (bytes.size() < 4)
                return result;
            length = (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3];
            header = 4;
        } else if (length == 127) {
            if (bytes.size() < 10)
                return result;
            length = 0;
            for (unsigned i = 0; i < 8; ++i)
                length = (length << 8) | bytes[2 + i];
            header = 10;
        }
        const bool control = opcode >= 0x8;
        if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2 && opcode != 0x8 && opcode != 0x9 &&
            opcode != 0xA) {
            result.status = Status::Error;
            result.error = "WebSocket frame has an unknown opcode";
            return result;
        }
        if (control && (length > 125 || !fin)) {
            result.status = Status::Error;
            result.error = "WebSocket control frame is invalid";
            return result;
        }
        // The limit applies to what the frame declares, before any of its body is awaited.
        if (!control && (length > m_max || m_message.size() + length > m_max)) {
            result.status = Status::Error;
            result.error = "WebSocket message exceeds the size limit";
            return result;
        }
        if (bytes.size() < header + length)
            return result; // NeedMore: the (bounded) rest of this frame
        const auto body = bytes.begin() + static_cast<std::ptrdiff_t>(header);
        std::string payload(body, body + static_cast<std::ptrdiff_t>(length));
        bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(header + length));

        if (opcode == 0x8) {
            result.status = Status::Close;
            return result;
        }
        if (opcode == 0x9) {
            result.status = Status::Ping;
            result.payload = std::move(payload);
            return result;
        }
        if (opcode == 0xA)
            continue; // a pong: nothing to do
        // Data frame (text, binary or continuation).
        if (opcode == 0x0) {
            if (!m_inMessage) {
                result.status = Status::Error;
                result.error = "WebSocket continuation without a message";
                return result;
            }
        } else {
            if (m_inMessage) {
                result.status = Status::Error;
                result.error = "WebSocket new message inside a fragmented one";
                return result;
            }
            m_message.clear();
            m_messageIsText = opcode == 0x1;
            m_inMessage = true;
        }
        m_message += payload;
        if (!fin)
            continue;
        m_inMessage = false;
        if (m_messageIsText) {
            result.status = Status::Message;
            result.payload = std::move(m_message);
            m_message.clear();
            return result;
        }
        m_message.clear(); // binary messages are not used
    }
}

std::string buildMaskedFrame(unsigned char opcode, const std::string& payload,
                             const unsigned char mask[4])
{
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
    const std::size_t n = payload.size();
    if (n < 126) {
        frame.push_back(static_cast<char>(0x80 | n));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((n >> 8) & 0xff));
        frame.push_back(static_cast<char>(n & 0xff));
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < n; ++i)
        frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
    return frame;
}

std::string buildPong(const std::string& pingPayload)
{
    static std::random_device device;
    unsigned char mask[4];
    const std::uint32_t r = device();
    for (unsigned i = 0; i < 4; ++i)
        mask[i] = static_cast<unsigned char>(r >> (8 * i));
    return buildMaskedFrame(0xA, pingPayload.substr(0, 125), mask);
}

} // namespace miyoofin
