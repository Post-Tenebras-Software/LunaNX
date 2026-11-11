const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const LunaInput = require('../browser-reference/luna-input.js');

module.exports = function checkLocalInput({ run, root, temporary, reference }) {
    for (const test of reference.cases) {
        const message = LunaInput.decode(Buffer.from(test.expectedHex, 'hex'));
        assert.equal(message.kind, test.kind, test.name);
        assert.deepEqual(message.resiliency, test.resiliency || null, test.name);
        if (test.kind === 'gamepad') {
            const { kind, timing, resiliency, bytes, ...state } = message;
            assert.deepEqual(state, test.state, test.name);
            assert.deepEqual(timing, { timestampMs: String(test.timing.timestampMs),
                captureTimestampMs: String(test.timing.timestampMs),
                hardwareClass: test.timing.hardwareClass }, test.name);
        } else if (test.kind === 'connection') {
            assert.equal(message.deviceId, test.deviceId, test.name);
            assert.equal(message.connected, test.connected, test.name);
            assert.equal(message.type, test.type, test.name);
        }
    }
    const frame = body => Buffer.from([0xc2, 6, body.length, ...body]);
    const malformed = [Buffer.alloc(0), Buffer.alloc(1025), Buffer.from([0]),
        Buffer.from([0xc2, 6]), Buffer.from([0xc0, 6, 0]),
        Buffer.from([0xc2, 6, 2, 0x30]),
        frame([0x30, 0x80]), frame([0x30, ...Array(9).fill(255), 2]),
        frame([0x30, 0x80, 0x80, 0x80, 0x80, 0x10]),
        frame([0x22, 0]), frame([0x20, 0x80, 2]), frame([0x30, 255, 255, 3]),
        frame([0x58, 2]), frame([0x62, 1, 8]),
        Buffer.from('a20200ca0600', 'hex')];
    for (const data of malformed) assert.throws(() => LunaInput.decode(data));
    assert.equal(LunaInput.decode(Buffer.from('a20200980601', 'hex')).kind, 'ack', 'Unknown field skipped');
    assert.equal(LunaInput.decode(Buffer.from('a20200c10c0000000000000000c50c00000000', 'hex')).kind,
        'ack', 'Unknown fixed-width fields skipped');
    // Views can cover only part of a larger receive buffer.
    assert.equal(LunaInput.decode(new Uint8Array([99, 0xa2, 2, 0, 99]).subarray(1, 4)).kind, 'ack');

    const executable = path.join(temporary, 'controller-probe');
    run(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-pthread',
        '-Itests/luna-input/stubs', '-Isource', 'source/luna_input.cpp', 'source/controller_input.cpp',
        'tests/luna-input/controller_probe.cpp', '-o', executable]);
    const packets = new Map(run(executable, []).split('\n').filter(line => line.startsWith('TEST '))
        .map(line => { const [, name, ...hex] = line.split(' ');
            return [name, hex.map(value => LunaInput.decode(Buffer.from(value, 'hex')))]; }));
    const get = name => { assert.ok(packets.has(name), name); return packets.get(name); };
    const oneState = name => {
        const messages = get(name);
        assert.equal(messages.length, 1, name);
        assert.equal(messages[0].kind, 'gamepad', name);
        return messages[0];
    };
    const neutral = message => {
        assert.equal(message.kind, 'gamepad');
        assert.equal(message.buttons, 0);
        assert.equal(message.leftTrigger, 0);
        assert.equal(message.rightTrigger, 0);
        assert.deepEqual([message.leftX, message.leftY, message.rightX, message.rightY], Array(4).fill(32767));
    };
    const connectThenState = name => {
        const messages = get(name);
        assert.equal(messages.length, 2, name);
        assert.equal(messages[0].kind, 'connection', name);
        assert.equal(messages[0].connected, true, name);
        assert.equal(messages[1].kind, 'gamepad', name);
        return messages[1];
    };
    neutral(connectThenState('initial'));
    assert.equal(connectThenState('server-clock').timing.timestampMs, '1794000000123');
    assert.equal(get('initial')[1].packetNumber, 0);
    // Match Xbox positions: bottom=B, right=A, left=Y, top=X on Switch.
    const mapping = { A: 0x2000, B: 0x1000, X: 0x8000, Y: 0x4000, LS: 0x40, RS: 0x80,
        L: 0x100, R: 0x200, ZL: 0, ZR: 0, Plus: 0x10, Minus: 0x20,
        Left: 4, Up: 1, Right: 8, Down: 2 };
    for (const [name, mask] of Object.entries(mapping)) {
        const state = oneState(name);
        assert.equal(state.buttons, mask, name);
        assert.equal(state.leftTrigger, name === 'ZL' ? 255 : 0, name);
        assert.equal(state.rightTrigger, name === 'ZR' ? 255 : 0, name);
    }
    const all = oneState('all-held');
    assert.equal(all.buttons, 0xf3ff);
    assert.equal(all.leftTrigger, 255);
    assert.equal(all.rightTrigger, 255);
    assert.deepEqual([all.leftX, all.leftY, all.rightX, all.rightY], [0, 65534, 65534, 0]);
    for (const name of ['disconnect', 'stop', 'final-stop', 'initially-disconnected']) {
        const messages = get(name);
        assert.equal(messages.length, 2, name);
        neutral(messages[0]);
        assert.equal(messages[1].kind, 'connection', name);
        assert.equal(messages[1].connected, false, name);
    }
    for (const name of ['still-disconnected', 'backpressure', 'stop-again', 'closed', 'send-failure', 'shortcut-backpressure'])
        assert.equal(get(name).length, 0, name);
    assert.equal(connectThenState('reconnect').buttons, 0x2000);
    assert.equal(oneState('latest-after-backpressure').buttons, 0x1000);
    assert.equal(connectThenState('restart').packetNumber, 0);
    assert.equal(connectThenState('reopen').packetNumber, 0);
    assert.equal(oneState('send-recovery').buttons, 0x1000);
    const beforeShortcut = oneState('before-shortcut');
    assert.equal(beforeShortcut.buttons, 0x120);
    assert.equal(beforeShortcut.leftTrigger, 255);
    assert.equal(beforeShortcut.leftX, 65534);
    for (const name of ['shortcut-neutral', 'shortcut-release-wait', 'shortcut-neutral-after-backpressure'])
        neutral(oneState(name));
    assert.equal(oneState('shortcut-resumed').buttons, 0x10);
    for (const messages of packets.values()) for (const message of messages) {
        assert.equal(message.deviceId, 0);
        assert.equal(message.type, 0);
        if (message.kind === 'gamepad') {
            assert.ok(BigInt(message.timing.timestampMs) > 1700000000000n, 'Epoch clock');
            assert.equal(message.timing.hardwareClass, 11);
            assert.equal(message.resiliency, null);
        }
    }

    const shortcutsExecutable = path.join(temporary, 'shortcuts-probe');
    run(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
        '-Itests/luna-input/stubs', '-Isource', 'tests/luna-input/shortcuts_probe.cpp', '-o', shortcutsExecutable]);
    console.log(run(shortcutsExecutable, []).trim());

    // Execute the actual UI handlers with a minimal DOM and a controlled clock.
    const elements = new Map(), timers = new Map();
    let clock = 0, nextTimer = 0;
    function element() {
        return { textContent: '', hidden: false, disabled: false,
            classList: { toggle() {} }, appendChild() {}, setAttribute() {}, addEventListener() {} };
    }
    function byId(id) { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); }
    const context = vm.createContext({ LunaInput, console,
        document: { getElementById: byId, createElement: element },
        window: { addEventListener() {} }, performance: { now: () => clock },
        setInterval: callback => { timers.set(++nextTimer, callback); return nextTimer; },
        clearInterval: id => timers.delete(id),
    });
    vm.runInContext(fs.readFileSync(path.join(root, 'tests/browser-reference/client.js'), 'utf8'), context);
    const exec = code => vm.runInContext(code, context);
    const deliver = hex => { context.packet = Buffer.from(hex, 'hex'); exec('receiveControllerState(packet)'); };
    const fixture = name => reference.cases.find(test => test.name === name).expectedHex;
    exec('startControllerTest()');
    deliver(fixture('connect-default-empty-body'));
    deliver(fixture('button-mask-4096'));
    assert.match(byId('controller-held').textContent, /Held: B\./);
    clock = 751;
    for (const tick of [...timers.values()]) tick();
    assert.equal(byId('controller-held').textContent, 'Held: none.');
    assert.match(byId('controller-status').textContent, /updates stopped/);
    // Recover with the next state, then ignore an older/duplicate sequence.
    deliver(fixture('packet-varint-127'));
    deliver(fixture('packet-varint-128'));
    deliver(fixture('button-mask-4096'));
    assert.equal(byId('controller-held').textContent, 'Held: none.');
    deliver(fixture('disconnect-default'));
    assert.match(byId('controller-status').textContent, /disconnected/);
    deliver(fixture('button-mask-4096'));
    assert.equal(byId('controller-held').textContent, 'Held: none.');
    deliver(fixture('connect-default-empty-body'));
    deliver(fixture('button-mask-4096'));
    assert.match(byId('controller-held').textContent, /Held: B\./);
    deliver(fixture('disconnect-default'));
    deliver(fixture('connect-default-empty-body'));
    deliver(fixture('packet-varint-4294967295'));
    deliver(fixture('button-mask-4096'));
    assert.match(byId('controller-held').textContent, /Held: B\./, 'uint32 packet wrap');
    deliver('c206');
    assert.equal(byId('controller-held').textContent, 'Held: none.');
    assert.match(byId('controller-status').textContent, /Invalid binary input/);
    exec('stopControllerTest("Data channel closed; controls released.")');
    assert.equal(timers.size, 0);
    assert.equal(byId('controller-held').textContent, 'Held: none.');
    console.log(`PASS: browser decodes ${reference.cases.length} reference packets; malformed packets rejected; ` +
        'native mapping/lifecycle/backpressure and browser release behavior pass.');
};
