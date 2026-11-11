const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const Control = require('../browser-reference/luna-control.js');
const normalize = value => JSON.parse(JSON.stringify(value, (_, v) => typeof v === 'bigint' ? Number(v) : v));

module.exports = function checkLocalPeer({ run, root, temporary, reference }) {
    for (const example of reference.examples) {
        const data = Buffer.from(example.expectedHex, 'hex'), decoded = Control.decode(data);
        assert.equal(decoded.kind, example.kind, example.name);
        const actual = normalize(decoded.input), input = example.input;
        if (example.kind === 'feature') assert.deepEqual(actual, input);
        if (example.kind === 'ping') {
            assert.deepEqual(actual, { clientSendTimestampMs: input.clientSendTimestampMs || 0,
                serverReceiveTimestampMs: input.serverReceiveTimestampMs || 0,
                serverSendTimestampMs: input.serverSendTimestampMs || 0 });
            assert.equal(Buffer.from(Control.encodePing(input)).toString('hex'), example.expectedHex);
        }
        if (example.kind === 'context') {
            assert.deepEqual(actual.browserMetadata, Object.fromEntries(['browserType', 'browserVersion',
                'deviceModel', 'deviceType', 'osName', 'osVersion'].map(k => [k, input[k] || ''])));
        }
        if (example.kind === 'display') {
            assert.equal(actual.negotiationId, input.negotiationId);
            assert.deepEqual(actual.display, { resolution: { width: input.width, height: input.height },
                refreshRate: input.refreshRate, safeZones: input.safeZones });
            assert.deepEqual(actual.codecs, input.codecs.map(c => ({ ...c, tier: c.tier || 0 })));
            for (const field of ['features', 'minBitsPerSecond', 'maxBitsPerSecond', 'startBitsPerSecond', 'clampingMode'])
                assert.deepEqual(actual[field], input[field]);
        }
    }
    for (const hex of ['00', 'a201', 'a2010b08ffffffffffffffffff02', 'f201020a80',
        'a00100', 'f201040a02c080']) assert.throws(() => Control.decode(Buffer.from(hex, 'hex')));
    assert.throws(() => Control.decode(new Uint8Array(65537)));
    assert.throws(() => Control.encodePing({ clientSendTimestampMs: 1n << 63n }));
    assert.equal(Control.decode(Buffer.from('f201050a03efbbbf', 'hex')).input.features[0], '\ufeff', 'UTF-8 BOM retained');

    const executable = path.join(temporary, 'session-probe');
    run(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-pthread',
        '-Isource', 'source/luna_control.cpp', 'source/luna_time_sync.cpp', 'source/local_control_session.cpp',
        'tests/luna-control/session_probe.cpp', '-o', executable]);
    const probe = commands => run(executable, [], { input: commands.join('\n') + '\n' }).trim().split('\n');
    const outgoing = lines => lines.filter(x => x.startsWith('OUT ')).map(x => Buffer.from(x.slice(4), 'hex'));
    const statuses = lines => lines.filter(x => x.startsWith('STATUS ')).map(x => JSON.parse(x.slice(7)));
    const start = ['tick 0 1 0', 'tick 16 1 0', 'tick 32 1 0', 'tick 48 1 0'];
    const sent = outgoing(probe(start));
    const liveSent = outgoing(run(executable, ['live'], { input: start.join('\n') + '\n' }).trim().split('\n'))
        .map(packet => Control.decode(packet));
    assert.deepEqual(liveSent.map(packet => packet.kind), ['feature', 'display', 'context', 'ping']);
    assert.equal(liveSent[1].input.display.resolution.width, 1280);
    assert.equal(liveSent[1].input.display.refreshRate, 30);
    assert.equal(liveSent[1].input.minBitsPerSecond, 4000000);
    assert.equal(liveSent[1].input.maxBitsPerSecond, 8000000);
    assert.equal(liveSent[2].input.browserMetadata.osVersion, 'homebrew');
    assert.deepEqual(sent.map(x => Control.decode(x).kind), ['feature', 'display', 'context', 'ping']);
    const replies = [];
    let acknowledged = false;
    const peer = new Control.LocalPeer({ send: data => replies.push(Buffer.from(data)), now: () => 0,
        acknowledged: () => { acknowledged = true; } });
    peer.open();
    for (const data of sent) peer.receive(data);
    assert.equal(peer.state.stages.length, 3);
    assert.equal(replies.length, 2);
    const frameCommand = data => `frame ${data.toString('hex')}`;
    const transcript = [...start, frameCommand(replies[0]), frameCommand(replies[1]), 'tick 68 1 0'];
    const report = statuses(probe(transcript)).at(-1);
    assert.equal(report.startupMessages, 3);
    assert.equal(report.displayReply, true);
    assert.equal(report.synchronized, true);
    assert.equal(report.offsetMs, 990);
    assert.equal(report.delayMs, 20);
    peer.noteNativeReport(report);
    assert.equal(peer.state.native.offsetMs + peer.state.native.delayMs / 2, 1000);
    peer.requestClose();
    const closeFrame = replies.at(-1);
    const closing = probe([...transcript, frameCommand(closeFrame), 'tick 80 1 0']);
    assert.equal(statuses(closing).at(-1).closeAckSent, true);
    peer.receive(outgoing(closing).at(-1));
    assert.equal(acknowledged, true);
    peer.stop();
    const replyCount = replies.length;
    peer.receive(sent.at(-1));
    assert.equal(replies.length, replyCount, 'Stopped peer emits no replies');
    const closed = statuses(probe([...transcript, frameCommand(closeFrame), 'tick 80 1 0', 'tick 96 0 0'])).at(-1);
    assert.equal(closed.shouldClosePeer, true);
    assert.equal(closed.synchronized, false);
    const fallback = statuses(probe([...transcript, frameCommand(closeFrame), 'tick 80 1 0', 'tick 1080 1 0'])).at(-1);
    assert.equal(fallback.shouldClosePeer, true, 'ACK close timeout bounded');
    const blocked = statuses(probe([...transcript, frameCommand(closeFrame), 'tick 80 1 1', 'tick 2080 1 1'])).at(-1);
    assert.equal(blocked.shouldClosePeer, true, 'Blocked close bounded');
    assert.equal(blocked.closeAckSent, false);
    const retry = probe(['fail', 'tick 0 1 0', 'tick 16 1 0', 'tick 32 1 1', 'tick 48 1 0']);
    assert.equal(statuses(retry)[0].sendErrors, 1);
    assert.deepEqual(outgoing(retry).map(x => Control.decode(x).kind), ['feature', 'display']);
    const invalid = statuses(probe(['frame 00', 'tick 0 1 0'])).at(-1);
    assert.equal(invalid.invalidFrames, 1);
    const overflow = probe(['flood 20 00', 'tick 0 1 0']);
    assert.ok(overflow.includes('QUEUED 16'));
    assert.equal(statuses(overflow).at(-1).droppedFrames, 4);
    assert.equal(statuses(overflow).at(-1).invalidFrames, 16);
    const bigFrame = Buffer.alloc(65552).toString('hex');
    const byteBound = probe([`flood 3 ${bigFrame}`, 'tick 0 1 0']);
    assert.ok(byteBound.includes('QUEUED 2'));
    assert.equal(statuses(byteBound).at(-1).droppedFrames, 1);
    const stopped = probe(['stop', frameCommand(replies[0]), 'tick 1 1 0']);
    assert.ok(stopped.includes('QUEUED 0'));
    assert.equal(outgoing(stopped).length, 0);

    // Exercise actual browser routing with control arriving before the input channel.
    const elements = new Map(), timers = new Map();
    const element = () => ({ textContent: '', value: '', checked: false, disabled: false,
        classList: { toggle() {} }, setAttribute() {}, appendChild() {},
        addEventListener(name, fn) { this[name] = fn; } });
    const byId = id => { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); };
    byId('computer-ip').value = '192.0.2.1';
    byId('offer').value = 'v=0\r\n';
    let connection, timerId = 0;
    class FakePeer {
        constructor() { connection = this; this.iceGatheringState = 'complete'; }
        async setRemoteDescription() {}
        getTransceivers() { return []; }
        async createAnswer() { return { type: 'answer', sdp: 'v=0\r\na=candidate:1 1 UDP 1 192.0.2.1 9000 typ host\r\n' }; }
        async setLocalDescription(answer) { this.localDescription = answer; }
        close() { this.connectionState = 'closed'; this.onconnectionstatechange?.(); }
    }
    const context = vm.createContext({ console, TextDecoder, TextEncoder, LunaControl: Control,
        document: { getElementById: byId, createElement: element },
        window: { addEventListener() {}, RTCPeerConnection: FakePeer }, RTCPeerConnection: FakePeer,
        performance: { now: () => 0 },
        setInterval: fn => { timers.set(++timerId, fn); return timerId; }, clearInterval: id => timers.delete(id),
        fetch: async () => ({ ok: true }),
    });
    vm.runInContext(fs.readFileSync(path.join(root, 'tests/browser-reference/client.js'), 'utf8'), context);
    return (async () => {
        await byId('create').click();
        const fakeChannel = label => ({ label, readyState: 'open', sent: [], send(data) { this.sent.push(data); }, close() {} });
        const control = fakeChannel('clientMessageChannel'), input = fakeChannel('lunanx-test');
        connection.ondatachannel({ channel: control });
        connection.ondatachannel({ channel: input });
        assert.equal(control.sent.length, 0, 'Hello must not be sent on control channel');
        assert.deepEqual(input.sent, ['Hello from Chromium!']);
        for (const data of sent) control.onmessage({ data });
        input.onmessage({ data: JSON.stringify(report) });
        assert.match(byId('control-status').textContent, /Control test passed/);
        assert.equal(byId('control-close').disabled, false);
        byId('control-close').click();
        control.onmessage({ data: Buffer.from('6a00', 'hex') });
        assert.match(byId('control-status').textContent, /Close acknowledged/);
        assert.equal(byId('create').disabled, false);
        assert.equal(timers.size, 0, 'Browser close cancels test intervals');
        console.log('PASS: browser control reference decoding; native↔browser startup, framed replies, clock ' +
            'anchor, close ACK/fallback, mailbox bounds, send recovery, and channel routing.');
    })();
};
