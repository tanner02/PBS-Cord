/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Livestream tab: drives the /discord/* routes served by the payload.
   Includes qrcodegen (MIT, Copyright (c) Project Nayuki) for the account
   linking QR. */
'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const destinationPicker = window.pbsCreateDestinationPicker?.({
    selectServer(value) {
      const select = $('discord-server');
      if (!select.disabled && hasOption(select, value)) { select.value = value; select.dispatchEvent(new Event('change')); }
    },
    selectChannel(value) {
      const select = $('discord-channel');
      if (!select.disabled && hasOption(select, value)) { select.value = value; select.dispatchEvent(new Event('change')); }
    },
    retry() { if (!$('discord-server').disabled) return loadChannels(lastStatus, true); },
  });
  let busy = false, generation = 0, channelLoading = false, qrValue = '';
  let lastStatus = null, polling = null, channelAccount = '', accountEpoch = 0;
  let displayedGuild = null, channelError = '';
  let destinationDirty = false, pendingDestination = null, restoredDestination = '';
  const channelCache = new Map(), optionSignatures = new WeakMap();
  let videoConfig = null, videoRecording = false, videoDirty = false, videoApplying = false;
  let videoPoll = null, videoLastPoll = -Infinity, videoError = '', videoRevision = 0;
  let activity = {enabled: false, game: null}, activityPending = false, activityRevision = 0, activityError = '', activityLoadError = false;
  const states = {disconnected:'Disconnected',linking:'Linking account',verification_required:'Login verification required',verifying:'Checking login verification',connecting:'Connecting',ready:'Ready',joining:'Joining voice',negotiating:'Negotiating encryption',streaming:'Streaming',error:'Engine error'};
  const activeStates = new Set(['joining','negotiating','streaming']);
  const transitional = new Set(['linking','verification_required','verifying','connecting','joining','negotiating']);
  let verification = null, cancelledVerification = '', providerLoad = null;
  function verificationNote(message) { $('discord-verification-status').textContent = message; }
  function removeVerificationWidget(current) {
    current.attempt++;
    if (current.widget !== null) {
      try { window.hcaptcha?.remove(current.widget); } catch (_) {}
      current.widget = null;
    }
    const host = $('discord-verification-widget');
    while (host.firstChild) host.removeChild(host.firstChild);
  }
  function loadVerificationProvider() {
    if (providerLoad) return providerLoad;
    providerLoad = new Promise((resolve, reject) => {
      const script = document.createElement('script');
      let finished = false;
      const finish = error => {
        if (finished) return;
        finished = true; clearTimeout(timer);
        if (error) { script.remove(); providerLoad = null; reject(new Error('Verification provider could not load. Select Show verification to retry.')); }
        else resolve(window.hcaptcha);
      };
      window.pbsDiscordHcaptchaReady = () => finish(!window.hcaptcha || typeof window.hcaptcha.render !== 'function');
      script.src = 'https://js.hcaptcha.com/1/api.js?onload=pbsDiscordHcaptchaReady&render=explicit&recaptchacompat=off';
      script.async = true; script.defer = true;
      script.onerror = () => finish(true);
      const timer = setTimeout(() => finish(true), 20000);
      document.head.append(script);
    });
    return providerLoad;
  }
  function renderVerification(s) {
    const pending = s?.state === 'verification_required' || s?.state === 'verifying';
    const descriptor = pending ? s?.verification : null;
    if (!pending) cancelledVerification = '';
    if (verification && (!descriptor || descriptor.id !== verification.id)) {
      removeVerificationWidget(verification); verification = null;
    }
    if (descriptor && descriptor.id !== cancelledVerification && !verification) {
      verification = {id: descriptor.id, descriptor, attempt: 0, widget: null, submitted: false, loading: false, autoShown: false};
      verificationNote('Complete the verification challenge yourself.');
    }
    // Keep cancellation available if a failed request or an old poll still
    // reports the cancelled challenge, while never restoring its widget.
    const cancelling = descriptor && descriptor.id === cancelledVerification;
    const awaitingResult = s?.state === 'verifying';
    $('discord-verification-panel').hidden = !verification && !cancelling && !awaitingResult;
    if (awaitingResult && !descriptor) verificationNote('Checking verification…');
    $('discord-verification-show').disabled = !verification || verification.loading || verification.widget !== null || verification.submitted || s?.state === 'verifying';
    $('discord-verification-cancel').disabled = false;
    if (s?.state === 'verification_required' && verification && !verification.autoShown) {
      verification.autoShown = true;
      void showVerification();
    }
  }
  async function showVerification() {
    const current = verification;
    if (!current || current.loading || current.submitted || current.widget !== null) return;
    current.loading = true;
    const attempt = ++current.attempt;
    const valid = () => verification === current && current.attempt === attempt && !current.submitted;
    const retry = message => {
      if (!valid()) return;
      removeVerificationWidget(current); current.loading = false;
      verificationNote(message); $('discord-verification-show').disabled = false;
    };
    $('discord-verification-show').disabled = true;
    verificationNote('Loading verification…');
    try {
      const descriptor = current.descriptor;
      if (typeof current.id !== 'string' || !current.id || descriptor.service !== 'hcaptcha' || typeof descriptor.sitekey !== 'string' || !descriptor.sitekey)
        throw new Error('Invalid provider');
      const provider = await loadVerificationProvider();
      if (!valid()) return;
      const widget = provider.render($('discord-verification-widget'), {
        sitekey: descriptor.sitekey, theme: 'dark',
        callback: async token => {
          if (!valid() || typeof token !== 'string' || !token) return;
          current.submitted = true;
          verificationNote('Checking verification…');
          try {
            await call('verify', {challenge_id: current.id, solution: token});
            if (verification === current) await tick();
          } catch (_) {
            if (verification === current) verificationNote('Verification could not be submitted. Cancel login and try again.');
          }
        },
        'expired-callback': () => retry('Verification expired. Select Show verification to retry.'),
        'error-callback': () => retry('Verification provider reported an error. Select Show verification to retry.'),
      });
      if (!valid()) { try { provider.remove(widget); } catch (_) {} return; }
      current.widget = widget;
      if (descriptor.rqdata) {
        if (typeof provider.setData !== 'function') throw new Error('Provider data API unavailable');
        provider.setData(current.widget, {rqdata: descriptor.rqdata});
      }
      current.loading = false;
      verificationNote('Complete the verification below.');
    } catch (_) { retry('Verification provider is unavailable. Select Show verification to retry, or cancel login.'); }
  }
  function note(message) { $('discord-state').textContent = message || ''; }
  function fail(message) { const el = $('discord-error'); el.textContent = message || ''; el.hidden = !message; }
  function call(path, body) {
    return localRequest('/discord/' + path, body === undefined ? 'GET' : 'POST', body);
  }
  function localRequest(path, method = 'GET', body) {
    // XHR instead of fetch: the console's WebKit handles it on every
    // firmware, and its error callbacks carry status detail.
    return new Promise((resolve, reject) => {
      try {
        const xhr = new XMLHttpRequest();
        // Only the Discord bridge strips cache queries. Capture routes match
        // exact request targets and already respond with Cache-Control: no-store.
        const bust = path.startsWith('/discord/') ? (path.indexOf('?') >= 0 ? '&' : '?') + 'ts=' + Date.now() : '';
        xhr.open(method, path + bust, true);
        xhr.timeout = 20000;
        xhr.onload = () => {
          let result = null;
          try { result = JSON.parse(xhr.responseText); } catch (_) {
            reject(new Error(`Console returned an invalid response (${xhr.status}).`));
            return;
          }
          if (result && typeof result.error === 'string' && result.error && !result.state) reject(new Error(result.error));
          else if (xhr.status < 200 || xhr.status >= 300) reject(new Error(result?.message || `Console request failed (${xhr.status}).`));
          else resolve(result);
        };
        xhr.onerror = () => reject(new Error(`Browser could not reach the console (status ${xhr.status || 'network error'}).`));
        xhr.ontimeout = () => reject(new Error('Console request timed out'));
        if (body === undefined) xhr.send();
        else {
          xhr.setRequestHeader('Content-Type', 'application/json');
          xhr.send(JSON.stringify(body));
        }
      } catch (e) { reject(e); }
    });
  }
  function controls(s) {
    const state = s?.state;
    const linked = Boolean(s?.account);
    const locked = busy || transitional.has(state) || activeStates.has(state);
    $('discord-link-panel').hidden = linked;
    $('discord-account-panel').hidden = !linked;
    $('discord-logout').hidden = !linked;
    const legacyVault = !linked && Boolean(s?.vault_saved) && !s?.auto_login_saved;
    $('discord-vault-panel').hidden = false;
    $('discord-vault-save').hidden = !linked;
    $('discord-vault-forget').hidden = !s?.vault_saved;
    $('discord-passphrase-panel').hidden = !legacyVault;
    $('discord-vault-unlock').hidden = !legacyVault;
    if (!busy) $('discord-vault-save').textContent = s?.vault_saved ? 'Save account again' : 'Save account';
    $('discord-account-save-status').textContent = s?.account_save_error ? 'Account could not be saved: ' + s.account_save_error :
      s?.auto_login_saved ? 'Saved on this PS5. Automatic sign-in is enabled.' :
      legacyVault ? 'A saved account needs its passphrase once. Restoring it enables automatic sign-in.' :
      linked ? 'Connected for this session. Save the account to sign in automatically next time.' :
      'No account saved on this PS5. Link Discord above to connect.';
    const destinationReady = linked && !channelLoading && displayedGuild === $('discord-server').value && Boolean($('discord-channel').value);
    $('discord-start').disabled = busy || state !== 'ready' || !destinationReady || s?.capture?.running === false;
    $('discord-stop').disabled = busy || !activeStates.has(state);
    $('discord-server').disabled = !linked || locked;
    $('discord-channel').disabled = !linked || locked || channelLoading;
    $('discord-link').disabled = busy || transitional.has(state);
    $('discord-remember').disabled = busy || transitional.has(state);
    $('discord-logout').disabled = busy;
    $('discord-vault-save').disabled = busy;
    $('discord-vault-forget').disabled = busy || transitional.has(state);
    $('discord-vault-unlock').disabled = busy || transitional.has(state);
    const guild = $('discord-server').value;
    destinationPicker?.render({account: channelAccount, guilds: s?.guilds || [], guild,
      channel: $('discord-channel').value, channels: channelCache.get(guild)?.entries || (!guild ? s?.dms || [] : []),
      serverDisabled: $('discord-server').disabled, channelDisabled: $('discord-channel').disabled,
      loading: channelLoading, error: channelError});
  }
  function options(select, entries, placeholder) {
    const choices = (entries || []).filter(entry => entry && typeof entry.id === 'string')
      .map(entry => [entry.id, String(entry.name || entry.id)]);
    const signature = JSON.stringify([placeholder, choices]);
    if (optionSignatures.get(select) === signature) return;
    optionSignatures.set(select, signature);
    const old = select.value;
    while (select.firstChild) select.removeChild(select.firstChild);
    select.add(new Option(placeholder, ''));
    for (const [id, name] of choices) select.add(new Option(name, id));
    for (const option of select.options) if (option.value === old) { select.value = old; break; }
  }
  function hasOption(select, value) {
    for (const option of select.options) if (option.value === value) return true;
    return false;
  }
  function restoreDestination(s) {
    if (destinationDirty || !s?.account || s.state !== 'streaming') return;
    const target = s.stream?.channel_id ? s.stream : null;
    if (!target || typeof target.channel_id !== 'string' || !target.channel_id ||
        (target.guild_id !== null && target.guild_id !== undefined && typeof target.guild_id !== 'string')) return;
    const guild = target.guild_id || '', channel = target.channel_id;
    const key = JSON.stringify([channelAccount, guild, channel]);
    if (key === restoredDestination) return;
    pendingDestination = {key, guild, channel};
    // Guild metadata may arrive after the first status response. Never invent
    // options or fetch a room until its server is actually available locally.
    if (hasOption($('discord-server'), guild)) $('discord-server').value = guild;
  }
  function finishDestinationRestore(guild) {
    const target = pendingDestination;
    if (!target || destinationDirty || target.guild !== guild || $('discord-server').value !== guild) return;
    if (!hasOption($('discord-channel'), target.channel)) {
      $('discord-channel').value = '';
      return;
    }
    $('discord-channel').value = target.channel;
    restoredDestination = target.key;
    pendingDestination = null;
  }
  function videoControls() {
    const locked = !videoConfig || videoApplying || videoConfig.profile_changing || videoRecording || Boolean(videoConfig.capture_error);
    $('discord-video-height').disabled = Boolean(locked);
    $('discord-video-fps').disabled = Boolean(locked);
    $('discord-video-apply').disabled = Boolean(locked || !videoDirty || !$('discord-video-height').value || !$('discord-video-fps').value);
  }
  function renderVideo() {
    if (!videoConfig) { videoControls(); return; }
    const c = videoConfig;
    const heights = (Array.isArray(c.resolutions) ? c.resolutions : []).filter(h => Number.isInteger(h) && h > 0);
    const rates = (Array.isArray(c.framerates) ? c.framerates : [c.fps]).filter(f => Number.isInteger(f) && f > 0);
    options($('discord-video-height'), heights.map(h => ({id: String(h), name: h + 'p'})), 'Choose resolution');
    options($('discord-video-fps'), rates.map(f => ({id: String(f), name: f + ' fps'})), 'Choose frame rate');
    if (!videoDirty) {
      $('discord-video-height').value = String(c.profile_changing ? c.requested_height || c.height : c.height);
      $('discord-video-fps').value = String(c.profile_changing ? c.requested_fps || c.fps : c.fps);
    }
    let text = 'Active PS5 capture: ' + c.height + 'p at ' + c.fps + ' fps.';
    if (c.capture_error) text = 'Video capture stopped: ' + c.capture_error + ' Reload PBS to retry capture.';
    else if (c.profile_error) text = (c.profile_error_height || c.requested_height || c.height) + 'p at ' + (c.profile_error_fps || c.requested_fps || c.fps) + ' fps could not start. Restored ' + c.height + 'p at ' + c.fps + ' fps.';
    else if (c.profile_changing || videoApplying) text = 'Applying video settings; capture and preview will reconnect…';
    else if (videoRecording) text = 'Stop recording before changing PS5 capture settings.';
    else if (c.profile_fallback_height) text = c.profile_fallback_height + 'p is unavailable with the current console video output. Using ' + c.height + 'p at ' + c.fps + ' fps; bitrate unchanged.';
    $('discord-video-status').textContent = c.capture_error ? text : videoError || text;
    videoControls();
  }
  function refreshVideo(force = false) {
    if (videoPoll) return videoPoll;
    const interval = videoConfig?.profile_changing ? 1000 : 5000;
    if (!force && Date.now() - videoLastPoll < interval) return Promise.resolve();
    videoLastPoll = Date.now();
    const revision = videoRevision;
    videoPoll = Promise.all([localRequest('/config.json'), localRequest('/status.json')]).then(([config, status]) => {
      if (revision !== videoRevision) return;
      if (!config || !Number.isInteger(config.height) || !Number.isInteger(config.fps) || typeof status?.recording !== 'boolean')
        throw new Error('Console returned invalid capture settings.');
      videoConfig = config; videoRecording = status.recording;
      renderVideo();
    }).catch(e => { if (revision === videoRevision) { videoError = e.message; $('discord-video-status').textContent = videoError; videoControls(); } })
      .finally(() => { videoPoll = null; });
    return videoPoll;
  }
  async function applyVideo() {
    if ($('discord-video-apply').disabled) return;
    const height = $('discord-video-height').value, fps = $('discord-video-fps').value;
    if (!videoConfig.resolutions?.includes(Number(height)) || !(videoConfig.framerates || [videoConfig.fps]).includes(Number(fps))) return;
    videoApplying = true; videoError = ''; videoRevision++; renderVideo();
    try {
      await localRequest('/capture/profile?height=' + encodeURIComponent(height) + '&fps=' + encodeURIComponent(fps), 'POST');
      videoDirty = false;
      // Ignore config requests begun before Apply, then request authoritative
      // active/requested settings so completion and rollback both render.
      if (videoPoll) await videoPoll;
      await refreshVideo(true);
    } catch (e) { videoError = e.message; }
    finally { videoApplying = false; renderVideo(); }
  }
  function validActivity(value) {
    return value && typeof value.enabled === 'boolean' && (value.game === null ||
      (value.game && typeof value.game.title_id === 'string' && typeof value.game.name === 'string'));
  }
  function renderActivity(value, revision = activityRevision) {
    if (activityPending || revision !== activityRevision) return;
    if (!validActivity(value)) return;
    activity = value;
    $('discord-activity-enabled').checked = activity.enabled;
    let text = !activity.enabled ? 'Game activity is off.' : !lastStatus?.account ? 'Connect your Discord account to share the running PS5 game.' :
      activity.game ? 'Running PS5 game: ' + (activity.game.name || activity.game.title_id) : 'No PS5 game detected. Discord activity stays empty.';
    $('discord-activity-status').textContent = activityError || text;
  }
  async function loadActivity() {
    const revision = activityRevision;
    try {
      const value = await call('activity');
      if (!validActivity(value)) throw new Error('Console returned invalid game activity settings.');
      renderActivity(value, revision);
    } catch (e) { if (revision === activityRevision && !activityPending) { activityError = e.message; activityLoadError = true; $('discord-activity-status').textContent = activityError; } }
  }
  async function changeActivity() {
    if (activityPending) return;
    const previous = activity, enabled = Boolean($('discord-activity-enabled').checked);
    activityPending = true; activityRevision++; activityError = ''; activityLoadError = false;
    $('discord-activity-enabled').disabled = true;
    $('discord-activity-status').textContent = 'Saving game activity setting…';
    try {
      const value = await call('activity', {enabled});
      if (!validActivity(value)) throw new Error('Console returned invalid game activity settings.');
      activity = value;
    } catch (e) { activity = previous; activityError = e.message; }
    finally {
      activityPending = false; activityRevision++;
      $('discord-activity-enabled').disabled = false;
      renderActivity(activity);
    }
  }
  function renderQr(value) {
    if (value === qrValue) return;
    qrValue = value; const qrHost = $('discord-qr'); while (qrHost.firstChild) qrHost.removeChild(qrHost.firstChild); $('discord-qr-panel').hidden = true;
    if (!value) return;
    try {
      const url = new URL(value);
      if (url.protocol !== 'https:' || url.hostname !== 'discord.com' || value.length > 4096) throw new Error('Console returned an invalid Discord linking URL.');
      const qr = qrcodegen.QrCode.encodeText(value, qrcodegen.QrCode.Ecc.MEDIUM), border = 4;
      const ns = 'http://www.w3.org/2000/svg', svg = document.createElementNS(ns, 'svg');
      svg.setAttribute('viewBox', `0 0 ${qr.size + border * 2} ${qr.size + border * 2}`);
      const background = document.createElementNS(ns, 'rect'); background.setAttribute('width', '100%'); background.setAttribute('height', '100%'); background.setAttribute('fill', '#fff'); svg.append(background);
      const path = document.createElementNS(ns, 'path'); let data = '';
      for (let y = 0; y < qr.size; y++) for (let x = 0; x < qr.size; x++) if (qr.getModule(x, y)) data += `M${x + border},${y + border}h1v1h-1z `;
      path.setAttribute('d', data); path.setAttribute('fill', '#000'); svg.setAttribute('shape-rendering', 'crispEdges'); svg.append(path);
      $('discord-qr').append(svg); $('discord-qr-panel').hidden = false;
    } catch (e) { fail(e.message); }
  }
  function render(s, activityVersion) {
    lastStatus = s;
    const accountKey = s?.account ? String(s.account.id || s.account.username || JSON.stringify(s.account)) : '';
    if (accountKey !== channelAccount) {
      channelAccount = accountKey; accountEpoch++; channelCache.clear();
      channelLoading = false; displayedGuild = null; channelError = '';
      destinationDirty = false; pendingDestination = null; restoredDestination = '';
      $('discord-server').value = '';
      options($('discord-channel'), [], 'Choose a destination');
    }
    const state = s?.state;
    note(states[state] || state || 'Unknown state');
    fail(s?.error || channelError);
    renderQr(s?.qr_url || null);
    renderVerification(s);
    if (validActivity(s?.activity)) {
      if (!activityPending && activityVersion === activityRevision && activityLoadError) { activityError = ''; activityLoadError = false; }
      renderActivity(s.activity, activityVersion);
      if (!activityPending && activityVersion === activityRevision) activityRevision++;
    }
    else renderActivity(activity, activityVersion);
    const account = s?.account;
    options($('discord-server'), s?.guilds, 'Direct messages');
    restoreDestination(s);
    const who = account?.username ? `Linked as ${account.username}` : '';
    const stats = s?.stream || {};
    const streaming = activeStates.has(state);
    $('discord-stream-info').textContent = streaming ? `${who ? who + ' · ' : ''}${stats.width || 0}×${stats.height || 0} @ ${stats.fps || 0} fps · ${Number(stats.video_packets || 0).toLocaleString()} video / ${Number(stats.audio_packets || 0).toLocaleString()} audio packets · ${(Number(stats.bytes || 0) / 1048576).toFixed(1)} MiB sent${stats.dave_ready ? ' · encrypted' : ' · waiting for encryption'}` : who;
    controls(s);
  }
  function tick() {
    if (polling) return polling;
    const current = ++generation;
    const activityVersion = activityRevision;
    polling = (async () => {
      try {
        const s = await call('status');
        if (current !== generation || busy) return;
        render(s, activityVersion);
        if (s?.account) void loadChannels(s);
      } catch (e) { if (current === generation) { note('Console unavailable'); fail(e.message || String(e)); } }
    })().finally(() => { polling = null; });
    return polling;
  }
  async function loadChannels(s, explicit = false) {
    if (!s?.account || !channelAccount) return;
    const guild = $('discord-server').value, epoch = accountEpoch;
    let entry = channelCache.get(guild);
    // DM choices already arrive in local status; only the selected server
    // needs a remote lookup. Remember failures until the user selects again.
    if (!guild) { entry = {entries: s.dms || []}; channelCache.set(guild, entry); }
    if (entry && !(explicit && entry.error)) {
      channelLoading = Boolean(entry.pending);
      channelError = entry.error || '';
      if (entry.entries) {
        options($('discord-channel'), entry.entries, 'Choose a destination'); displayedGuild = guild;
        finishDestinationRestore(guild);
      }
      fail(s.error || channelError); controls(s);
      return;
    }
    entry = {pending: true}; channelCache.set(guild, entry);
    const current = () => epoch === accountEpoch && channelCache.get(guild) === entry && $('discord-server').value === guild;
    channelLoading = true; channelError = ''; controls(s);
    try {
      const entries = await call('channels', {guild_id: guild});
      if (!Array.isArray(entries)) throw new Error('Console returned an invalid channel list.');
      entry.entries = entries;
      if (!current()) return;
      options($('discord-channel'), entries, 'Choose a destination'); displayedGuild = guild;
      finishDestinationRestore(guild);
    } catch (e) {
      entry.error = e.message || String(e);
      if (current()) { channelError = entry.error; fail(channelError); }
    } finally {
      entry.pending = false;
      if (current()) { channelLoading = false; controls(lastStatus); }
    }
  }
  async function act(path, body, button) {
    if (busy) return false;
    let succeeded = false, actionError = '';
    busy = true; const previous = button?.textContent;
    if (button) { button.disabled = true; button.textContent = 'Working…'; }
    try { await call(path, body); succeeded = true; await tick(); } catch (e) { actionError = e.message || String(e); fail(actionError); }
    finally { busy = false; if (button) { button.disabled = false; if (previous) button.textContent = previous; } await tick(); }
    if (actionError) fail(actionError);
    return succeeded;
  }
  $('discord-link').addEventListener('click', () => act('link', {remember: Boolean($('discord-remember').checked)}, $('discord-link')));
  $('discord-video-height').addEventListener('change', () => { videoDirty = true; videoControls(); });
  $('discord-video-fps').addEventListener('change', () => { videoDirty = true; videoControls(); });
  $('discord-video-apply').addEventListener('click', applyVideo);
  $('discord-activity-enabled').addEventListener('change', changeActivity);
  $('discord-verification-show').addEventListener('click', showVerification);
  $('discord-verification-cancel').addEventListener('click', () => {
    if (verification) {
      cancelledVerification = verification.id;
      removeVerificationWidget(verification); verification = null;
      $('discord-verification-show').disabled = true;
      verificationNote('Cancel requested. If login does not stop, select Cancel login again.');
    }
    return act('cancel-link', {}, $('discord-verification-cancel'));
  });
  $('discord-start').addEventListener('click', () => act('start', {channel_id: $('discord-channel').value, guild_id: $('discord-server').value || null}, $('discord-start')));
  $('discord-stop').addEventListener('click', () => act('stop', {}, $('discord-stop')));
  $('discord-logout').addEventListener('click', () => act('logout', {forget: false}, $('discord-logout')));
  $('discord-vault-forget').addEventListener('click', () => act('logout', {forget: true}, $('discord-vault-forget')));
  $('discord-server').addEventListener('change', () => { destinationDirty = true; pendingDestination = null; return loadChannels(lastStatus, true); });
  $('discord-channel').addEventListener('change', () => { destinationDirty = true; pendingDestination = null; controls(lastStatus); });
  $('discord-vault-save').addEventListener('click', () => act('vault', {automatic: true}, $('discord-vault-save')));
  $('discord-vault-unlock').addEventListener('click', () => act('unlock', {passphrase: $('discord-passphrase').value, remember: true}, $('discord-vault-unlock')).then(success => { if (success) $('discord-passphrase').value = ''; }));
  tick();
  setInterval(tick, 2000);
  videoControls();
  refreshVideo();
  loadActivity();
  setInterval(refreshVideo, 1000);
})();
