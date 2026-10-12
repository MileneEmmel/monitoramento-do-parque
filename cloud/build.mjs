import { mkdir, copyFile, readdir, unlink } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
export async function build(root = path.dirname(fileURLToPath(import.meta.url))) {
  const files = new Map();
  const publicAsset = (name) =>
    /^[\w.-]+\.(?:html|js|css|woff2|webp)$/.test(name) && !name.startsWith('.');
  for (const entry of await readdir(path.join(root, 'public'), { withFileTypes: true })) {
    if (!entry.isFile() || !publicAsset(entry.name))
      throw Error('Arquivo inesperado em public: ' + entry.name);
    files.set(entry.name, path.join(root, 'public', entry.name));
  }
  for (const name of ['style.css', 'source-sans-3-v1.woff2', 'folhagem-v2.webp'])
    files.set(name, path.join(root, '../dashboard/public', name));
  const dist = path.join(root, 'dist');
  await mkdir(dist, { recursive: true });
  const previous = await readdir(dist, { withFileTypes: true });
  // Fail closed: credentials and deployment metadata do not belong in public output.
  for (const entry of previous)
    if (!entry.isFile() || !publicAsset(entry.name))
      throw Error('Remova o arquivo não público de dist antes de publicar: ' + entry.name);
  for (const [name, source] of files) await copyFile(source, path.join(dist, name));
  for (const entry of previous)
    if (!files.has(entry.name)) await unlink(path.join(dist, entry.name));
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await build();
  console.log('Dashboard online preparado em cloud/dist.');
}
