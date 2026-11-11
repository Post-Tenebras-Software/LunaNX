// Host simulation only: no Switch, downloaded bundles, or network needed.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');

const root = path.resolve(__dirname, '../..');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'lunanx-session-'));
const sanitizer = process.env.LUNANX_SANITIZE === '1'
    ? ['-fsanitize=undefined', '-fno-sanitize-recover=all'] : [];
function run(command, args) {
    const result = spawnSync(command, args, { cwd: root, encoding: 'utf8' });
    if (result.error) throw result.error;
    if (result.status !== 0)
        throw new Error(`${command} failed (${result.status}):\n${result.stdout}${result.stderr}`);
    return result.stdout;
}
try {
    for (const target of ['unit', 'demo']) {
        const executable = path.join(temporary, target);
        run(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
            ...sanitizer, '-Isource',
            'source/luna_session.cpp', `tests/luna-session/${target}.cpp`, '-o', executable]);
        process.stdout.write(run(executable, []));
    }
} finally {
    fs.rmSync(temporary, { recursive: true, force: true });
}
