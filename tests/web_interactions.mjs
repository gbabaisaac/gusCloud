import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const html = readFileSync(new URL('../web/index.html', import.meta.url), 'utf8');
const app = readFileSync(new URL('../web/app.js', import.meta.url), 'utf8');

const views = [...html.matchAll(/data-view="([^"]+)"/g)].map(match => match[1]);
for (const view of views) {
  assert.match(html, new RegExp(`id="${view}-view"`), `${view} needs a page container`);
  assert.match(app, new RegExp(`${view}\\s*:`), `${view} needs a label or renderer`);
}

for (const id of ['theme-toggle', 'global-search', 'notifications', 'dismiss-banner']) {
  assert.match(html, new RegExp(`id="${id}"`), `${id} needs a stable control id`);
  assert.match(app, new RegExp(`['"]${id}['"]`), `${id} needs an interaction handler`);
}

assert.doesNotMatch(
  html,
  /<button class="nav-item"(?![^>]*data-view)/,
  'sidebar buttons must navigate to a real view',
);
assert.match(app, /querySelectorAll\('\.nav-item\[data-view\]'\)/, 'all sidebar destinations need navigation binding');

console.log(`web interaction contract: ${views.length} views and 4 shell controls verified`);
