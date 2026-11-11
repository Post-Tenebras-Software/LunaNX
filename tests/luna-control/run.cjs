// Uses committed oracle bytes; no network, archived bundles or Switch required.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');

const root = path.resolve(__dirname, '../..');
const reference = JSON.parse(fs.readFileSync(path.join(root,
    'tests/luna-control/reference.json'), 'utf8'));
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'lunanx-control-'));
function run(command, args, options = {}) {
    const result = spawnSync(command, args, { cwd: root, encoding: 'utf8', ...options });
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`${command} failed (${result.status}):\n${result.stderr}`);
    return result.stdout;
}
const string = value => Buffer.from(value || '', 'utf8').toString('hex') || '-';
const strings = values => [values.length, ...values.map(string)];
async function main() {
try {
    const executable = path.join(temporary, 'control-probe');
    run(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
        '-Isource', 'source/luna_control.cpp', 'source/luna_time_sync.cpp',
        'tests/luna-control/probe.cpp', 'tests/luna-control/unit.cpp', '-o', executable]);
    const commands = reference.examples.map(example => {
        const input = example.input, args = [example.kind, example.expectedHex];
        switch (example.kind) {
        case 'feature': args.push(...strings(input.features)); break;
        case 'ping': args.push(input.clientSendTimestampMs || 0,
            input.serverReceiveTimestampMs || 0, input.serverSendTimestampMs || 0); break;
        case 'context': args.push(...['browserType', 'browserVersion', 'deviceModel', 'deviceType',
            'osName', 'osVersion'].map(field => string(input[field]))); break;
        case 'display':
            args.push(input.negotiationId, input.width, input.height, input.refreshRate,
                input.safeZones.left, input.safeZones.right, input.safeZones.top, input.safeZones.bottom,
                input.minBitsPerSecond, input.maxBitsPerSecond, input.startBitsPerSecond, input.clampingMode,
                input.codecs.length);
            for (const codec of input.codecs) args.push(codec.videoCodec, codec.profile, codec.level,
                codec.tier || 0, codec.maxReferenceFramesNum);
            args.push(...strings(input.features));
            break;
        case 'ackClose': case 'resetIdle': break;
        default: throw new Error(`Unsupported fixture ${example.name}`);
        }
        return args.join(' ');
    });
    process.stdout.write(run(executable, [], { input: commands.join('\n') + '\n' }));
    await require('./local-peer.cjs')({ run, root, temporary, reference });
} finally {
    fs.rmSync(temporary, { recursive: true, force: true });
}
}
main().catch(error => { console.error(error); process.exitCode = 1; });
