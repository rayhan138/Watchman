import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';

const renderer = await readFile(new URL('../src/renderer.js', import.meta.url), 'utf8');

test('Network tab refresh interval is twenty minutes', () => {
  assert.match(renderer, /NETWORK_HEALTH_REFRESH_INTERVAL_MS\s*=\s*20\s*\*\s*60\s*\*\s*1000/);
});

test('Network tab polling stops when leaving the Network tab', () => {
  assert.match(renderer, /function stopNetworkHealthPolling\(\)/);
  assert.match(renderer, /if\s*\(previousTab\s*===\s*'network'\s*&&\s*tabId\s*!==\s*'network'\)\s*{\s*stopNetworkHealthPolling\(\);/s);
});
