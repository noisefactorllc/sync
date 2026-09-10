const COUNTERS = [
  'accepted', 'dropped', 'rejected', 'failed', 'lastSequence', 'lastPresentationTimeUs',
];
const FIELDS = new Set(['type', 'id', ...COUNTERS, 'checksum']);
const MAX_UINT64 = (1n << 64n) - 1n;
const TOKEN = /[\x20\t\r\n]*("(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"|0|[1-9][0-9]*)[\x20\t\r\n]*/y;

// Read the flat v1 record before JSON numbers lose uint64 precision.
export function decodeSenderStats(raw, senderId) {
  const fail = () => { throw new TypeError('Sync statistics are invalid'); };
  if (typeof raw !== 'string' || raw.length > 16_384) fail();
  let cursor = 0;
  const space = () => { while (/[\x20\t\r\n]/.test(raw[cursor] ?? '') && cursor < raw.length) cursor++; };
  const take = (character) => {
    space();
    if (raw[cursor++] !== character) fail();
  };
  const token = () => {
    TOKEN.lastIndex = cursor;
    const match = TOKEN.exec(raw);
    if (!match) fail();
    cursor = TOKEN.lastIndex;
    return match[1];
  };
  const values = Object.create(null);
  take('{');
  while (true) {
    const keyToken = token();
    if (keyToken[0] !== '"') fail();
    const key = JSON.parse(keyToken);
    if (!FIELDS.has(key) || Object.hasOwn(values, key)) fail();
    take(':');
    const value = token();
    const stringField = key === 'type' || key === 'id';
    if (stringField !== (value[0] === '"')) fail();
    values[key] = stringField ? JSON.parse(value) : BigInt(value);
    space();
    if (raw[cursor] === '}') { cursor++; break; }
    take(',');
  }
  space();
  if (cursor !== raw.length || Object.keys(values).length !== FIELDS.size ||
      values.type !== 'stats' || values.id !== senderId || values.checksum > MAX_UINT64) fail();
  const result = {};
  for (const field of COUNTERS) {
    if (values[field] > BigInt(Number.MAX_SAFE_INTEGER)) fail();
    result[field] = Number(values[field]);
  }
  result.checksum = values.checksum.toString(16).padStart(16, '0');
  return result;
}
