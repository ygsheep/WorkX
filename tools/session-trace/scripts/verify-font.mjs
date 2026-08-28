// 验证 PureNerdFont.woff2 是否包含 icons.js 中使用的全部码点
import { openSync } from 'fontkit';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const fontPath = path.resolve(__dirname, '../src/assets/fonts/PureNerdFont.woff2');
const font = openSync(fontPath);
const set = font.characterSet; // Set<number>

const iconsSrc = fs.readFileSync(path.resolve(__dirname, '../src/lib/icons.js'), 'utf8');
const nfBlock = iconsSrc.match(/export const NF = \{([\s\S]*?)\n\};/)[1];
const re = /([a-zA-Z0-9_]+):\s*'\\u([0-9a-fA-F]{4})'/g;
const entries = [...nfBlock.matchAll(re)];
console.log('entries found:', entries.length);

let missing = 0;
for (const [, name, hex] of entries) {
  const cp = parseInt(hex, 16);
  if (!set.includes(cp)) {
    missing++;
    console.log(`MISSING ${name} U+${hex.toUpperCase()}`);
  }
}
console.log(`\nchecked ${entries.length} icons, ${missing} missing`);
process.exit(missing ? 1 : 0);
