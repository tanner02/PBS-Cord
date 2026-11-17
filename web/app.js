/* SPDX-License-Identifier: GPL-3.0-or-later */
'use strict';
(() => {
  const $ = id => document.getElementById(id);
  let pairing = '', status = null, busy = false, channelLoading = false, qrValue = '', guildSignature = '', dmSignature = '', generation = 0, actionError = '';
  let requestQueue = Promise.resolve();
  try { pairing = sessionStorage.getItem('pbsd-pair') || ''; } catch (_) { /* Session storage may be disabled. */ }
  const states = {disconnected:'Disconnected',linking:'Linking account',connecting:'Connecting',ready:'Ready',joining:'Joining voice',negotiating:'Negotiating encryption',streaming:'Streaming',error:'Console error'};
  const activeStates = new Set(['joining','negotiating','streaming']);
  const transitional = new Set(['linking','connecting','joining','negotiating']);
  function error(message) { $('error').textContent = message || ''; $('error').hidden = !message; }
  function api(path, body) {
    const code = pairing;
    const run = async () => {
      const response = await fetch(`/api/${path}`, {method:body === undefined ? 'GET' : 'POST',cache:'no-store',credentials:'same-origin',headers:{'X-PBSD-Pair':code,...(body === undefined ? {} : {'Content-Type':'application/json'})},body:body === undefined ? undefined : JSON.stringify(body),signal:AbortSignal.timeout(15000)});
      let result;
      const raw = await response.text();
      try { result = raw ? JSON.parse(raw) : {}; } catch (_) { throw new Error(`Console returned an invalid response (${response.status}).`); }
      if (!response.ok) { const failure = new Error(typeof result.error === 'string' ? result.error : `Console request failed (${response.status}).`); failure.status = response.status; throw failure; }
      return result;
    };
    const task = requestQueue.then(run,run); requestQueue = task.catch(() => {}); return task;
  }
  function controls() {
    const state = status?.state;
    const linked = Boolean(status?.account);
    const locked = busy || transitional.has(state) || activeStates.has(state);
    $('link').disabled = busy || linked || transitional.has(state) || activeStates.has(state);
    $('server').disabled = !linked || locked;
    $('channel').disabled = !linked || locked || channelLoading;
    $('start').disabled = busy || channelLoading || state !== 'ready' || !linked || !$('channel').value;
    $('stop').disabled = busy || !activeStates.has(state);
    $('logout').disabled = busy || transitional.has(state) || activeStates.has(state);
    for (const id of ['vault-form','unlock-form']) for (const element of $(id).elements) element.disabled = locked;
  }
  function options(select, entries, placeholder) {
    const old = select.value;
    select.replaceChildren(new Option(placeholder,''));
    for (const entry of entries) if (entry && typeof entry.id === 'string') select.add(new Option(String(entry.name || entry.id),entry.id));
    if ([...select.options].some(option => option.value === old)) select.value = old;
  }
  async function loadChannels() {
    const current = ++generation, guild = $('server').value;
    channelLoading = true; controls();
    try {
      const entries = guild ? await api('channels',{guild_id:guild}) : (status?.dms || []);
      if (current !== generation) return;
      if (!Array.isArray(entries)) throw new Error('Console returned an invalid channel list.');
      options($('channel'),entries,'Choose a destination');
      $('channel-note').textContent = entries.length ? 'Select the voice destination for this stream.' : 'No voice destinations available here.';
    } catch (e) { if (current === generation) { options($('channel'),[],'Channels unavailable'); actionError = e.message; error(actionError); } }
    finally { if (current === generation) { channelLoading = false; controls(); } }
  }
  function renderQr(value) {
    if (value === qrValue) return;
    qrValue = value; $('qr').replaceChildren(); $('qr-panel').hidden = true;
    if (!value) return;
    try {
      const url = new URL(value);
      if (url.protocol !== 'https:' || url.hostname !== 'discord.com' || url.username || url.password || value.length > 4096) throw new Error('Console returned an invalid Discord linking URL.');
      const qr = qrcodegen.QrCode.encodeText(value,qrcodegen.QrCode.Ecc.MEDIUM), border = 4;
      const ns = 'http://www.w3.org/2000/svg', svg = document.createElementNS(ns,'svg');
      svg.setAttribute('viewBox',`0 0 ${qr.size+border*2} ${qr.size+border*2}`); svg.setAttribute('role','img'); svg.setAttribute('aria-label','Scan to link your Discord account');
      const background = document.createElementNS(ns,'rect'); background.setAttribute('width','100%'); background.setAttribute('height','100%'); background.setAttribute('fill','#fff'); svg.append(background);
      const path = document.createElementNS(ns,'path'); let data = '';
      for (let y=0; y<qr.size; y++) for(let x=0; x<qr.size; x++) if(qr.getModule(x,y)) data += `M${x+border},${y+border}h1v1h-1z `;
      path.setAttribute('d',data); path.setAttribute('fill','#000'); svg.setAttribute('shape-rendering','crispEdges'); svg.append(path); $('qr').append(svg);
      $('qr-link').href = url.href; $('qr-panel').hidden = false;
    } catch (e) { actionError = e.message; error(actionError); }
  }
  const count = value => Number.isFinite(Number(value)) && Number(value) >= 0 ? Number(value).toLocaleString() : '0';
  const format = capture => capture?.width && capture?.height ? `${capture.width} × ${capture.height} · ${capture.fps || 0} fps` : '—';
  function render(next) {
    if (!next || typeof next !== 'object' || typeof next.state !== 'string') throw new Error('Console returned an invalid status.');
    status = next;
    $('pair-panel').hidden = true; $('dashboard').hidden = false;
    $('state').textContent = states[next.state] || 'Unknown console state';
    $('account').textContent = next.account ? `${next.account.username} · ${next.account.id}` : 'No account linked';
    $('link-progress').hidden = !['linking','connecting'].includes(next.state);
    $('logout').hidden = !next.account && !next.vault_saved;
    $('unlock-form').hidden = Boolean(next.account) || !next.vault_saved;
    $('vault-form').hidden = !next.account || Boolean(next.vault_saved);
    $('vault-state').textContent = next.vault_saved ? 'Encrypted account saved on this console.' : 'Account is not saved on this console.';
    const guilds = Array.isArray(next.guilds) ? next.guilds : [], dms = Array.isArray(next.dms) ? next.dms : [];
    const signature = JSON.stringify(guilds);
    if (signature !== guildSignature) { guildSignature = signature; options($('server'),guilds,'Direct messages'); void loadChannels(); }
    const ds = JSON.stringify(dms);
    if (ds !== dmSignature) { dmSignature = ds; if (!$('server').value) void loadChannels(); }
    if (!next.account) { ++generation; channelLoading = false; options($('channel'),[],'Link an account first'); }
    renderQr(next.account ? '' : (typeof next.qr_url === 'string' ? next.qr_url : ''));
    const capture = next.capture || {}, stream = next.stream || {};
    $('capture').textContent = capture.running ? 'Active' : 'Inactive'; $('format').textContent = format(capture); $('dropped').textContent = count(capture.dropped);
    $('dave').textContent = stream.dave_ready ? 'Ready' : 'Not ready'; $('video').textContent = count(stream.video_packets); $('audio').textContent = count(stream.audio_packets); $('bytes').textContent = count(stream.bytes); $('stream-format').textContent = format(stream);
    $('destination').textContent = stream.channel_id ? `Active channel: ${stream.channel_id}${stream.guild_id ? ` · Server: ${stream.guild_id}` : ' · Direct message'}` : 'No active stream';
    $('updated').textContent = `Console updated ${new Date().toLocaleTimeString()}`;
    error((typeof next.error === 'string' && next.error) || actionError); controls();
  }
  async function refresh() {
    const code = pairing;
    if (!code) return;
    try { const next = await api('status'); if (pairing === code) render(next); }
    catch (e) { if (pairing !== code) return; error(e.message); $('state').textContent = 'Console unavailable'; status = null; controls(); if (e.status === 401 || e.status === 403) { $('pair-panel').hidden = false; $('dashboard').hidden = true; } }
  }
  async function action(path, body, after) {
    if (busy) return;
    busy = true; actionError = ''; error(''); controls();
    try { await api(path,body); if (after) after(); await refresh(); }
    catch (e) { actionError = e.message; error(actionError); }
    finally { busy = false; controls(); }
  }
  $('pair-form').addEventListener('submit',async event => { event.preventDefault(); pairing = $('pair-code').value.trim(); $('pair-code').value = ''; if (!pairing) return; try { sessionStorage.setItem('pbsd-pair',pairing); } catch (_) {} await refresh(); });
  $('link').addEventListener('click',() => action('link',{}));
  $('server').addEventListener('change',() => { options($('channel'),[],'Loading channels…'); void loadChannels(); });
  $('channel').addEventListener('change',controls);
  $('start').addEventListener('click',() => action('start',{channel_id:$('channel').value,guild_id:$('server').value || null}));
  $('stop').addEventListener('click',() => action('stop',{}));
  $('logout').addEventListener('click',() => action('logout',{forget:true}));
  for (const [form,path,input] of [['unlock-form','unlock','unlock-pass'],['vault-form','vault','vault-pass']]) $(form).addEventListener('submit',event => { event.preventDefault(); const passphrase = $(input).value; $(input).value = ''; if (passphrase) void action(path,{passphrase}); });
  $('unpair').addEventListener('click',() => { pairing = ''; status = null; ++generation; try { sessionStorage.removeItem('pbsd-pair'); } catch (_) {} $('dashboard').hidden = true; $('pair-panel').hidden = false; $('state').textContent = 'Not paired'; error(''); });
  async function poll() { if (pairing && !busy) await refresh(); setTimeout(poll,1000); }
  void poll();
})();
