// Local browser check: production viewer + synthetic capture, mocked Discord.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE);
const endpoint = process.env.PBS_TEST_URL || 'http://127.0.0.1:18101';
assert.ok(['127.0.0.1', 'localhost'].includes(new URL(endpoint).hostname));
const out = process.env.PBS_UI_OUTPUT;
fs.mkdirSync(out, {recursive: true});
(async () => {
  const browser = await chromium.launch({headless: true});
  try {
    for (const width of [1440, 1024, 390]) {
      const page = await browser.newPage({viewport: {width, height: 1000}});
      const errors = [], profiles = [];
      let frames = 0, engineState = 'ready', target = null, failedBeta = false;
      const channelQueries = [];
      const alpha = '111111111111111111', beta = '222222222222222222', empty = '333333333333333333';
      const guilds = [{id: alpha, name: 'Console Lab', icon: 'a'.repeat(32)},
        {id: beta, name: 'Friends & Games'}, {id: empty, name: 'Quiet corner'}];
      const choices = [{id: '101', name: 'general', position: 0, category_id: '10', category_name: 'Hangouts', category_position: 0},
        {id: '102', name: 'game-night', position: 1, category_id: '10', category_name: 'Hangouts', category_position: 0},
        {id: '103', name: 'stream-testing', position: 0, category_id: '20', category_name: 'Development', category_position: 1}];
      page.on('pageerror', error => errors.push(error.message));
      page.on('request', request => {
        if (new URL(request.url()).pathname === '/frames') frames++;
        if (request.method() === 'POST' && new URL(request.url()).pathname === '/capture/profile') profiles.push(request.url());
      });
      await page.route('**/*', async route => {
        const url = new URL(route.request().url());
        if (url.hostname === 'cdn.discordapp.com') return route.fulfill({contentType: 'image/svg+xml', body: '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" rx="16" fill="#dc5236"/><text x="32" y="41" text-anchor="middle" fill="white" font-family="sans-serif" font-size="27">PS</text></svg>'});
        if (url.origin !== endpoint) return route.abort();
        if (!url.pathname.startsWith('/discord/')) return route.continue();
        let json;
        if (url.pathname === '/discord/status') json = {
          state: engineState, error: '', account: {id: 'demo', username: 'Console player'},
          guilds, dms: [{id: '104', name: 'A friend'}], voice_target: target,
          activity: {enabled: false, game: null}, auto_login_saved: true, vault_saved: true,
          capture: {running: true}, voice: {supported: false, enabled: false},
        };
        else if (url.pathname === '/discord/activity') json = {enabled: false, game: null};
        else if (url.pathname === '/discord/channels') {
          const guild = route.request().postDataJSON().guild_id;
          channelQueries.push(guild);
          if (guild === beta && !failedBeta) {
            failedBeta = true;
            await route.fulfill({status: 503, json: {error: 'Channels temporarily unavailable.'}}); return;
          }
          json = guild === empty ? [] : choices;
        }
        else if (url.pathname === '/discord/start') { target = route.request().postDataJSON(); engineState = 'streaming'; json = {ok: true}; }
        else if (url.pathname === '/discord/stop') { engineState = 'ready'; target = null; json = {ok: true}; }
        else throw new Error('Unexpected Discord request: ' + url.pathname);
        await route.fulfill({json});
      });
      await page.goto(endpoint);
      assert.ok((await page.title()).startsWith('PBS Cord'));
      assert.equal(await page.locator('header h1').textContent(), 'PBS Cord');
      await page.waitForFunction(() => document.getElementById('video').currentTime > 0.5);
      await page.waitForFunction(() => document.getElementById('discord-video-height').options.length > 1);
      assert.equal(await page.locator('#discord-voice-panel').count(), 0, 'streaming-only viewer has no voice UI');
      assert.equal(await page.locator('#toggle-preview').getAttribute('aria-expanded'), 'false');
      assert.equal(await page.locator('#discord-server').isVisible(), false, 'native destination dropdowns are replaced');
      assert.equal(await page.locator('#discord-channel').isVisible(), false);
      await page.locator(`.destination-server[data-destination="${alpha}"]`).click();
      await page.locator('.destination-channel[data-destination="101"]').click();
      assert.equal(await page.locator('#discord-start').isEnabled(), true);
      assert.equal(await page.locator('.channel-category-heading').count(), 2);
      assert.equal(await page.locator(`.destination-server[data-destination="${alpha}"] img`).count(), 1, 'server icon accompanies its name');
      const colors = await page.evaluate(() => ({outer: getComputedStyle(document.getElementById('livestream')).backgroundColor,
        card: getComputedStyle(document.getElementById('discord-video-panel')).backgroundColor}));
      assert.equal(colors.outer, 'rgb(23, 23, 22)'); assert.equal(colors.card, 'rgb(23, 23, 22)');
      await page.locator('#destination-server-search').fill('friends');
      assert.equal(await page.locator('.destination-server:visible').count(), 1);
      assert.equal(channelQueries.length, 1, 'search is local');
      await page.locator('#destination-server-search').fill('');
      const heading = page.locator('.channel-category-heading').first();
      await heading.click(); assert.equal(await heading.getAttribute('aria-expanded'), 'false');
      await page.locator('#destination-channel-search').fill('game');
      assert.equal(await page.locator('.destination-channel:visible').count(), 1, 'search exposes matching channels inside collapsed categories');
      await page.locator('#destination-channel-search').fill('');
      assert.equal(await heading.getAttribute('aria-expanded'), 'false', 'search preserves collapsed state');
      await heading.click();
      await page.evaluate(() => { window.selectedRow = document.querySelector('.destination-channel[data-destination="101"]'); });
      await page.locator('.destination-channel[data-destination="101"]').focus();
      await page.waitForTimeout(4200);
      assert.equal(await page.evaluate(() => document.activeElement === selectedRow && document.querySelector('.destination-channel[data-destination="101"]') === selectedRow), true, 'status polls preserve row and keyboard focus');
      assert.equal(channelQueries.length, 1, 'status polls do not refetch channel choices');
      await page.keyboard.press('ArrowDown');
      assert.equal(await page.locator('.destination-channel[data-destination="102"]').evaluate(e => e === document.activeElement), true);
      await page.keyboard.press('Enter');
      assert.equal(await page.locator('#discord-channel').inputValue(), '102');
      assert.equal(await page.locator('.destination-channel[data-destination="102"]').getAttribute('aria-pressed'), 'true');
      assert.equal(await page.locator('.destination-channel[data-destination="101"]').getAttribute('aria-pressed'), 'false');
      const compact = await page.locator('.video-shell').boundingBox();
      if (width > 780) {
        assert.ok(compact.width <= 402 && compact.width >= 250, 'desktop preview is compact');
        const controls = await page.locator('.livestream-controls').boundingBox();
        assert.ok(compact.x > controls.x + controls.width, 'preview sits to the right');
      }
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'compact page has no horizontal overflow');
      await page.evaluate(() => scrollTo(0, 0));
      await page.screenshot({path: path.join(out, `livestream-${width}.png`), fullPage: true});
      await page.evaluate(() => { window.originalVideo = document.getElementById('video'); });
      const requests = frames, time = await page.locator('#video').evaluate(video => video.currentTime);
      await page.locator('#toggle-preview').click();
      assert.equal(await page.locator('#toggle-preview').getAttribute('aria-expanded'), 'true');
      const expanded = await page.locator('.video-shell').boundingBox();
      if (width > 780) assert.ok(expanded.width > compact.width * 1.6, 'expanded preview spans the workspace');
      await page.waitForFunction(before => document.getElementById('video').currentTime > before + 0.5, time);
      assert.equal(await page.evaluate(() => originalVideo === document.getElementById('video')), true);
      assert.equal(frames, requests, 'expanding does not reconnect capture');
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'expanded page has no horizontal overflow');
      if (width === 1440) await page.screenshot({path: path.join(out, 'livestream-expanded.png'), fullPage: true});
      await page.locator('#toggle-preview').click();
      assert.equal(await page.locator('#toggle-preview').getAttribute('aria-expanded'), 'false');
      assert.equal(frames, requests, 'compacting does not reconnect capture');
      await page.locator('#discord-start').click();
      await page.waitForFunction(() => document.querySelector('.destination-server').disabled);
      assert.equal(await page.locator('.destination-channel[data-destination="102"]').isDisabled(), true, 'active stream locks destination');
      await page.locator('#discord-stop').click();
      await page.waitForFunction(() => !document.querySelector('.destination-server').disabled);
      await page.locator(`.destination-server[data-destination="${empty}"]`).click();
      await page.waitForFunction(() => document.getElementById('destination-channel-status').textContent === 'No voice channels available.');
      assert.equal(await page.locator('#discord-start').isDisabled(), true);
      await page.locator(`.destination-server[data-destination="${alpha}"]`).click();
      assert.equal(channelQueries.length, 2, 'returning to a cached server adds no lookup');
      await page.locator(`.destination-server[data-destination="${beta}"]`).click();
      await page.locator('#destination-retry').waitFor({state: 'visible'});
      assert.equal(channelQueries.length, 3);
      assert.match(await page.locator('#destination-channel-status').textContent(), /temporarily unavailable/);
      await page.waitForTimeout(2100);
      assert.equal(channelQueries.length, 3, 'failed lookups are not repeated by polling');
      await page.locator('#destination-retry').click();
      await page.locator('.destination-channel[data-destination="101"]').waitFor({state: 'visible'});
      assert.equal(channelQueries.length, 4, 'explicit Retry makes one request');
      await page.locator('#discord-video-height').selectOption('1080');
      await page.locator('#discord-video-fps').selectOption('30');
      assert.equal(await page.locator('#discord-video-apply').isEnabled(), true);
      await page.locator('#discord-video-apply').click();
      await page.waitForFunction(() => document.getElementById('discord-video-status').textContent.includes('could not start'));
      assert.equal(profiles.length, 1, 'Apply submits once');
      assert.equal(new URL(profiles[0]).searchParams.get('height'), '1080');
      assert.equal(new URL(profiles[0]).searchParams.get('fps'), '30');
      assert.deepEqual(errors, []);
      console.log(`PASS ${width}px: original palette, named servers/icons, grouped/searchable channels, keyboard and stable polling/cache, layout/playback/settings, no VC`);
      await page.close();
    }
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
