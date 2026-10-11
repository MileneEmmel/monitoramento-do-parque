import {mkdir,copyFile,readdir} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const root=path.dirname(fileURLToPath(import.meta.url));
await mkdir(path.join(root,'dist'),{recursive:true});
for(const name of await readdir(path.join(root,'public'))) await copyFile(path.join(root,'public',name),path.join(root,'dist',name));
for(const name of ['style.css','source-sans-3-v1.woff2','folhagem-v2.webp']) await copyFile(path.join(root,'../dashboard/public',name),path.join(root,'dist',name));
console.log('Dashboard online preparado em cloud/dist.');
