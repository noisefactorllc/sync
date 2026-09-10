import assert from 'node:assert/strict';
import test from 'node:test';
import { decodeSenderStats } from '../../browser/stats.js';

const raw = (checksum = '18446744073709551615') =>
  '{"type":"stats","id":"sender-1","accepted":3,"dropped":2,"rejected":1,' +
  '"failed":0,"lastSequence":9,"lastPresentationTimeUs":123000,"checksum":' + checksum + '}';

test('native statistics retain all 64 checksum bits', () => {
  assert.deepEqual(decodeSenderStats(raw(), 'sender-1'), {
    accepted: 3, dropped: 2, rejected: 1, failed: 0, lastSequence: 9,
    lastPresentationTimeUs: 123000, checksum: 'ffffffffffffffff',
  });
  assert.equal(decodeSenderStats(raw('9007199254740993'), 'sender-1').checksum, '0020000000000001');
  assert.equal(decodeSenderStats(raw('0'), 'sender-1').checksum, '0000000000000000');
});

test('native statistics reject ambiguous fields and unsafe numeric counters', () => {
  for (const value of [
    raw().replace('"accepted":3', '"accepted":9007199254740993'),
    raw().replace('"accepted":3', '"accepted":-1'),
    raw().replace('"accepted":3', '"accepted":1.5'),
    raw().replace('"accepted":3', '"accepted":3,"accepted":4'),
    raw().replace('"accepted":3', '"accepted":3,"\\u0061ccepted":4'),
    raw().replace('"accepted":3,', ''),
    raw().replace('"type":"stats"', '"type":"other"'),
    raw().replace('"id":"sender-1"', '"id":"sender-2"'),
    raw().replace('"checksum":', '"extra":0,"checksum":'),
    raw('18446744073709551616'), raw('-1'), raw('1e3'), raw('"12"'),
    raw('null'), raw('{}'), raw('[]'), raw('01'), raw() + '{}',
    raw().replace(':3', ':\u000b3'), raw().replace(':3', ':\u00a03'),
  ]) {
    assert.throws(() => decodeSenderStats(value, 'sender-1'), TypeError, value);
  }
});

test('statistics decoding accepts reordered fields and escaped string values', () => {
  const value = '{ "checksum":42, "lastPresentationTimeUs":0,"lastSequence":0,' +
    '"failed":0,"rejected":0,"dropped":0,"accepted":0,"id":"sender\\u002d1","type":"stats" }';
  assert.equal(decodeSenderStats(value, 'sender-1').checksum, '000000000000002a');
  assert.throws(() => decodeSenderStats(' '.repeat(16385) + value, 'sender-1'), TypeError);
});
