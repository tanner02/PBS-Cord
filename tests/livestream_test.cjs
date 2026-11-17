// SPDX-License-Identifier: GPL-3.0-or-later
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../payload/web/livestream.js'), 'utf8');
const disconnected = {state: 'disconnected', error: '', account: null, guilds: [], dms: [], vault_saved: false};
const settle = () => new Promise(resolve => setImmediate(resolve));

// Every request is answered in memory. No browser, console, Discord account,
// timers, or network connection is needed to exercise the actual UI script.
async function harness(respond, settings = {}) {
  class Element {
    constructor(id) {
      this.id = id;
      this.hidden = false;
      this.disabled = false;
      this.textContent = '';
      this.children = [];
      this.listeners = new Map();
      this.selected = '';
      this.checked = false;
    }
    get firstChild() { return this.children[0] || null; }
    get options() { return this.children; }
    get value() { return this.selected; }
    set value(value) { this.selected = value; }
    add(option) { this.children.push(option); }
    append(child) { this.children.push(child); }
    remove() { this.removed = true; }
    setAttribute() {}
    removeChild(child) {
      this.children.splice(this.children.indexOf(child), 1);
      if (!this.children.some(option => option.value === this.selected)) this.selected = '';
    }
    addEventListener(event, listener) { this.listeners.set(event, listener); }
  }
  const elements = new Map([...source.matchAll(/\$\('([^']+)'\)/g)].map(match => [match[1], new Element(match[1])]));
  const get = id => {
    if (settings.streamingOnly === true) assert.ok(!id.startsWith('discord-voice-'), 'streaming-only dashboard must not access removed voice controls');
    assert.ok(elements.has(id), `Unexpected DOM element ${id}`);
    return elements.get(id);
  };
  get('discord-link').textContent = 'Link with Discord QR';
  get('discord-remember').checked = true; // The real checkbox has the checked attribute.
  const requests = [], intervals = [], timers = new Map();
  const head = new Element('head');
  class XMLHttpRequest {
    open(method, url, asynchronous) { this.method = method; this.url = url; assert.equal(asynchronous, true); }
    setRequestHeader() {}
    send(body) {
      const request = {method: this.method, url: this.url, body: body === undefined ? undefined : JSON.parse(body)};
      requests.push(request);
      const response = request.url.startsWith('/config.json') ? (settings.localRespond ? settings.localRespond(request) : {json: {height: 720, fps: 60, resolutions: [720, 1080], framerates: [30, 60]}})
        : request.url.startsWith('/status.json') ? (settings.localRespond ? settings.localRespond(request) : {json: {recording: false}})
        : request.url.startsWith('/capture/profile') ? settings.localRespond(request)
        : request.url.startsWith('/discord/activity?') ? (settings.activityRespond ? settings.activityRespond(request) : {json: {enabled: false, game: null}})
        : respond(request);
      const finish = value => {
        this.status = value.status || 200;
        this.responseText = JSON.stringify(value.json);
        this.onload();
      };
      if (response && typeof response.then === 'function') response.then(finish);
      else finish(response);
    }
  }
  let now = 100000;
  const globals = {
    document: {getElementById: get, head, createElement: tag => new Element(tag), createElementNS: (_namespace, tag) => new Element(tag)}, XMLHttpRequest,
    Option: class { constructor(text, value) { this.text = text; this.value = value; } },
    URL, setInterval: callback => intervals.push(callback),
    Date: {now: () => now},
    setTimeout: callback => { const id = Symbol(); timers.set(id, callback); return id; },
    clearTimeout: id => timers.delete(id),
    qrcodegen: {QrCode: {Ecc: {MEDIUM: 1}, encodeText: () => ({size: 1, getModule: () => true})}},
  };
  globals.window = globals;
  vm.runInNewContext(source, globals, {filename: 'livestream.js'});
  await settle();
  return {
    get, requests, scripts: head.children,
    beginClick(id) { return get(id).listeners.get('click')(); },
    beginPoll() { return intervals[0](); },
    async videoPoll(milliseconds = 5000) { now += milliseconds; await intervals[1](); await settle(); },
    beginVideoPoll(milliseconds = 5000) { now += milliseconds; return intervals[1](); },
    async toggle(enabled) { get('discord-activity-enabled').checked = enabled; await get('discord-activity-enabled').listeners.get('change')(); await settle(); },
    beginToggle(enabled) { get('discord-activity-enabled').checked = enabled; return get('discord-activity-enabled').listeners.get('change')(); },
    async change(id, value) { get(id).value = value; await get(id).listeners.get('change')(); await settle(); },
    beginChange(id, value) { get(id).value = value; return get(id).listeners.get('change')(); },
    async checkbox(id, checked) { get(id).checked = checked; await get(id).listeners.get('change')(); await settle(); },
    async input(id, value) { get(id).value = value; await get(id).listeners.get('input')(); await settle(); },
    async loadProvider(provider) { globals.hcaptcha = provider; globals.pbsDiscordHcaptchaReady(); await settle(); },
    async providerTimeout() { for (const callback of [...timers.values()]) callback(); await settle(); },
    async poll() { await intervals[0](); await settle(); },
    async click(id) {
      const element = get(id);
      assert.equal(element.disabled, false, `${id} must be enabled before clicking`);
      await element.listeners.get('click')();
      await settle();
    },
  };
}

test('healthy initial status with an empty error renders and uses a cache timestamp', async () => {
  const ui = await harness(() => ({json: disconnected}));
  assert.equal(ui.get('discord-state').textContent, 'Disconnected');
  assert.equal(ui.get('discord-error').hidden, true);
  assert.equal(ui.get('discord-link-panel').hidden, false);
  assert.equal(ui.requests[0].method, 'GET');
  assert.match(ui.requests[0].url, /^\/discord\/status\?ts=\d+$/);
});

test('server choices come from status and retain the selected server across polls', async () => {
  const linked = {...disconnected, state: 'ready', account: {username: 'Tester'},
    guilds: [{id: 'guild-a', name: 'Alpha'}, {id: 'guild-b', name: 'Beta'}], dms: []};
  const ui = await harness(request => ({json: request.url.startsWith('/discord/channels') ? [] : linked}));
  const select = ui.get('discord-server');
  assert.deepEqual(select.options.filter(option => option.value).map(option => [option.value, option.text]),
    [['guild-a', 'Alpha'], ['guild-b', 'Beta']]);
  await ui.change('discord-server', 'guild-b');
  await ui.poll();
  assert.equal(select.value, 'guild-b');
  const request = ui.requests.find(request => request.url.startsWith('/discord/channels'));
  assert.ok(request, 'selected guild loads its channels');
  assert.equal(request.method, 'POST');
  assert.equal(request.body.guild_id, 'guild-b');
});

test('a failed Link request leaves the button available for a second attempt', async () => {
  const ui = await harness(request => ({json: request.method === 'POST' ? {error: 'Temporary link failure'} : disconnected}));
  await ui.click('discord-link');
  assert.equal(ui.get('discord-link').disabled, false);
  assert.equal(ui.get('discord-link').textContent, 'Link with Discord QR');
  await ui.click('discord-link');
  assert.equal(ui.requests.filter(request => request.method === 'POST' && request.url.startsWith('/discord/link?')).length, 2);
});

test('a saved vault exposes its unlock control while the account is unlinked', async () => {
  const ui = await harness(() => ({json: {...disconnected, vault_saved: true}}));
  assert.equal(ui.get('discord-vault-panel').hidden, false, 'unlock ancestor must be visible');
  assert.equal(ui.get('discord-vault-unlock').hidden, false);
});

test('linking keeps the QR code and its parent visible while disabling duplicate links', async () => {
  const ui = await harness(() => ({json: {...disconnected, state: 'linking', qr_url: 'https://discord.com/ra/test'}}));
  assert.equal(ui.get('discord-link-panel').hidden, false);
  assert.equal(ui.get('discord-qr-panel').hidden, false);
  assert.equal(ui.get('discord-qr').children.length, 1);
  assert.equal(ui.get('discord-link').disabled, true);
});

test('an error-only response reports the console request failure', async () => {
  const ui = await harness(() => ({json: {error: 'Unknown control operation'}}));
  assert.equal(ui.get('discord-state').textContent, 'Console unavailable');
  assert.equal(ui.get('discord-error').textContent, 'Unknown control operation');
  assert.equal(ui.get('discord-error').hidden, false);
});

test('a status carrying an engine error renders its state and diagnostic', async () => {
  const ui = await harness(() => ({json: {...disconnected, state: 'error', error: 'Capture temporarily unavailable'}}));
  assert.equal(ui.get('discord-state').textContent, 'Engine error');
  assert.equal(ui.get('discord-error').textContent, 'Capture temporarily unavailable');
  assert.equal(ui.get('discord-error').hidden, false);
});

const challenge = id => ({...disconnected, state: 'verification_required', vault_saved: true,
  verification: {id, service: 'hcaptcha', sitekey: 'public-sitekey', rqdata: 'challenge-data'}});
function fakeProvider() {
  const renders = [], removed = [], data = [];
  return {
    renders, removed, data,
    render(container, options) { renders.push({container, options}); return renders.length - 1; },
    setData(widget, value) { data.push({widget, value}); },
    remove(widget) { removed.push(widget); },
  };
}
async function showWidget(ui, provider) {
  await ui.loadProvider(provider);
}
const verifyRequests = ui => ui.requests.filter(request => request.url.startsWith('/discord/verify?'));

test('verification automatically loads the official provider once and preserves a same-ID widget', async () => {
  const ui = await harness(() => ({json: challenge('challenge-a')}));
  const provider = fakeProvider();
  assert.equal(ui.get('discord-verification-panel').hidden, false);
  assert.equal(ui.get('discord-link').disabled, true);
  assert.equal(ui.get('discord-vault-unlock').disabled, true);
  assert.equal(ui.get('discord-verification-cancel').disabled, false);
  assert.equal(ui.scripts.length, 1);
  assert.equal(ui.requests.filter(request => request.method === 'POST').length, 0);
  await ui.poll();
  assert.equal(ui.scripts.length, 1, 'polling the pending challenge must not reload its provider');
  await showWidget(ui, provider);
  assert.equal(ui.scripts.length, 1);
  assert.equal(ui.scripts[0].src, 'https://js.hcaptcha.com/1/api.js?onload=pbsDiscordHcaptchaReady&render=explicit&recaptchacompat=off');
  assert.equal(provider.renders.length, 1);
  assert.equal(provider.renders[0].container, ui.get('discord-verification-widget'));
  assert.equal(provider.renders[0].options.sitekey, 'public-sitekey');
  assert.equal(provider.renders[0].options.theme, 'dark');
  assert.equal(provider.data[0].widget, 0);
  assert.equal(provider.data[0].value.rqdata, 'challenge-data');
  await ui.poll();
  assert.equal(provider.renders.length, 1);
  assert.equal(provider.removed.length, 0);
  assert.equal(ui.scripts.length, 1);
  assert.equal(verifyRequests(ui).length, 0);
});

test('only a human provider completion submits, exactly once for a challenge', async () => {
  const ui = await harness(request => ({json: request.method === 'POST' ? {} : challenge('challenge-a')}));
  const provider = fakeProvider();
  await showWidget(ui, provider);
  const completed = provider.renders[0].options.callback;
  const first = completed('human-token');
  await completed('duplicate-token');
  await first;
  assert.equal(verifyRequests(ui).length, 1);
  assert.deepEqual(verifyRequests(ui)[0].body, {challenge_id: 'challenge-a', solution: 'human-token'});
  assert.equal(ui.get('discord-verification-status').textContent.includes('human-token'), false);
});

test('expiry and provider errors offer manual retry and invalidate old callbacks', async () => {
  const ui = await harness(() => ({json: challenge('challenge-a')}));
  const provider = fakeProvider();
  await showWidget(ui, provider);
  const first = provider.renders[0].options;
  first['expired-callback']();
  assert.match(ui.get('discord-verification-status').textContent, /expired/);
  assert.equal(ui.get('discord-verification-show').disabled, false);
  await first.callback('expired-token');
  assert.equal(verifyRequests(ui).length, 0);
  await ui.click('discord-verification-show');
  assert.equal(provider.renders.length, 2);
  const second = provider.renders[1].options;
  second['error-callback']('provider-detail-not-for-display');
  assert.match(ui.get('discord-verification-status').textContent, /provider reported an error/);
  assert.equal(ui.get('discord-verification-show').disabled, false);
  await second.callback('invalidated-token');
  assert.equal(verifyRequests(ui).length, 0);
  assert.deepEqual(provider.removed, [0, 1]);
});

test('replacement and cancellation remove widgets and ignore stale completions', async () => {
  let status = challenge('challenge-a');
  const ui = await harness(request => {
    if (request.url.startsWith('/discord/cancel-link')) return {json: {}};
    return {json: status};
  });
  const provider = fakeProvider();
  await showWidget(ui, provider);
  const old = provider.renders[0].options.callback;
  status = challenge('challenge-b');
  await ui.poll();
  assert.deepEqual(provider.removed, [0]);
  await old('old-token');
  assert.equal(provider.renders.length, 2, 'a replacement challenge opens automatically');
  const current = provider.renders[1].options.callback;
  await ui.click('discord-verification-cancel');
  await current('cancelled-token');
  await ui.poll(); // A stale status response must not restore the cancelled ID.
  assert.equal(ui.get('discord-verification-panel').hidden, false);
  assert.equal(ui.get('discord-verification-show').disabled, true);
  assert.equal(ui.get('discord-verification-cancel').disabled, false);
  assert.deepEqual(provider.removed, [0, 1]);
  assert.equal(verifyRequests(ui).length, 0);
  const cancel = ui.requests.find(request => request.url.startsWith('/discord/cancel-link?'));
  assert.equal(cancel.method, 'POST');
  assert.deepEqual(cancel.body, {});
});

test('leaving verification removes its widget and ignores completion', async () => {
  let status = challenge('challenge-a');
  const ui = await harness(() => ({json: status}));
  const provider = fakeProvider();
  await showWidget(ui, provider);
  status = disconnected;
  await ui.poll();
  await provider.renders[0].options.callback('stale-token');
  assert.equal(ui.get('discord-verification-panel').hidden, true);
  assert.deepEqual(provider.removed, [0]);
  assert.equal(verifyRequests(ui).length, 0);
});

test('failed script load, missing provider API, and timeout expose retry without submitting', async () => {
  for (const failure of ['script', 'missing', 'timeout']) {
    const ui = await harness(() => ({json: challenge('challenge-a')}));
    const showing = ui.beginClick('discord-verification-show');
    if (failure === 'script') ui.scripts[0].onerror();
    else if (failure === 'missing') await ui.loadProvider(undefined);
    else await ui.providerTimeout();
    await showing;
    assert.match(ui.get('discord-verification-status').textContent, /provider is unavailable/);
    assert.equal(ui.get('discord-verification-show').disabled, false);
    assert.equal(verifyRequests(ui).length, 0);
  }
});

test('verifying disables duplicate login actions while keeping Cancel available', async () => {
  const ui = await harness(() => ({json: {...challenge('challenge-a'), state: 'verifying'}}));
  assert.equal(ui.get('discord-link').disabled, true);
  assert.equal(ui.get('discord-vault-unlock').disabled, true);
  assert.equal(ui.get('discord-verification-show').disabled, true);
  assert.equal(ui.get('discord-verification-cancel').disabled, false);
  assert.equal(ui.scripts.length, 0);
});

test('Cancel stays visible after the backend consumes the verification descriptor', async () => {
  const ui = await harness(() => ({json: {...disconnected, state: 'verifying', verification: null}}));
  assert.equal(ui.get('discord-verification-panel').hidden, false);
  assert.equal(ui.get('discord-verification-show').disabled, true);
  assert.equal(ui.get('discord-verification-cancel').disabled, false);
  assert.equal(ui.get('discord-verification-status').textContent, 'Checking verification…');
});

test('cancelling while the script loads prevents a late provider from rendering', async () => {
  const ui = await harness(request => ({json: request.method === 'POST' ? {} : challenge('challenge-a')}));
  const showing = ui.beginClick('discord-verification-show');
  await ui.click('discord-verification-cancel');
  const provider = fakeProvider();
  await ui.loadProvider(provider);
  await showing;
  assert.equal(provider.renders.length, 0);
  assert.equal(verifyRequests(ui).length, 0);
  assert.equal(ui.get('discord-verification-panel').hidden, false);
  assert.equal(ui.get('discord-verification-show').disabled, true);
});

test('missing provider data support surfaces a failure and removes the incomplete widget', async () => {
  const ui = await harness(() => ({json: challenge('challenge-a')}));
  const provider = fakeProvider();
  delete provider.setData;
  await showWidget(ui, provider);
  assert.match(ui.get('discord-verification-status').textContent, /provider is unavailable/);
  assert.equal(ui.get('discord-verification-show').disabled, false);
  assert.deepEqual(provider.removed, [0]);
  await provider.renders[0].options.callback('incomplete-widget-token');
  assert.equal(verifyRequests(ui).length, 0);
});

test('verification submission errors provide cancellation without repeating the token', async () => {
  const ui = await harness(request => ({json: request.method === 'POST' ? {error: 'Rejected'} : challenge('challenge-a')}));
  const provider = fakeProvider();
  await showWidget(ui, provider);
  await provider.renders[0].options.callback('human-token');
  assert.match(ui.get('discord-verification-status').textContent, /Cancel login and try again/);
  await provider.renders[0].options.callback('duplicate-token');
  assert.equal(verifyRequests(ui).length, 1);
  assert.equal(ui.get('discord-verification-cancel').disabled, false);
});

const linkedStatus = id => ({...disconnected, state: 'ready', account: {id, username: id},
  guilds: [{id: 'guild-a', name: 'Alpha'}, {id: 'guild-b', name: 'Beta'}],
  dms: [{id: 'dm-a', name: 'Friend'}]});
const channelRequests = ui => ui.requests.filter(request => request.url.startsWith('/discord/channels?'));

test('streaming-only build starts and stops video without voice controls or voice requests', async () => {
  let status = linkedStatus('account-a');
  const ui = await harness(request => {
    if (request.url.startsWith('/discord/start?')) status = {...status, state: 'streaming', stream: {channel_id: 'dm-a', guild_id: null}};
    if (request.url.startsWith('/discord/stop?')) status = {...status, state: 'ready', stream: null};
    return {json: status};
  }, {streamingOnly: true});
  await ui.change('discord-channel', 'dm-a');
  assert.equal(ui.get('discord-start').disabled, false);
  await ui.click('discord-start');
  assert.equal(ui.get('discord-stop').disabled, false);
  await ui.poll();
  await ui.click('discord-stop');
  assert.equal(ui.get('discord-start').disabled, false);
  assert.equal(ui.requests.some(request => /^\/discord\/(voice|join|leave)[?]/.test(request.url)), false);
});
function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return {promise, resolve};
}

test('many status polls keep selected server/channel nodes and issue only one channel lookup', async () => {
  const ui = await harness(request => ({json: request.method === 'POST'
    ? [{id: 'voice-a', name: 'Voice'}] : JSON.parse(JSON.stringify(linkedStatus('account-a')))}));
  const dmOption = ui.get('discord-channel').options[1];
  await ui.poll();
  assert.equal(ui.get('discord-channel').options[1], dmOption);
  await ui.change('discord-server', 'guild-a');
  assert.equal(ui.requests.filter(request => request.url.startsWith('/discord/status?')).length, 2,
    'channel loading must not perform an extra status request');
  await ui.change('discord-channel', 'voice-a');
  const serverOption = ui.get('discord-server').options[1];
  const channelOption = ui.get('discord-channel').options[1];
  for (let i = 0; i < 20; ++i) await ui.poll();
  assert.equal(channelRequests(ui).length, 1);
  assert.equal(ui.get('discord-server').value, 'guild-a');
  assert.equal(ui.get('discord-channel').value, 'voice-a');
  assert.equal(ui.get('discord-server').options[1], serverOption);
  assert.equal(ui.get('discord-channel').options[1], channelOption);
  assert.equal(ui.get('discord-start').disabled, false);
});

test('server selection fetches only that server and revisiting uses the account cache', async () => {
  const ui = await harness(request => ({json: request.method === 'POST'
    ? [{id: request.body.guild_id, name: request.body.guild_id}] : linkedStatus('account-a')}));
  assert.equal(channelRequests(ui).length, 0, 'initial DM selection needs no remote lookup');
  await ui.change('discord-server', 'guild-a');
  await ui.change('discord-server', 'guild-b');
  await ui.change('discord-server', 'guild-a');
  assert.deepEqual(channelRequests(ui).map(request => request.body.guild_id), ['guild-a', 'guild-b']);
  assert.equal(ui.get('discord-channel').options[1].value, 'guild-a');
});

test('logout and account replacement invalidate cached server choices', async () => {
  let status = linkedStatus('account-a');
  const ui = await harness(request => ({json: request.method === 'POST'
    ? [{id: status.account.id, name: status.account.id}] : status}));
  await ui.change('discord-server', 'guild-a');
  status = disconnected;
  await ui.poll();
  assert.equal(ui.get('discord-channel').options.length, 1);
  status = linkedStatus('account-a');
  await ui.poll();
  await ui.change('discord-server', 'guild-a');
  status = linkedStatus('account-b');
  await ui.poll();
  assert.equal(ui.get('discord-server').value, '');
  await ui.change('discord-server', 'guild-a');
  assert.equal(channelRequests(ui).length, 3);
  assert.equal(ui.get('discord-channel').options[1].value, 'account-b');
});

test('out-of-order channel responses cannot replace the selected server choices', async () => {
  const a = deferred(), b = deferred();
  const ui = await harness(request => request.method === 'POST'
    ? (request.body.guild_id === 'guild-a' ? a.promise : b.promise)
    : {json: linkedStatus('account-a')});
  const loadingA = ui.beginChange('discord-server', 'guild-a');
  const loadingB = ui.beginChange('discord-server', 'guild-b');
  b.resolve({json: [{id: 'voice-b', name: 'Beta voice'}]});
  await loadingB;
  await ui.change('discord-channel', 'voice-b');
  a.resolve({json: [{id: 'voice-a', name: 'Alpha voice'}]});
  await loadingA;
  assert.equal(ui.get('discord-channel').value, 'voice-b');
  assert.equal(ui.get('discord-channel').options[1].value, 'voice-b');
  assert.equal(ui.get('discord-channel').disabled, false);
});

test('a response from a logged-out session cannot overwrite a relinked account', async () => {
  const old = deferred();
  let status = linkedStatus('account-a'), lookups = 0;
  const ui = await harness(request => {
    if (request.method !== 'POST') return {json: status};
    return ++lookups === 1 ? old.promise : {json: [{id: 'new-voice', name: 'New voice'}]};
  });
  const loadingOld = ui.beginChange('discord-server', 'guild-a');
  status = disconnected; await ui.poll();
  status = linkedStatus('account-a'); await ui.poll();
  await ui.change('discord-server', 'guild-a');
  old.resolve({json: [{id: 'old-voice', name: 'Old voice'}]});
  await loadingOld;
  assert.equal(ui.get('discord-channel').options[1].value, 'new-voice');
});

test('channel failure preserves current options and selection without polling retries', async () => {
  let failures = 0;
  const ui = await harness(request => {
    if (request.method !== 'POST') return {json: linkedStatus('account-a')};
    if (request.body.guild_id === 'guild-b' && ++failures === 1)
      return {json: {error: 'Rate limited; try selecting this server later'}};
    return {json: [{id: 'voice-a', name: 'Voice'}]};
  });
  await ui.change('discord-server', 'guild-a');
  await ui.change('discord-channel', 'voice-a');
  const option = ui.get('discord-channel').options[1];
  await ui.change('discord-server', 'guild-b');
  for (let i = 0; i < 10; ++i) await ui.poll();
  assert.equal(channelRequests(ui).length, 2);
  assert.equal(ui.get('discord-channel').options[1], option);
  assert.equal(ui.get('discord-channel').value, 'voice-a');
  assert.match(ui.get('discord-error').textContent, /Rate limited/);
  assert.equal(ui.get('discord-start').disabled, true, 'old choices must not start in a different server');
  await ui.change('discord-server', 'guild-a');
  await ui.change('discord-server', 'guild-b');
  assert.equal(channelRequests(ui).length, 3, 'a later explicit selection may retry the failed lookup');
});

test('slow status requests remain single-flight across timer ticks', async () => {
  const pending = deferred();
  const ui = await harness(() => pending.promise);
  const polls = Array.from({length: 10}, () => ui.beginPoll());
  assert.equal(ui.requests.filter(request => request.url.startsWith('/discord/status?')).length, 1);
  pending.resolve({json: linkedStatus('account-a')});
  await Promise.all(polls);
  assert.equal(ui.get('discord-state').textContent, 'Ready');
  assert.equal(ui.requests.filter(request => request.url.startsWith('/discord/status?')).length, 1);
});

const defaultVideo = () => ({height: 720, fps: 60, resolutions: [720, 1080], framerates: [30, 60],
  requested_height: 720, requested_fps: 60, profile_changing: false, profile_error: false});
const configRequests = ui => ui.requests.filter(request => request.url === '/config.json');
const profileRequests = ui => ui.requests.filter(request => request.url.startsWith('/capture/profile?'));

test('video choices stay dirty and keep stable option nodes across local config and Discord polls', async () => {
  const ui = await harness(() => ({json: disconnected}));
  assert.equal(ui.get('discord-account-panel').hidden, true);
  assert.equal(ui.get('discord-video-height').disabled, false, 'capture settings work without a linked account');
  const heightOption = ui.get('discord-video-height').options[1];
  const fpsOption = ui.get('discord-video-fps').options[1];
  await ui.change('discord-video-height', '1080');
  await ui.change('discord-video-fps', '30');
  for (let i = 0; i < 10; ++i) { await ui.poll(); await ui.videoPoll(); }
  assert.equal(ui.get('discord-video-height').value, '1080');
  assert.equal(ui.get('discord-video-fps').value, '30');
  assert.equal(ui.get('discord-video-height').options[1], heightOption);
  assert.equal(ui.get('discord-video-fps').options[1], fpsOption);
  assert.equal(ui.get('discord-video-apply').disabled, false);
  assert.equal(profileRequests(ui).length, 0);
  assert.equal(channelRequests(ui).length, 0);
});

test('Apply posts both settings locally, follows completion, and displays a rollback', async () => {
  let config = defaultVideo();
  const ui = await harness(() => ({json: disconnected}), {localRespond(request) {
    if (request.url.startsWith('/capture/profile?')) {
      const params = new URL(request.url, 'http://console').searchParams;
      config = {...config, requested_height: Number(params.get('height')), requested_fps: Number(params.get('fps')), profile_changing: true};
      return {json: {message: 'Requested'}};
    }
    return {json: request.url.startsWith('/config.json') ? config : {recording: false}};
  }});
  await ui.change('discord-video-height', '1080');
  await ui.change('discord-video-fps', '30');
  await ui.click('discord-video-apply');
  const request = profileRequests(ui)[0], params = new URL(request.url, 'http://console').searchParams;
  assert.equal(request.method, 'POST');
  assert.equal(params.get('height'), '1080');
  assert.equal(params.get('fps'), '30');
  assert.equal(request.url, '/capture/profile?height=1080&fps=30');
  assert.equal(ui.get('discord-video-height').disabled, true);
  config = {...config, height: 1080, fps: 30, profile_changing: false};
  await ui.videoPoll(1000);
  assert.equal(ui.get('discord-video-height').value, '1080');
  assert.equal(ui.get('discord-video-fps').value, '30');
  assert.equal(ui.get('discord-video-height').disabled, false);
  assert.equal(ui.get('discord-video-apply').disabled, true);
  await ui.change('discord-video-height', '720');
  await ui.change('discord-video-fps', '60');
  await ui.click('discord-video-apply');
  config = {...config, profile_changing: false, profile_error: true, profile_error_height: 720, profile_error_fps: 60};
  await ui.videoPoll(1000);
  assert.equal(ui.get('discord-video-height').value, '1080');
  assert.equal(ui.get('discord-video-fps').value, '30');
  assert.match(ui.get('discord-video-status').textContent, /720p at 60 fps could not start. Restored 1080p at 30 fps/);
});

test('capture failure disables video edits and explains recovery without claiming restored video', async () => {
  let config = defaultVideo();
  const ui = await harness(() => ({json: disconnected}), {localRespond: request => ({json: request.url === '/config.json' ? config : {recording: false}})});
  await ui.change('discord-video-height', '1080');
  config = {...config, capture_error: 'OpenVideo 0x81950008', profile_error: true};
  await ui.videoPoll();
  assert.match(ui.get('discord-video-status').textContent, /Video capture stopped: OpenVideo 0x81950008/);
  assert.match(ui.get('discord-video-status').textContent, /Reload PBS to retry capture/);
  assert.doesNotMatch(ui.get('discord-video-status').textContent, /Voice controls/);
  assert.doesNotMatch(ui.get('discord-video-status').textContent, /Restored|Active PS5 capture/);
  assert.equal(ui.get('discord-video-height').disabled, true);
  assert.equal(ui.get('discord-video-fps').disabled, true);
  assert.equal(ui.get('discord-video-apply').disabled, true);
  assert.equal(profileRequests(ui).length, 0);
});

test('video config polls are single-flight and limited to every five seconds when settled', async () => {
  const pending = deferred();
  let initial = true;
  const ui = await harness(() => ({json: disconnected}), {localRespond(request) {
    if (request.url.startsWith('/status.json')) return {json: {recording: false}};
    return initial ? pending.promise : {json: defaultVideo()};
  }});
  const polls = Array.from({length: 10}, () => ui.beginVideoPoll(0));
  assert.equal(configRequests(ui).length, 1);
  assert.equal(ui.requests.filter(request => request.url === '/status.json').length, 1);
  initial = false; pending.resolve({json: defaultVideo()});
  await Promise.all(polls);
  await ui.videoPoll(4000);
  assert.equal(configRequests(ui).length, 1);
  await ui.videoPoll(1000);
  assert.equal(configRequests(ui).length, 2);
  assert.equal(ui.requests.filter(request => request.url.startsWith('/discord/activity?')).length, 1);
});

test('only advertised frame rates appear, with current-fps fallback for older payloads', async () => {
  for (const config of [{...defaultVideo(), fps: 30, framerates: [30]}, {...defaultVideo(), fps: 30, framerates: undefined}]) {
    const ui = await harness(() => ({json: disconnected}), {localRespond: request => ({json: request.url.startsWith('/config.json') ? config : {recording: false}})});
    assert.deepEqual(ui.get('discord-video-fps').options.filter(option => option.value).map(option => option.value), ['30']);
    assert.equal(ui.get('discord-video-fps').value, '30');
  }
});

test('recording and profile changes disable capture edits, and rejected Apply keeps dirty choices', async () => {
  let recording = true, config = defaultVideo();
  const ui = await harness(() => ({json: disconnected}), {localRespond(request) {
    if (request.url.startsWith('/capture/profile')) return {status: 409, json: {message: 'Stop recording first'}};
    return {json: request.url.startsWith('/config.json') ? config : {recording}};
  }});
  assert.equal(ui.get('discord-video-height').disabled, true);
  assert.equal(ui.get('discord-video-fps').disabled, true);
  assert.equal(ui.get('discord-video-apply').disabled, true);
  recording = false; await ui.videoPoll();
  await ui.change('discord-video-height', '1080');
  await ui.click('discord-video-apply');
  assert.equal(ui.get('discord-video-height').value, '1080');
  assert.match(ui.get('discord-video-status').textContent, /Stop recording first/);
  assert.equal(ui.get('discord-video-apply').disabled, false);
  config = {...config, profile_changing: true}; await ui.videoPoll();
  assert.equal(ui.get('discord-video-height').disabled, true);
});

test('game activity is opt-in, posts a boolean, and displays game names as text', async () => {
  const game = {title_id: 'PPSA12345', name: '<img src=x onerror=alert(1)> Game'};
  const ui = await harness(() => ({json: linkedStatus('account-a')}), {
    activityRespond: request => ({json: {enabled: request.method === 'POST' ? request.body.enabled : false, game: request.method === 'POST' ? game : null}}),
  });
  assert.equal(ui.get('discord-activity-enabled').checked, false);
  assert.equal(ui.requests.filter(request => request.method === 'POST').length, 0);
  await ui.toggle(true);
  const request = ui.requests.find(request => request.url.startsWith('/discord/activity?') && request.method === 'POST');
  assert.deepEqual(request.body, {enabled: true});
  assert.equal(ui.get('discord-activity-enabled').checked, true);
  assert.equal(ui.get('discord-activity-status').textContent, 'Running PS5 game: ' + game.name);
  assert.equal(ui.get('discord-activity-status').children.length, 0);
});

test('stale status cannot revert game activity during or after its save request', async () => {
  let pendingStatus = null;
  const save = deferred();
  const ui = await harness(() => pendingStatus ? pendingStatus.promise : {json: {...linkedStatus('account-a'), activity: {enabled: false, game: null}}}, {
    activityRespond: request => request.method === 'POST' ? save.promise : {json: {enabled: false, game: null}},
  });
  const saving = ui.beginToggle(true);
  assert.equal(ui.get('discord-activity-enabled').disabled, true);
  await ui.poll();
  assert.equal(ui.get('discord-activity-enabled').checked, true);
  pendingStatus = deferred();
  const polling = ui.beginPoll();
  save.resolve({json: {enabled: true, game: null}}); await saving;
  pendingStatus.resolve({json: {...linkedStatus('account-a'), activity: {enabled: false, game: null}}});
  await polling;
  assert.equal(ui.get('discord-activity-enabled').checked, true);
  assert.equal(ui.get('discord-activity-enabled').disabled, false);
  assert.match(ui.get('discord-activity-status').textContent, /activity stays empty/);
});

test('activity validation failures restore the previous checkbox and keep a useful error', async () => {
  for (const bad of [{enabled: 'true', game: null}, {enabled: true, game: {title_id: 123, name: 'Game'}}, {error: 'Saving failed'}]) {
    const ui = await harness(() => ({json: disconnected}), {
      activityRespond: request => ({json: request.method === 'POST' ? bad : {enabled: false, game: null}}),
    });
    await ui.toggle(true);
    assert.equal(ui.get('discord-activity-enabled').checked, false);
    assert.equal(ui.get('discord-activity-enabled').disabled, false);
    assert.match(ui.get('discord-activity-status').textContent, /invalid game activity settings|Saving failed/);
    await ui.poll();
    assert.equal(ui.get('discord-activity-enabled').checked, false);
  }
});

test('persisted game activity reflects server state without auto-enabling or needing an account', async () => {
  const ui = await harness(() => ({json: disconnected}), {activityRespond: () => ({json: {enabled: true, game: null}})});
  assert.equal(ui.get('discord-activity-enabled').checked, true);
  assert.match(ui.get('discord-activity-status').textContent, /Connect your Discord account/);
  for (let i = 0; i < 10; ++i) await ui.poll();
  assert.equal(ui.requests.filter(request => request.url.startsWith('/discord/activity?')).length, 1);
  assert.equal(ui.requests.filter(request => request.method === 'POST').length, 0);
});

test('a config response started before Apply cannot reset newly requested choices', async () => {
  const old = deferred();
  let config = defaultVideo(), hold = false;
  const ui = await harness(() => ({json: disconnected}), {localRespond(request) {
    if (request.url.startsWith('/status.json')) return {json: {recording: false}};
    if (request.url.startsWith('/capture/profile')) {
      hold = false;
      config = {...config, profile_changing: true, requested_height: 1080, requested_fps: 30};
      return {json: {message: 'Requested'}};
    }
    return hold ? old.promise : {json: config};
  }});
  await ui.change('discord-video-height', '1080');
  await ui.change('discord-video-fps', '30');
  hold = true;
  const polling = ui.beginVideoPoll();
  const applying = ui.beginClick('discord-video-apply');
  await settle();
  old.resolve({json: defaultVideo()});
  await polling; await applying;
  assert.equal(ui.get('discord-video-height').value, '1080');
  assert.equal(ui.get('discord-video-fps').value, '30');
  assert.equal(ui.get('discord-video-height').disabled, true);
});

test('local status recovers an initial activity-load error without repeating the GET', async () => {
  let status = disconnected;
  const ui = await harness(() => ({json: status}), {activityRespond: () => ({json: {error: 'Engine is starting'}})});
  assert.match(ui.get('discord-activity-status').textContent, /Engine is starting/);
  status = {...linkedStatus('account-a'), activity: {enabled: true, game: null}};
  await ui.poll();
  assert.equal(ui.get('discord-activity-enabled').checked, true);
  assert.match(ui.get('discord-activity-status').textContent, /activity stays empty/);
  assert.equal(ui.requests.filter(request => request.url.startsWith('/discord/activity?')).length, 1);
});

test('completed capture fallback explains actual resolution and preserved frame rate without requests', async () => {
  for (const fps of [30, 60]) {
    const config = {...defaultVideo(), height: 1080, fps, profile_fallback_height: 1440};
    const ui = await harness(() => ({json: disconnected}), {localRespond: request => ({json: request.url === '/config.json' ? config : {recording: false}})});
    assert.equal(ui.get('discord-video-status').textContent,
      '1440p is unavailable with the current console video output. Using 1080p at ' + fps + ' fps; bitrate unchanged.');
    const initialRequests = ui.requests.length;
    for (let i = 0; i < 10; ++i) await ui.videoPoll(0);
    assert.equal(ui.requests.length, initialRequests);
    assert.equal(profileRequests(ui).length, 0);
    assert.equal(ui.requests.filter(request => request.method === 'POST').length, 0);
  }
});

test('fallback notice preserves dirty choices and option nodes while advertised capabilities stay unchanged', async () => {
  let config = {...defaultVideo(), height: 1080};
  const ui = await harness(() => ({json: disconnected}), {localRespond: request => ({json: request.url === '/config.json' ? config : {recording: false}})});
  await ui.change('discord-video-height', '720');
  await ui.change('discord-video-fps', '30');
  const heightOption = ui.get('discord-video-height').options[1];
  const fpsOption = ui.get('discord-video-fps').options[1];
  config = {...config, profile_fallback_height: 1440};
  await ui.videoPoll();
  for (let i = 0; i < 10; ++i) await ui.poll();
  assert.equal(ui.get('discord-video-height').value, '720');
  assert.equal(ui.get('discord-video-fps').value, '30');
  assert.equal(ui.get('discord-video-height').options[1], heightOption);
  assert.equal(ui.get('discord-video-fps').options[1], fpsOption);
  assert.equal(profileRequests(ui).length, 0);
  assert.match(ui.get('discord-video-status').textContent, /Using 1080p at 60 fps; bitrate unchanged/);
});

test('unsupported 1440p disappears from capabilities without disturbing other dirty selections', async () => {
  let config = {...defaultVideo(), height: 1080, resolutions: [720, 1080, 1440]};
  const ui = await harness(() => ({json: disconnected}), {localRespond: request => ({json: request.url === '/config.json' ? config : {recording: false}})});
  await ui.change('discord-video-height', '720');
  await ui.change('discord-video-fps', '30');
  config = {...config, profile_fallback_height: 1440, resolutions: [720, 1080]};
  await ui.videoPoll();
  assert.deepEqual(ui.get('discord-video-height').options.filter(option => option.value).map(option => option.value), ['720', '1080']);
  assert.equal(ui.get('discord-video-height').value, '720');
  assert.equal(ui.get('discord-video-fps').value, '30');
  const option = ui.get('discord-video-height').options[1];
  await ui.videoPoll();
  assert.equal(ui.get('discord-video-height').options[1], option);
});

test('fallback keeps changing/error precedence and clears after a new manual Apply', async () => {
  let config = {...defaultVideo(), height: 1080, profile_fallback_height: 1440, profile_changing: true};
  const ui = await harness(() => ({json: disconnected}), {localRespond(request) {
    if (request.url.startsWith('/capture/profile')) {
      config = {...config, height: 720, fps: 30, profile_fallback_height: 0};
      return {json: {message: 'Applied'}};
    }
    return {json: request.url === '/config.json' ? config : {recording: false}};
  }});
  assert.match(ui.get('discord-video-status').textContent, /Applying video settings/);
  config = {...config, profile_changing: false, profile_error: true, profile_error_height: 1440};
  await ui.videoPoll(1000);
  assert.match(ui.get('discord-video-status').textContent, /could not start. Restored/);
  config = {...config, profile_error: false};
  await ui.videoPoll();
  assert.match(ui.get('discord-video-status').textContent, /1440p is unavailable/);
  await ui.change('discord-video-height', '720');
  await ui.change('discord-video-fps', '30');
  await ui.click('discord-video-apply');
  assert.equal(ui.get('discord-video-status').textContent, 'Active PS5 capture: 720p at 30 fps.');
  assert.equal(profileRequests(ui).length, 1);
});

test('Remember on this PS5 defaults checked and sends the selected boolean with Link', async () => {
  const html = fs.readFileSync(path.join(__dirname, '../payload/web/h264.html'), 'utf8');
  assert.match(html, /id="discord-remember"[^>]*\bchecked\b/);
  for (const remember of [true, false]) {
    const ui = await harness(request => ({json: request.method === 'POST' ? {} : disconnected}));
    assert.equal(ui.get('discord-remember').checked, true);
    ui.get('discord-remember').checked = remember;
    await ui.click('discord-link');
    const request = ui.requests.find(request => request.url.startsWith('/discord/link?'));
    assert.deepEqual(request.body, {remember});
  }
});

test('connected accounts always expose Save, including an already remembered account', async () => {
  let status = linkedStatus('account-a');
  const ui = await harness(request => {
    if (request.url.startsWith('/discord/vault?')) {
      status = {...status, vault_saved: true, auto_login_saved: true};
      return {json: {ok: true}};
    }
    return {json: status};
  });
  assert.equal(ui.get('discord-vault-panel').hidden, false);
  assert.equal(ui.get('discord-vault-save').hidden, false);
  assert.equal(ui.get('discord-vault-save').textContent, 'Save account');
  assert.equal(ui.get('discord-passphrase-panel').hidden, true);
  await ui.click('discord-vault-save');
  const saved = ui.requests.find(request => request.url.startsWith('/discord/vault?'));
  assert.deepEqual(saved.body, {automatic: true});
  assert.equal(ui.get('discord-vault-save').hidden, false);
  assert.equal(ui.get('discord-vault-save').textContent, 'Save account again');
  assert.match(ui.get('discord-account-save-status').textContent, /Saved on this PS5. Automatic sign-in is enabled/);
  const html = fs.readFileSync(path.join(__dirname, '../payload/web/h264.html'), 'utf8');
  assert.match(html, /<section id="discord-vault-panel"/);
  assert.doesNotMatch(html, /<details id="discord-vault-panel"/);
});

test('an automatically remembered account never triggers a frontend unlock loop', async () => {
  const ui = await harness(() => ({json: {...disconnected, vault_saved: true, auto_login_saved: true}}));
  assert.equal(ui.get('discord-vault-panel').hidden, false);
  assert.equal(ui.get('discord-passphrase-panel').hidden, true);
  assert.equal(ui.get('discord-vault-unlock').hidden, true);
  assert.match(ui.get('discord-account-save-status').textContent, /Automatic sign-in is enabled/);
  for (let i = 0; i < 10; ++i) await ui.poll();
  assert.equal(ui.requests.filter(request => request.method === 'POST').length, 0);
});

test('legacy restore upgrades to automatic sign-in and clears its passphrase only after success', async () => {
  let status = {...disconnected, vault_saved: true, auto_login_saved: false}, attempts = 0;
  const ui = await harness(request => {
    if (request.url.startsWith('/discord/unlock?')) {
      if (++attempts === 1) return {json: {error: 'Passphrase rejected'}};
      status = {...linkedStatus('account-a'), vault_saved: true, auto_login_saved: true};
      return {json: {ok: true}};
    }
    return {json: status};
  });
  assert.equal(ui.get('discord-passphrase-panel').hidden, false);
  assert.equal(ui.get('discord-vault-unlock').hidden, false);
  assert.match(ui.get('discord-account-save-status').textContent, /needs its passphrase once/);
  ui.get('discord-passphrase').value = 'fixture-passphrase';
  await ui.click('discord-vault-unlock');
  assert.equal(ui.get('discord-passphrase').value, 'fixture-passphrase');
  await ui.click('discord-vault-unlock');
  assert.equal(ui.get('discord-passphrase').value, '');
  const restores = ui.requests.filter(request => request.url.startsWith('/discord/unlock?'));
  assert.equal(restores.length, 2);
  assert.deepEqual(restores[1].body, {passphrase: 'fixture-passphrase', remember: true});
  assert.equal(ui.get('discord-passphrase-panel').hidden, true);
  assert.equal(ui.get('discord-vault-save').hidden, false);
});

test('account storage failures remain visible alongside usable account controls', async () => {
  const ui = await harness(() => ({json: {...linkedStatus('account-a'), vault_saved: true,
    auto_login_saved: false, account_save_error: 'Storage unavailable'}}));
  assert.equal(ui.get('discord-vault-panel').hidden, false);
  assert.equal(ui.get('discord-vault-save').hidden, false);
  assert.equal(ui.get('discord-vault-save').disabled, false);
  assert.equal(ui.get('discord-account-save-status').textContent, 'Account could not be saved: Storage unavailable');
});

test('an automatic popup failure offers manual retry without a polling reload loop', async () => {
  const ui = await harness(() => ({json: challenge('challenge-a')}));
  assert.equal(ui.scripts.length, 1);
  ui.scripts[0].onerror();
  await settle();
  assert.match(ui.get('discord-verification-status').textContent, /provider is unavailable/);
  assert.equal(ui.get('discord-verification-show').disabled, false);
  for (let i = 0; i < 5; ++i) await ui.poll();
  assert.equal(ui.scripts.length, 1);
  const retrying = ui.beginClick('discord-verification-show');
  assert.equal(ui.scripts.length, 2);
  const provider = fakeProvider();
  await ui.loadProvider(provider); await retrying;
  assert.equal(provider.renders.length, 1);
  assert.equal(verifyRequests(ui).length, 0, 'opening the widget never submits a solution');
});

test('reloading a video stream restores its server and channel through one cached lookup', async () => {
  const channels = deferred();
  const status = {...linkedStatus('account-a'), state: 'streaming',
    stream: {guild_id: 'guild-b', channel_id: 'channel-b'}};
  const ui = await harness(request => request.url.startsWith('/discord/channels?')
    ? channels.promise : {json: status});
  assert.equal(ui.get('discord-server').value, 'guild-b');
  assert.equal(ui.get('discord-start').disabled, true);
  channels.resolve({json: [{id: 'channel-b', name: 'Stream room'}]});
  await settle();
  assert.equal(ui.get('discord-channel').value, 'channel-b');
  for (let i = 0; i < 5; i++) await ui.poll();
  assert.equal(ui.get('discord-channel').value, 'channel-b');
  assert.equal(channelRequests(ui).length, 1);
});
