import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, readdir, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { build } from './build.mjs';

// Synthetic files only, confined to the unique temporary directory below.
const temp = await mkdtemp(path.join(os.tmpdir(), 'park-build-test-'));
try {
  const root = path.join(temp, 'cloud'),
    shared = path.join(temp, 'dashboard/public');
  await mkdir(path.join(root, 'public'), { recursive: true });
  await mkdir(shared, { recursive: true });
  await writeFile(path.join(root, 'public/index.html'), 'fixture');
  for (const name of ['style.css', 'source-sans-3-v1.woff2', 'folhagem-v2.webp'])
    await writeFile(path.join(shared, name), 'fixture');
  await build(root);
  assert.equal((await readdir(path.join(root, 'dist'))).length, 4);
  assert.equal(await readFile(path.join(root, 'dist/index.html'), 'utf8'), 'fixture');
  await writeFile(path.join(root, 'dist/obsolete.js'), 'obsolete');
  await build(root);
  assert(!(await readdir(path.join(root, 'dist'))).includes('obsolete.js'));
  await writeFile(path.join(root, 'public/.env'), 'synthetic-secret');
  await assert.rejects(() => build(root), /inesperado/);
  await rm(path.join(root, 'public/.env'));
  await writeFile(path.join(root, 'dist/.env.local'), 'synthetic-secret');
  await assert.rejects(() => build(root), /não público/);
  console.log('PASS: public build, shared assets, stale asset removal and private-file rejection.');
} finally {
  await rm(temp, { recursive: true, force: true });
}
