// Uses committed reference bytes: no network, downloaded bundles, or Switch needed.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');

const root = path.resolve(__dirname, '../..');
const reference = JSON.parse(fs.readFileSync(path.join(__dirname, 'reference.json'), 'utf8'));
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'lunanx-input-'));
function run(command, args, options = {}) {
    const result = spawnSync(command, args, { cwd: root, encoding: 'utf8', ...options });
    if (result.error) throw result.error;
    if (result.status !== 0)
        throw new Error(`${command} failed (${result.status}):\n${result.stderr}\n${result.stdout}`);
    return result.stdout;
}
function resiliencyArguments(r) {
    return r ? [1, r.category, r.eventId, r.keyframeId, r.terminalIdentifier, r.retransmitCount] : [0];
}
try {
    const executable = path.join(temporary, 'encoder-probe');
    run(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
        '-Isource', 'source/luna_input.cpp', 'tests/luna-input/encoder_probe.cpp', '-o', executable]);
    const commands = [], expected = [], names = [];
    for (const test of reference.cases) {
        const args = [test.kind];
        if (test.kind === 'connection')
            args.push(test.deviceId, +test.connected, test.type, ...resiliencyArguments(test.resiliency));
        if (test.kind === 'gamepad') {
            const s = test.state;
            args.push(s.deviceId, s.packetNumber, s.buttons, s.leftTrigger, s.rightTrigger,
                s.leftX, s.leftY, s.rightX, s.rightY, s.type, s.leftSensitivity, s.rightSensitivity,
                test.timing.timestampMs, test.timing.hardwareClass, ...resiliencyArguments(test.resiliency));
        }
        commands.push(args.join(' '));
        expected.push(test.expectedHex);
        names.push(test.name);
    }
    for (const kind of ['axis', 'trigger']) {
        for (const test of reference[kind]) {
            commands.push(`${kind} ${test.value}`);
            expected.push(String(test.expected));
            names.push(`${kind}(${test.value})`);
        }
    }
    const actual = run(executable, [], { input: commands.join('\n') + '\n' }).trim().split('\n');
    assert.equal(actual.length, expected.length, 'Output count');
    for (let i = 0; i < expected.length; ++i)
        assert.equal(actual[i], expected[i], names[i]);
    console.log(`PASS: ${reference.cases.length} packets match Luna's JavaScript writer; ` +
        `${reference.axis.length + reference.trigger.length} conversions match browser rounding; invalid inputs rejected.`);
    require('./local-test.cjs')({ run, root, temporary, reference });
}
finally {
    fs.rmSync(temporary, { recursive: true, force: true });
}
