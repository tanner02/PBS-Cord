/* SPDX-License-Identifier: GPL-3.0-or-later */
'use strict';
// Presentation only: selections use the existing channel cache and handlers.
window.pbsCreateDestinationPicker = handlers => {
  const $ = id => document.getElementById(id);
  const serverList = $('destination-servers'), channelList = $('destination-channels');
  if (!serverList || !channelList) return null;
  const serverSearch = $('destination-server-search'), channelSearch = $('destination-channel-search');
  let state = {}, serverSignature = '', channelSignature = '', account = null;
  let serverRows = [], channelRows = [], groups = [];
  const collapsed = new Set();
  const voicePath = 'M11 5 6 9H3v6h3l5 4V5Zm4 3a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14';
  const dmPath = 'M5 4h14a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H9l-6 3V6a2 2 0 0 1 2-2Z';
  function icon(path) {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 24 24'); svg.setAttribute('aria-hidden', 'true');
    const line = document.createElementNS(svg.namespaceURI, 'path');
    line.setAttribute('d', path); svg.append(line); return svg;
  }
  function node(tag, className, text) {
    const element = document.createElement(tag); element.className = className;
    if (text !== undefined) element.textContent = text;
    return element;
  }
  function row(id, name, className, choose) {
    const button = node('button', className); button.type = 'button';
    button.dataset.destination = id; button.title = name; button.onclick = () => choose(id);
    return button;
  }
  function serverIcon(guild) {
    const badge = node('span', 'server-icon');
    badge.setAttribute('aria-hidden', 'true');
    if (!guild.id) { badge.append(icon(dmPath)); return badge; }
    const initials = guild.name.trim().split(/\s+/).slice(0, 2).map(word => Array.from(word)[0] || '').join('').toUpperCase();
    badge.append(node('span', 'server-initials', initials || '?'));
    // Construct public Discord CDN URLs; never accept an arbitrary image URL.
    if (/^\d{1,32}$/.test(guild.id) && /^(a_)?[0-9a-f]{32}$/.test(guild.icon || '')) {
      const img = document.createElement('img');
      img.alt = ''; img.loading = 'lazy'; img.decoding = 'async'; img.referrerPolicy = 'no-referrer';
      img.src = `https://cdn.discordapp.com/icons/${guild.id}/${guild.icon}.webp?size=64`;
      img.onerror = () => img.remove(); badge.append(img);
    }
    return badge;
  }
  function rebuildServers(guilds) {
    const fragment = document.createDocumentFragment(); serverRows = [];
    for (const guild of [{id: '', name: 'Direct messages'}, ...guilds]) {
      const button = row(guild.id, guild.name, 'destination-server', handlers.selectServer);
      button.append(serverIcon(guild), node('span', 'destination-name', guild.name));
      fragment.append(button); serverRows.push({button, entry: guild});
    }
    serverList.replaceChildren(fragment);
  }
  function rebuildChannels(channels) {
    channelRows = []; groups = [];
    const map = new Map(), fragment = document.createDocumentFragment();
    for (const entry of channels) {
      const id = typeof entry.category_id === 'string' ? entry.category_id : '';
      if (!map.has(id)) map.set(id, {id, name: id && typeof entry.category_name === 'string' ? entry.category_name : state.guild ? 'Voice channels' : 'Direct calls',
        position: Number.isInteger(entry.category_position) ? entry.category_position : -1, entries: []});
      map.get(id).entries.push(entry);
    }
    for (const group of [...map.values()].sort((a, b) => a.position - b.position)) {
      const section = node('div', 'channel-category');
      const header = node('button', 'channel-category-heading'); header.type = 'button';
      header.append(node('span', 'category-chevron', '⌄'), node('span', '', group.name));
      const list = node('div', 'channel-category-rows'), key = JSON.stringify([state.guild, group.id]);
      header.onclick = () => { if (collapsed.has(key)) collapsed.delete(key); else collapsed.add(key); filter(); };
      section.append(header, list); fragment.append(section);
      const members = [];
      for (const entry of group.entries.sort((a, b) => (Number.isInteger(a.position) ? a.position : 0) - (Number.isInteger(b.position) ? b.position : 0))) {
        const button = row(entry.id, entry.name, 'destination-channel', handlers.selectChannel);
        button.append(icon(state.guild ? voicePath : dmPath), node('span', 'destination-name', entry.name));
        list.append(button); const record = {button, entry}; members.push(record); channelRows.push(record);
      }
      groups.push({section, header, list, key, members});
    }
    channelList.replaceChildren(fragment);
  }
  function filter() {
    const serverTerm = serverSearch.value.trim().toLocaleLowerCase();
    for (const {button, entry} of serverRows) button.hidden = !entry.name.toLocaleLowerCase().includes(serverTerm);
    $('destination-server-empty').hidden = serverRows.some(row => !row.button.hidden);
    const term = channelSearch.value.trim().toLocaleLowerCase(); let matches = 0;
    for (const group of groups) {
      let visible = 0;
      for (const {button, entry} of group.members) {
        button.hidden = !entry.name.toLocaleLowerCase().includes(term); if (!button.hidden) visible++;
      }
      group.section.hidden = visible === 0;
      const expanded = Boolean(term) || !collapsed.has(group.key);
      group.list.hidden = !expanded; group.header.setAttribute('aria-expanded', String(expanded)); matches += visible;
    }
    const message = state.loading ? 'Loading voice channels…' : state.error ||
      (!channelRows.length ? (state.guild ? 'No voice channels available.' : 'No direct calls available.') : !matches ? 'No matching channels.' : '');
    $('destination-channel-status').textContent = message;
    $('destination-channel-status').hidden = !message;
    $('destination-retry').hidden = !state.error;
    $('destination-retry').disabled = Boolean(state.serverDisabled || state.loading);
  }
  function keyboard(event) {
    if (!['ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) return;
    const buttons = [...event.currentTarget.querySelectorAll('button[data-destination]')]
      .filter(button => !button.disabled && !button.hidden && !button.closest('[hidden]'));
    if (!buttons.length) return;
    const index = buttons.indexOf(document.activeElement);
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1 :
      (index + (event.key === 'ArrowDown' ? 1 : -1) + buttons.length) % buttons.length;
    event.preventDefault(); buttons[next].focus();
  }
  serverList.addEventListener('keydown', keyboard); channelList.addEventListener('keydown', keyboard);
  serverSearch.addEventListener('input', filter); channelSearch.addEventListener('input', filter);
  $('destination-retry').onclick = () => handlers.retry();
  return {render(value) {
    if (account !== value.account) {
      account = value.account; collapsed.clear(); serverSearch.value = channelSearch.value = '';
      serverSignature = channelSignature = '';
    }
    if (state.guild !== value.guild) channelSearch.value = '';
    state = value;
    const guilds = (value.guilds || []).filter(g => typeof g.id === 'string' && g.id && typeof g.name === 'string');
    const signature = JSON.stringify(guilds);
    if (signature !== serverSignature) { serverSignature = signature; rebuildServers(guilds); }
    const channels = (value.channels || []).filter(c => typeof c.id === 'string' && c.id && typeof c.name === 'string');
    const nextChannels = JSON.stringify([value.guild, channels]);
    if (nextChannels !== channelSignature) { channelSignature = nextChannels; rebuildChannels(channels); }
    for (const {button, entry} of serverRows) {
      button.disabled = Boolean(value.serverDisabled); button.setAttribute('aria-pressed', String(entry.id === value.guild));
    }
    for (const {button, entry} of channelRows) {
      button.disabled = Boolean(value.channelDisabled); button.setAttribute('aria-pressed', String(entry.id === value.channel));
    }
    const guild = guilds.find(g => g.id === value.guild), channel = channels.find(c => c.id === value.channel);
    $('destination-channel-title').textContent = value.guild ? guild?.name || 'Voice channels' : 'Direct messages';
    $('destination-selection').textContent = channel ? `Selected: ${guild?.name || 'Direct messages'} / ${channel.name}` : 'Choose a server and a voice channel to stream.';
    filter();
  }};
};
