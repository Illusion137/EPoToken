#!/usr/bin/env node
/* eslint-disable */
// Copies the C++ core sources from the repo root into a binding's local
// cpp/core/ directory so the binding's published npm tarball is self-contained
// (npm `files` cannot reach above the package root).
//
// Usage:    node scripts/sync-core.js <relative-dest-from-cwd>
// Example:  node ../../scripts/sync-core.js cpp/core
//
// Layout mirrored: <dest>/include/, <dest>/src/

const fs   = require('fs');
const path = require('path');

const REPO_ROOT = path.resolve(__dirname, '..');

const FILES = [
    ['include/epotoken.h',         'include/epotoken.h'],
    ['src/base64.h',               'src/base64.h'],
    ['src/base64.cpp',             'src/base64.cpp'],
    ['src/challenge.h',            'src/challenge.h'],
    ['src/challenge.cpp',          'src/challenge.cpp'],
    ['src/constants.h',            'src/constants.h'],
    ['src/http_client.h',          'src/http_client.h'],
    ['src/http_client.cpp',        'src/http_client.cpp'],
    ['src/innertube_client.h',     'src/innertube_client.h'],
    ['src/innertube_client.cpp',   'src/innertube_client.cpp'],
    ['src/innertube_messages.h',   'src/innertube_messages.h'],
    ['src/innertube_messages.cpp', 'src/innertube_messages.cpp'],
    ['src/placeholder.cpp',        'src/placeholder.cpp'],
];

const destArg = process.argv[2];
if (!destArg) {
    console.error('sync-core: missing <dest> argument');
    process.exit(2);
}
const dest = path.resolve(process.cwd(), destArg);

// When installed from an npm tarball the parent repo is not present.
// The tarball already contains the synced files, so silently no-op.
if (!fs.existsSync(REPO_ROOT) || !fs.existsSync(path.join(REPO_ROOT, 'src'))) {
    console.log('sync-core: parent repo not present, assuming pre-synced tarball');
    process.exit(0);
}

let copied = 0;
for (const [rel, target] of FILES) {
    const from = path.join(REPO_ROOT, rel);
    const to   = path.join(dest, target);
    if (!fs.existsSync(from)) {
        console.warn(`sync-core: skip ${rel} (not found in repo)`);
        continue;
    }
    fs.mkdirSync(path.dirname(to), { recursive: true });
    fs.copyFileSync(from, to);
    copied++;
}
console.log(`sync-core: copied ${copied} files into ${path.relative(process.cwd(), dest)}`);
