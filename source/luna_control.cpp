#include "luna_control.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace luna::control
{
namespace
{
void require(bool condition, const char* error)
{
    if (!condition) throw std::invalid_argument(error);
}

void validateUtf8(const std::uint8_t* data, std::size_t size)
{
    for (std::size_t i = 0; i < size;)
    {
        const auto first = data[i++];
        if (first < 0x80) continue;
        const unsigned count = first >= 0xc2 && first <= 0xdf ? 1 :
            first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        require(count != 0 && count <= size - i, "Invalid UTF-8 string");
        std::uint32_t codepoint = first & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
        for (unsigned j = 0; j < count; ++j)
        {
            const auto next = data[i++];
            require((next & 0xc0) == 0x80, "Invalid UTF-8 continuation");
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        require(codepoint >= (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) &&
            codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff),
            "Invalid UTF-8 codepoint");
    }
}

class Writer
{
public:
    Bytes bytes;
    void varint(std::uint64_t value)
    {
        do
        {
            require(bytes.size() < MaxPayloadBytes, "Control payload too large");
            bytes.push_back(static_cast<std::uint8_t>((value & 0x7f) | (value > 0x7f ? 0x80 : 0)));
            value >>= 7;
        } while (value);
    }
    void scalar(std::uint32_t field, std::uint64_t value)
    {
        if (!value) return;
        varint(static_cast<std::uint64_t>(field) << 3);
        varint(value);
    }
    void enumeration(std::uint32_t field, std::uint32_t value)
    {
        require(value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()),
            "Control enum outside supported nonnegative int32 range");
        scalar(field, value);
    }
    void message(std::uint32_t field, const Bytes& body)
    {
        varint((static_cast<std::uint64_t>(field) << 3) | 2);
        varint(body.size());
        require(body.size() <= MaxPayloadBytes - bytes.size(), "Control payload too large");
        bytes.insert(bytes.end(), body.begin(), body.end());
    }
    void string(std::uint32_t field, const std::string& text, bool repeated = false)
    {
        if (text.empty() && !repeated) return;
        require(text.size() <= MaxStringBytes, "Control string too large");
        validateUtf8(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        message(field, Bytes(text.begin(), text.end()));
    }
    void strings(std::uint32_t field, const std::vector<std::string>& texts)
    {
        require(texts.size() <= MaxRepeatedItems, "Too many repeated control fields");
        for (const auto& text : texts) string(field, text, true);
    }
};

struct View { const std::uint8_t* data; std::size_t size; };
class Reader
{
public:
    explicit Reader(View view) : view_(view) {}
    bool empty() const { return offset_ == view_.size; }
    std::uint64_t varint()
    {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 10; ++i)
        {
            require(!empty(), "Truncated control varint");
            const auto byte = view_.data[offset_++];
            require(i != 9 || byte <= 1, "Control varint exceeds uint64");
            value |= static_cast<std::uint64_t>(byte & 0x7f) << (7 * i);
            if (!(byte & 0x80)) return value;
        }
        throw std::invalid_argument("Unterminated control varint");
    }
    std::uint32_t uint32()
    {
        const auto value = varint();
        require(value <= std::numeric_limits<std::uint32_t>::max(), "Control value exceeds uint32");
        return static_cast<std::uint32_t>(value);
    }
    std::int64_t int64()
    {
        const auto bits = varint();
        if (bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return static_cast<std::int64_t>(bits);
        return -1 - static_cast<std::int64_t>(~bits);
    }
    std::uint32_t enumeration()
    {
        const auto value = uint32();
        require(value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()),
            "Control enum outside supported nonnegative int32 range");
        return value;
    }
    View take(std::size_t size)
    {
        require(size <= view_.size - offset_, "Truncated control field");
        const View result{view_.data ? view_.data + offset_ : nullptr, size};
        offset_ += size;
        return result;
    }
    View message() { return take(uint32()); }
    std::string string()
    {
        const auto value = message();
        require(value.size <= MaxStringBytes, "Control string too large");
        validateUtf8(value.data, value.size);
        return value.size ? std::string(reinterpret_cast<const char*>(value.data), value.size) : std::string{};
    }
    void skip(unsigned wire)
    {
        switch (wire)
        {
        case 0: varint(); break;
        case 1: take(8); break;
        case 2: message(); break;
        case 5: take(4); break;
        default: throw std::invalid_argument("Unsupported control wire type");
        }
    }
private:
    View view_;
    std::size_t offset_ = 0;
};

template <typename F>
void fields(View view, F handle)
{
    Reader reader(view);
    while (!reader.empty())
    {
        const auto key = reader.uint32();
        const auto field = key >> 3;
        const auto wire = key & 7;
        require(field != 0, "Invalid control field zero");
        if (!handle(reader, field, wire)) reader.skip(wire);
    }
}
void wireIs(unsigned actual, unsigned expected)
{
    require(actual == expected, "Wrong control field wire type");
}
template <typename T>
void append(std::vector<T>& values, T value)
{
    require(values.size() < MaxRepeatedItems, "Too many repeated control fields");
    values.push_back(std::move(value));
}

Bytes encodeResolution(const Resolution& resolution)
{
    Writer w;
    w.scalar(1, resolution.width); w.scalar(2, resolution.height);
    return std::move(w.bytes);
}
Resolution decodeResolution(View view)
{
    Resolution result;
    fields(view, [&](Reader& r, unsigned field, unsigned wire)
    {
        if (field != 1 && field != 2) return false;
        wireIs(wire, 0);
        (field == 1 ? result.width : result.height) = r.uint32();
        return true;
    });
    return result;
}
SafeZones decodeSafeZones(View view)
{
    SafeZones result;
    fields(view, [&](Reader& r, unsigned field, unsigned wire)
    {
        if (field < 1 || field > 4) return false;
        wireIs(wire, 0);
        std::uint32_t* members[] = {&result.left, &result.right, &result.top, &result.bottom};
        *members[field - 1] = r.uint32();
        return true;
    });
    return result;
}
Display decodeDisplay(View view)
{
    Display result;
    fields(view, [&](Reader& r, unsigned field, unsigned wire)
    {
        switch (field)
        {
        case 1: wireIs(wire, 2); result.resolution = decodeResolution(r.message()); break;
        case 2: wireIs(wire, 0); result.refreshRate = r.uint32(); break;
        case 3: wireIs(wire, 2); result.safeZones = decodeSafeZones(r.message()); break;
        default: return false;
        }
        return true;
    });
    return result;
}
VideoCodec decodeCodec(View view)
{
    VideoCodec result;
    fields(view, [&](Reader& r, unsigned field, unsigned wire)
    {
        if (field < 1 || field > 5) return false;
        wireIs(wire, 0);
        const auto value = field == 1 || field == 4 ? r.enumeration() : r.uint32();
        switch (field)
        {
        case 1: result.codec = static_cast<Codec>(value); break;
        case 2: result.profile = value; break;
        case 3: result.level = value; break;
        case 4: result.tier = static_cast<Tier>(value); break;
        case 5: result.maxReferenceFrames = value; break;
        }
        return true;
    });
    return result;
}
DisplayConfiguration decodeDisplayConfiguration(View view)
{
    DisplayConfiguration result;
    fields(view, [&](Reader& r, unsigned field, unsigned wire)
    {
        switch (field)
        {
        case 1: wireIs(wire, 0); result.negotiationId = r.varint(); break;
        case 2: wireIs(wire, 2); result.captureResolution = decodeResolution(r.message()); break;
        case 3: wireIs(wire, 2); result.display = decodeDisplay(r.message()); break;
        case 4: wireIs(wire, 2); append(result.codecs, decodeCodec(r.message())); break;
        case 5: wireIs(wire, 2); append(result.features, r.string()); break;
        case 6: wireIs(wire, 0); result.minBitsPerSecond = r.uint32(); break;
        case 7: wireIs(wire, 0); result.maxBitsPerSecond = r.uint32(); break;
        case 8: wireIs(wire, 0); result.startBitsPerSecond = r.uint32(); break;
        case 9: wireIs(wire, 0); result.clampingMode = static_cast<ClampingMode>(r.enumeration()); break;
        default: return false;
        }
        return true;
    });
    return result;
}
BrowserMetadata decodeMetadata(View view)
{
    BrowserMetadata result;
    fields(view, [&](Reader& r, unsigned field, unsigned wire)
    {
        if (field < 1 || field > 6) return false;
        wireIs(wire, 2);
        std::string* members[] = {&result.browserType, &result.browserVersion,
            &result.deviceModel, &result.deviceType, &result.osName, &result.osVersion};
        *members[field - 1] = r.string();
        return true;
    });
    return result;
}
bool knownEvent(unsigned field)
{
    switch (field)
    {
    case 10: case 11: case 12: case 13: case 20: case 30: case 40: case 41: case 42:
    case 43: case 50: case 51: case 52: case 53: case 54: case 60: case 61: case 70:
    case 71: case 100: case 110: return true;
    default: return false;
    }
}
Body decodeEvent(unsigned event, View view)
{
    if (event == 20)
    {
        Ping result;
        fields(view, [&](Reader& r, unsigned field, unsigned wire)
        {
            if (field < 1 || field > 3) return false;
            wireIs(wire, 0);
            std::int64_t* members[] = {&result.clientSendTimestampMs,
                &result.serverReceiveTimestampMs, &result.serverSendTimestampMs};
            *members[field - 1] = r.int64();
            return true;
        });
        return result;
    }
    if (event == 30)
    {
        FeatureConfiguration result;
        fields(view, [&](Reader& r, unsigned field, unsigned wire)
        {
            if (field != 1) return false;
            wireIs(wire, 2); append(result.features, r.string()); return true;
        });
        return result;
    }
    if (event == 40) return decodeDisplayConfiguration(view);
    if (event == 42)
    {
        ClientContext result;
        fields(view, [&](Reader& r, unsigned field, unsigned wire)
        {
            if (field != 1) return false;
            wireIs(wire, 2); result.browserMetadata = decodeMetadata(r.message()); return true;
        });
        return result;
    }
    if (event == 10)
    {
        CloseConnectionRequest result;
        fields(view, [&](Reader& r, unsigned field, unsigned wire)
        {
            switch (field)
            {
            case 1: wireIs(wire, 2); result.sessionId = r.string(); break;
            case 2: wireIs(wire, 0); result.connectionId = r.uint32(); break;
            case 3: wireIs(wire, 0); result.closingReason = r.enumeration(); break;
            default: return false;
            }
            return true;
        });
        return result;
    }
    if (event == 11)
    {
        IdleTimeoutWarning result;
        fields(view, [&](Reader& r, unsigned field, unsigned wire)
        {
            if (field != 1 && field != 2) return false;
            wireIs(wire, 0);
            (field == 1 ? result.connectionId : result.warnTime) = r.uint32();
            return true;
        });
        return result;
    }
    if (event == 12 || event == 13)
    {
        fields(view, [](Reader&, unsigned, unsigned) { return false; });
        if (event == 12) return ResetIdleTimeoutRequest{};
        return AckCloseConnectionRequest{};
    }
    return UnhandledMessage{event, Bytes(view.data, view.data + view.size)};
}
Bytes envelope(std::uint32_t field, const Writer& body)
{
    Writer w; w.message(field, body.bytes); return std::move(w.bytes);
}
}

Bytes encodeFeatureConfiguration(const FeatureConfiguration& configuration)
{
    Writer w; w.strings(1, configuration.features); return envelope(30, w);
}
Bytes encodeDisplayConfiguration(const DisplayConfiguration& configuration)
{
    Writer w;
    w.scalar(1, configuration.negotiationId);
    if (configuration.captureResolution) w.message(2, encodeResolution(*configuration.captureResolution));
    if (configuration.display)
    {
        Writer screen;
        const auto& display = *configuration.display;
        if (display.resolution) screen.message(1, encodeResolution(*display.resolution));
        screen.scalar(2, display.refreshRate);
        if (display.safeZones)
        {
            Writer zones;
            zones.scalar(1, display.safeZones->left); zones.scalar(2, display.safeZones->right);
            zones.scalar(3, display.safeZones->top); zones.scalar(4, display.safeZones->bottom);
            screen.message(3, zones.bytes);
        }
        w.message(3, screen.bytes);
    }
    require(configuration.codecs.size() <= MaxRepeatedItems, "Too many control codecs");
    for (const auto& codec : configuration.codecs)
    {
        Writer entry;
        entry.enumeration(1, static_cast<std::uint32_t>(codec.codec));
        entry.scalar(2, codec.profile); entry.scalar(3, codec.level);
        entry.enumeration(4, static_cast<std::uint32_t>(codec.tier));
        entry.scalar(5, codec.maxReferenceFrames);
        w.message(4, entry.bytes);
    }
    w.strings(5, configuration.features);
    w.scalar(6, configuration.minBitsPerSecond); w.scalar(7, configuration.maxBitsPerSecond);
    w.scalar(8, configuration.startBitsPerSecond);
    w.enumeration(9, static_cast<std::uint32_t>(configuration.clampingMode));
    return envelope(40, w);
}
Bytes encodeClientContext(const BrowserMetadata& metadata)
{
    Writer w, browser;
    browser.string(1, metadata.browserType); browser.string(2, metadata.browserVersion);
    browser.string(3, metadata.deviceModel); browser.string(4, metadata.deviceType);
    browser.string(5, metadata.osName); browser.string(6, metadata.osVersion);
    w.message(1, browser.bytes);
    return envelope(42, w);
}
Bytes encodePing(const Ping& ping)
{
    Writer w;
    w.scalar(1, static_cast<std::uint64_t>(ping.clientSendTimestampMs));
    w.scalar(2, static_cast<std::uint64_t>(ping.serverReceiveTimestampMs));
    w.scalar(3, static_cast<std::uint64_t>(ping.serverSendTimestampMs));
    return envelope(20, w);
}
Bytes encodeCloseAck() { return envelope(13, {}); }
Bytes encodeResetIdleTimeout() { return envelope(12, {}); }

Message decodePayload(const std::uint8_t* data, std::size_t size)
{
    require(size <= MaxPayloadBytes && (data || size == 0), "Invalid control payload size/pointer");
    Message result;
    fields({data, size}, [&](Reader& r, unsigned field, unsigned wire)
    {
        if (field == 2)
        {
            wireIs(wire, 0); result.profilingMarkerIdentifier = r.uint32(); return true;
        }
        if (!knownEvent(field)) return false;
        wireIs(wire, 2);
        result.body = decodeEvent(field, r.message()); // Protobuf oneof: last recognized member wins.
        return true;
    });
    return result;
}
Message decodeServerFrame(const std::uint8_t* data, std::size_t size)
{
    require(data && size >= 16 && size <= MaxPayloadBytes + 16, "Invalid control frame size/pointer");
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i) length |= static_cast<std::uint32_t>(data[8 + i]) << (8 * i);
    require(length <= MaxPayloadBytes && length <= size - 16, "Truncated/oversized control frame payload");
    // Other prefix bytes and bounded trailing bytes are opaque, as in the browser handler.
    return decodePayload(data + 16, length);
}
}
