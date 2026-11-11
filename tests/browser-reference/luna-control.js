"use strict";

(function (root) {
    const maxPayload = 65536, maxString = 4096, maxItems = 128;
    const utf8 = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
    function bytes(data) {
        if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        if (data instanceof ArrayBuffer) return new Uint8Array(data);
        throw new Error("Expected binary control data");
    }
    class Reader {
        constructor(data) { this.data = data; this.offset = 0; }
        varint() {
            let value = 0n;
            for (let i = 0; i < 10; ++i) {
                if (this.offset === this.data.length) throw new Error("Truncated control varint");
                const byte = this.data[this.offset++];
                if (i === 9 && byte > 1) throw new Error("Control varint overflow");
                value |= BigInt(byte & 127) << BigInt(7 * i);
                if (!(byte & 128)) return value;
            }
            throw new Error("Unterminated control varint");
        }
        uint32() {
            const value = this.varint();
            if (value > 0xffffffffn) throw new Error("Control uint32 overflow");
            return Number(value);
        }
        take(size) {
            if (size > this.data.length - this.offset) throw new Error("Truncated control field");
            const value = this.data.subarray(this.offset, this.offset + size);
            this.offset += size;
            return value;
        }
        message() { return this.take(this.uint32()); }
        string() {
            const value = this.message();
            if (value.length > maxString) throw new Error("Control string too large");
            return utf8.decode(value);
        }
        skip(wire) {
            if (wire === 0) this.varint();
            else if (wire === 1) this.take(8);
            else if (wire === 2) this.message();
            else if (wire === 5) this.take(4);
            else throw new Error("Unsupported control wire type");
        }
    }
    function fields(data, handlers) {
        const r = new Reader(data);
        while (r.offset < data.length) {
            const tag = r.uint32(), field = tag >>> 3, wire = tag & 7;
            if (!field) throw new Error("Control field zero");
            const handler = handlers[field];
            if (!handler) r.skip(wire);
            else {
                if (handler[0] !== wire) throw new Error(`Wrong wire type for control field ${field}`);
                handler[1](r);
            }
        }
    }
    function scalarFields(definitions) {
        const value = {}, handlers = {};
        for (const [field, name, type = "uint32"] of definitions) {
            value[name] = type === "string" ? "" : type === "int64" || type === "uint64" ? 0n : 0;
            handlers[field] = [type === "string" ? 2 : 0, r => {
                value[name] = type === "string" ? r.string() : type === "int64" ?
                    BigInt.asIntN(64, r.varint()) : type === "uint64" ? r.varint() : r.uint32();
            }];
        }
        return { value, handlers };
    }
    function scalarMessage(data, definitions) {
        const { value, handlers } = scalarFields(definitions);
        fields(data, handlers);
        return value;
    }
    function append(list, value) {
        if (list.length >= maxItems) throw new Error("Too many repeated control fields");
        list.push(value);
    }
    function display(data) {
        const { value, handlers } = scalarFields([[1, "negotiationId", "uint64"],
            [6, "minBitsPerSecond"], [7, "maxBitsPerSecond"], [8, "startBitsPerSecond"], [9, "clampingMode"]]);
        Object.assign(value, { display: null, captureResolution: null, codecs: [], features: [] });
        const resolution = data => scalarMessage(data, [[1, "width"], [2, "height"]]);
        handlers[2] = [2, r => { value.captureResolution = resolution(r.message()); }];
        handlers[3] = [2, r => {
            const screen = scalarFields([[2, "refreshRate"]]);
            Object.assign(screen.value, { resolution: null, safeZones: null });
            screen.handlers[1] = [2, r => { screen.value.resolution = resolution(r.message()); }];
            screen.handlers[3] = [2, r => { screen.value.safeZones = scalarMessage(r.message(),
                [[1, "left"], [2, "right"], [3, "top"], [4, "bottom"]]); }];
            fields(r.message(), screen.handlers);
            value.display = screen.value;
        }];
        handlers[4] = [2, r => append(value.codecs, scalarMessage(r.message(), [[1, "videoCodec"],
            [2, "profile"], [3, "level"], [4, "tier"], [5, "maxReferenceFramesNum"]]))];
        handlers[5] = [2, r => append(value.features, r.string())];
        fields(data, handlers);
        return value;
    }
    function decode(data) {
        data = bytes(data);
        if (data.length > maxPayload) throw new Error("Control payload too large");
        let result = { kind: "empty", input: {} };
        const handlers = {};
        const handler = (field, kind, parse) => {
            handlers[field] = [2, r => { result = { kind, input: parse(r.message()) }; }];
        };
        handler(20, "ping", data => scalarMessage(data, [[1, "clientSendTimestampMs", "int64"],
            [2, "serverReceiveTimestampMs", "int64"], [3, "serverSendTimestampMs", "int64"]]));
        handler(30, "feature", data => {
            const features = [];
            fields(data, { 1: [2, r => append(features, r.string())] });
            return { features };
        });
        handler(40, "display", display);
        handler(42, "context", data => {
            let metadata = null;
            fields(data, { 1: [2, r => { metadata = scalarMessage(r.message(), [[1, "browserType", "string"],
                [2, "browserVersion", "string"], [3, "deviceModel", "string"], [4, "deviceType", "string"],
                [5, "osName", "string"], [6, "osVersion", "string"]]); }] });
            return { browserMetadata: metadata };
        });
        handler(10, "close", data => scalarMessage(data, [[1, "sessionId", "string"],
            [2, "connectionId"], [3, "closingReason"]]));
        for (const [field, kind] of [[12, "resetIdle"], [13, "ackClose"]])
            handler(field, kind, data => { fields(data, {}); return {}; });
        fields(data, handlers);
        return result;
    }
    function varint(value) {
        value = BigInt(value);
        if (value < 0n || value > 0xffffffffffffffffn) throw new Error("Varint outside uint64");
        const result = [];
        do {
            result.push(Number(value & 127n) | (value > 127n ? 128 : 0));
            value >>= 7n;
        } while (value);
        return result;
    }
    function nested(field, body) {
        return [...varint((field << 3) | 2), ...varint(body.length), ...body];
    }
    function encodePing(ping) {
        const body = [];
        for (const [field, name] of [[1, "clientSendTimestampMs"], [2, "serverReceiveTimestampMs"],
            [3, "serverSendTimestampMs"]]) {
            const value = BigInt(ping[name] || 0);
            if (value < -0x8000000000000000n || value > 0x7fffffffffffffffn) throw new Error("Ping int64 overflow");
            if (value) body.push(...varint(field << 3), ...varint(BigInt.asUintN(64, value)));
        }
        return new Uint8Array(nested(20, body));
    }
    function encodeCloseRequest() {
        return new Uint8Array(nested(10, [...nested(1, new TextEncoder().encode("local-test")), 0x10, 1]));
    }
    function frame(payload) {
        payload = bytes(payload);
        if (payload.length > maxPayload) throw new Error("Control frame too large");
        const result = new Uint8Array(16 + payload.length);
        result.fill(0xa5, 0, 16); // Opaque test prefix, deliberately not all zeros.
        new DataView(result.buffer).setUint32(8, payload.length, true);
        result.set(payload, 16);
        return result;
    }

    class LocalPeer {
        constructor({ send, changed = () => {}, acknowledged = () => {}, now = () => performance.now() }) {
            this.send = send; this.changed = changed; this.acknowledged = acknowledged; this.now = now;
            this.state = { open: false, stopped: false, stages: [], displayEchoes: 0,
                pings: 0, closeRequested: false, closeAck: false, native: null };
        }
        open() {
            if (this.state.stopped) return;
            this.state.open = true;
            this.changed(this.state);
        }
        receive(data) {
            if (this.state.stopped) return;
            const started = this.now(), message = decode(data);
            const { kind, input } = message;
            if (["feature", "display", "context"].includes(kind)) {
                const expected = ["feature", "display", "context"][this.state.stages.length];
                if (kind !== expected) throw new Error(`Startup order: expected ${expected}, received ${kind}`);
                if (kind === "feature" && JSON.stringify(input.features) !==
                    JSON.stringify(["server-reconfigure", "client-display-config-flag"]))
                    throw new Error("Unexpected initial feature list");
                if (kind === "display") {
                    this.state.display = input;
                    this.send(frame(data)); // Explicit synthetic echo, not server negotiation.
                    this.state.displayEchoes++;
                }
                if (kind === "context") this.state.context = input;
                this.state.stages.push(kind);
            } else if (kind === "ping" && !this.state.closeRequested) {
                if (input.clientSendTimestampMs <= 0n || input.clientSendTimestampMs > 9007199254739991n)
                    throw new Error("Ping timestamp outside test clock range");
                const serverReceive = input.clientSendTimestampMs + 1000n;
                const elapsed = BigInt(Math.max(0, Math.trunc(this.now() - started)));
                this.send(frame(encodePing({ clientSendTimestampMs: input.clientSendTimestampMs,
                    serverReceiveTimestampMs: serverReceive, serverSendTimestampMs: serverReceive + elapsed })));
                this.state.pings++;
            } else if (kind === "ackClose" && this.state.closeRequested) {
                this.state.closeAck = true;
                this.changed(this.state);
                this.acknowledged();
                return;
            } else if (kind !== "ping") throw new Error(`Unexpected local control message: ${kind}`);
            this.changed(this.state);
        }
        noteNativeReport(report) {
            const numbers = ["startupMessages", "offsetMs", "delayMs", "pingsSent", "pingsAccepted",
                "invalidFrames", "droppedFrames", "sendErrors"];
            const flags = ["displayReply", "synchronized", "closing", "closeAckSent", "shouldClosePeer"];
            if (report.type !== "lunanx-control-status" || numbers.some(k => !Number.isFinite(report[k])) ||
                flags.some(k => typeof report[k] !== "boolean")) throw new Error("Invalid native control report");
            this.state.native = report;
            this.changed(this.state);
        }
        requestClose() {
            if (!this.state.open || this.state.stopped || this.state.stages.length !== 3 || this.state.closeRequested)
                throw new Error("Control startup is not ready for a close request");
            this.state.closeRequested = true;
            try { this.send(frame(encodeCloseRequest())); }
            catch (error) { this.state.closeRequested = false; throw error; }
            this.changed(this.state);
        }
        stop() {
            if (this.state.stopped) return;
            this.state.stopped = true;
            this.state.open = false;
            this.changed(this.state);
        }
    }
    const api = { decode, encodePing, encodeCloseRequest, frame, LocalPeer };
    if (typeof module !== "undefined" && module.exports) module.exports = api;
    else root.LunaControl = api;
})(globalThis);
