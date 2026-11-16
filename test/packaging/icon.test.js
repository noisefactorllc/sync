import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { inflateSync } from "node:zlib";

const sourceDirectory = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");

// The Noise Factor desktop apps share one icon palette: a #1a1a1a tile with a
// #c8ff00 mark. Sync's icon uses the same two colours.
const TILE = "#1a1a1a";
const MARK = "#c8ff00";

// Decodes an 8-bit RGBA, non-interlaced PNG into rows of RGBA bytes.
function decodeRgbaPng(buffer) {
  assert.equal(buffer.subarray(0, 8).toString("hex"), "89504e470d0a1a0a");
  let width = 0;
  let height = 0;
  const data = [];
  for (let offset = 8; offset < buffer.length;) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString("latin1", offset + 4, offset + 8);
    const body = buffer.subarray(offset + 8, offset + 8 + length);
    if (type === "IHDR") {
      width = body.readUInt32BE(0);
      height = body.readUInt32BE(4);
      assert.deepEqual([...body.subarray(8, 13)], [8, 6, 0, 0, 0], "8-bit RGBA, non-interlaced");
    } else if (type === "IDAT") {
      data.push(body);
    }
    offset += length + 12;
  }
  const raw = inflateSync(Buffer.concat(data));
  const stride = width * 4;
  const rows = [];
  let previous = Buffer.alloc(stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = Buffer.from(raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)));
    for (let x = 0; x < stride; x++) {
      const left = x >= 4 ? line[x - 4] : 0;
      const up = previous[x];
      const upLeft = x >= 4 ? previous[x - 4] : 0;
      let predictor = 0;
      if (filter === 1) predictor = left;
      else if (filter === 2) predictor = up;
      else if (filter === 3) predictor = (left + up) >> 1;
      else if (filter === 4) {
        const p = left + up - upLeft;
        const pa = Math.abs(p - left);
        const pb = Math.abs(p - up);
        const pc = Math.abs(p - upLeft);
        predictor = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft;
      }
      line[x] = (line[x] + predictor) & 0xff;
    }
    rows.push(line);
    previous = line;
  }
  return { width, height, pixel(x, y) {
    const [r, g, b, a] = rows[y].subarray(x * 4, x * 4 + 4);
    return { hex: `#${[r, g, b].map(value => value.toString(16).padStart(2, "0")).join("")}`, alpha: a };
  } };
}

test("the Sync icon uses the desktop apps' tile and mark colours", () => {
  const icon = readFileSync(path.join(sourceDirectory, "packaging/Sync.svg"), "utf8");
  assert.match(icon, new RegExp(`<rect x="6" y="6" width="108" height="108" rx="26" fill="${TILE}"/>`));
  assert.match(icon, new RegExp(`<g fill="none" stroke="${MARK}"`));
  assert.match(icon, new RegExp(`<g fill="${MARK}">`));
  const colours = new Set(icon.match(/#[0-9a-fA-F]{6}\b/g).map(colour => colour.toLowerCase()));
  assert.deepEqual([...colours].sort(), [TILE, MARK].sort());
});

test("the Linux icon is the Sync icon in the desktop apps' colours", () => {
  const png = decodeRgbaPng(readFileSync(path.join(sourceDirectory, "packaging/linux/noisedeck-sync.png")));
  assert.equal(png.width, 256);
  assert.equal(png.height, 256);
  // Sample points in the icon's 120-unit viewBox, scaled to 256 pixels.
  const at = (x, y) => png.pixel(Math.round(x * 256 / 120), Math.round(y * 256 / 120));
  assert.deepEqual(at(60, 15), { hex: TILE, alpha: 255 }, "open tile above the crossing");
  assert.deepEqual(at(22, 35), { hex: MARK, alpha: 255 }, "upper-left terminal");
  assert.deepEqual(at(98, 85), { hex: MARK, alpha: 255 }, "lower-right terminal");
  assert.equal(at(1, 1).alpha, 0, "corner outside the rounded tile");
});
