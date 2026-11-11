"use strict";

// Decoder for the observed input subset. No server-to-client 16-byte header.
// Also exported for offline checks against the original JavaScript writer.
(function (root) {
    class Reader {
        constructor(bytes) { this.bytes = bytes; this.offset = 0; }
        varint() {
            let value = 0n;
            for (let i = 0; i < 10; ++i) {
                if (this.offset === this.bytes.length) throw new Error("Truncated varint");
                const byte = this.bytes[this.offset++];
                if (i === 9 && byte > 1) throw new Error("Varint exceeds 64 bits");
                value |= BigInt(byte & 127) << BigInt(i * 7);
                if (!(byte & 128)) return value;
            }
            throw new Error("Unterminated varint");
        }
        uint32() {
            const value = this.varint();
            if (value > 0xffffffffn) throw new Error("Value exceeds uint32");
            return Number(value);
        }
        take(length) {
            if (length > this.bytes.length - this.offset) throw new Error("Truncated field");
            const bytes = this.bytes.subarray(this.offset, this.offset + length);
            this.offset += length;
            return bytes;
        }
        message() { return this.take(this.uint32()); }
        skip(wire) {
            if (wire === 0) this.varint();
            else if (wire === 1) this.take(8);
            else if (wire === 2) this.message();
            else if (wire === 5) this.take(4);
            else throw new Error(`Unsupported wire type ${wire}`);
        }
    }

    function fields(bytes, handlers) {
        const reader = new Reader(bytes);
        while (reader.offset < bytes.length) {
            const key = reader.uint32(), field = key >>> 3, wire = key & 7;
            if (field === 0) throw new Error("Invalid field number zero");
            const handler = handlers[field];
            if (!handler) reader.skip(wire);
            else {
                if (wire !== handler[0]) throw new Error(`Wrong wire type for field ${field}`);
                handler[1](reader);
            }
        }
    }

    function scalars(definitions) {
        const result = {}, handlers = {};
        for (const [field, name, type = "uint32"] of definitions) {
            result[name] = type === "int64" ? "0" : 0;
            handlers[field] = [0, reader => {
                if (type === "int64") result[name] = BigInt.asIntN(64, reader.varint()).toString();
                else {
                    const value = reader.uint32();
                    result[name] = type === "sint32" ? (value >>> 1) ^ -(value & 1) : value;
                }
            }];
        }
        return { result, handlers };
    }

    function decodeGamepad(bytes) {
        const { result, handlers } = scalars([
            [1, "deviceId"], [2, "packetNumber"], [3, "buttons"],
            [4, "leftTrigger"], [5, "rightTrigger"],
            [6, "leftX"], [7, "leftY"], [8, "rightX"], [9, "rightY"],
            [11, "type"], [13, "leftSensitivity", "sint32"], [14, "rightSensitivity", "sint32"],
        ]);
        result.timing = null;
        handlers[12] = [2, reader => {
            const nested = reader.message();
            const latency = scalars([
                [1, "timestampMs", "int64"], [3, "hardwareClass"], [8, "captureTimestampMs", "int64"],
            ]);
            fields(nested, latency.handlers);
            result.timing = latency.result;
        }];
        fields(bytes, handlers);
        if (result.leftTrigger > 255 || result.rightTrigger > 255 ||
            [result.leftX, result.leftY, result.rightX, result.rightY].some(value => value > 65534))
            throw new Error("Controller values outside the observed range");
        if (result.type > 1) throw new Error("Unknown gamepad injection type");
        return result;
    }

    function decodeConnection(bytes) {
        const { result, handlers } = scalars([[1, "deviceId"], [2, "status"], [10, "type"]]);
        fields(bytes, handlers);
        if (result.status > 1 || result.type > 1) throw new Error("Unknown connection status/type");
        return { deviceId: result.deviceId, connected: result.status === 0, type: result.type };
    }

    function decode(data) {
        let bytes;
        if (ArrayBuffer.isView(data)) bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        else if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
        else throw new Error("Expected a binary input message");
        if (!bytes.length || bytes.length > 1024) throw new Error("Invalid input message size");
        let result = null, resiliency = null;
        const event = (kind, value) => {
            if (result) throw new Error("Multiple input events in one message");
            result = { kind, ...value };
        };
        fields(bytes, {
            1: [2, reader => {
                const nested = reader.message();
                const attributes = scalars([[10, "category"], [20, "eventId"],
                    [30, "keyframeId"], [40, "terminalIdentifier"], [50, "retransmitCount"]]);
                fields(nested, attributes.handlers);
                resiliency = attributes.result;
            }],
            36: [2, reader => { fields(reader.message(), {}); event("ack", {}); }],
            104: [2, reader => event("gamepad", decodeGamepad(reader.message()))],
            105: [2, reader => event("connection", decodeConnection(reader.message()))],
        });
        if (!result) throw new Error("No supported input event");
        return { ...result, resiliency, bytes: bytes.length };
    }

    const api = { decode };
    if (typeof module !== "undefined" && module.exports) module.exports = api;
    else root.LunaInput = api;
})(globalThis);
