// NestDrive web interface.
//
// Plain ES modules, no build step. The whole point of this file is the virtual
// scroller: a 5000-file folder used to be 1.5 MB of server-rendered markup and
// 5000 DOM nodes, which a phone struggles to lay out. Here the server sends JSON
// in pages and only the rows actually on screen become elements.

const PAGE_SIZE = 200;

// Kept in step with app.css. The scroller works out which rows are visible by
// arithmetic rather than by measuring elements, so these must match the stylesheet.
const CARD_HEIGHT = () => readPixels('--card-h');
const CARD_WIDTH  = () => readPixels('--card-w');
const GAP         = () => readPixels('--gap');
const PADDING     = () => readPixels('--pad');

/** Rows rendered above and below the viewport, so scrolling does not show gaps. */
const OVERSCAN_ROWS = 2;

const dom = {
  scroller: document.getElementById('scroller'),
  sizer:    document.getElementById('sizer'),
  grid:     document.getElementById('grid'),
  crumbs:   document.getElementById('crumbs'),
  count:    document.getElementById('count'),
  back:     document.getElementById('back'),
  whoami:   document.getElementById('whoami'),
  signout:  document.getElementById('signout'),
  status:   document.getElementById('status'),
  overlay:  document.getElementById('overlay'),
  ovTitle:  document.getElementById('ov-title'),
  ovImg:    document.getElementById('ov-img'),
  ovVid:    document.getElementById('ov-vid'),
  ovMsg:    document.getElementById('ov-msg'),
  ovCount:  document.getElementById('ov-counter'),
  ovDl:     document.getElementById('ov-dl'),
};

const state = {
  path: '/',
  total: 0,
  entries: [],          // Sparse: index -> entry, filled as pages arrive.
  requestedPages: new Set(),
  media: [],            // Indices into `entries` that can be previewed.
  byUrl: new Map(),     // entry.u -> index, so a status reply can find its tile.
  current: -1,
  columns: 1,
  renderedRange: [-1, -1],

  // Set when someone chose "play the original" for the open video. An optimised
  // rendition finishing must not then yank the file out from under them mid-watch.
  playingOriginal: false,

  // Who the server says we are, and what it says we may do here. Advisory only —
  // it drives what the interface offers, never what the server allows.
  perm: null,
};

function readPixels(name) {
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name);
  return parseInt(raw, 10) || 0;
}

function statusMessage(text) {
  if (!text) {
    dom.status.hidden = true;
    return;
  }
  dom.status.textContent = text;
  dom.status.hidden = false;
}

function humanSize(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1048576) return `${Math.round(bytes / 1024)} KB`;
  if (bytes < 1073741824) return `${Math.round(bytes / 1048576)} MB`;
  return `${(bytes / 1073741824).toFixed(1)} GB`;
}

// ── Data ────────────────────────────────────────────────────────────────────

/**
 * Fetches one page. Returns silently if that page is already loaded or in flight,
 * which matters because scrolling fires far more often than pages are needed.
 */
async function loadPage(pageIndex) {
  if (state.requestedPages.has(pageIndex)) return;
  state.requestedPages.add(pageIndex);

  const offset = pageIndex * PAGE_SIZE;
  const url = `/api/list?path=${encodeURIComponent(state.path)}`
            + `&offset=${offset}&limit=${PAGE_SIZE}`;

  let response;
  try {
    response = await fetch(url);
  } catch {
    state.requestedPages.delete(pageIndex);
    statusMessage('Cannot reach the server.');
    return;
  }

  // A session that expired while this page was open. The gate answers 401 to a
  // fetch rather than redirecting it, because redirecting an XHR to an HTML page
  // surfaces as a JSON parse error instead of a usable signal — so the redirect has
  // to happen here, carrying where we were so signing in comes back to it.
  if (response.status === 401) {
    const next = encodeURIComponent(location.pathname + location.search);
    location.replace(`/login?next=${next}`);
    return;
  }

  if (!response.ok) {
    state.requestedPages.delete(pageIndex);
    statusMessage(`Could not list this folder (${response.status}).`);
    return;
  }

  const data = await response.json();
  state.total = data.total;
  if (data.perm) state.perm = data.perm;

  data.entries.forEach((entry, i) => { state.entries[offset + i] = entry; });

  rebuildMediaIndex();
  adoptVideoJobs(data.entries);
  updateChrome();
  applySizer();
  render(true);
  statusMessage('');
}

/**
 * Media entries, in listing order, for previous/next in the overlay — and a lookup
 * from URL to index, so a video status reply can find the tile it belongs to without
 * re-scanning a five thousand entry folder every two seconds.
 */
function rebuildMediaIndex() {
  state.media = [];
  state.byUrl = new Map();
  for (let i = 0; i < state.entries.length; i++) {
    const entry = state.entries[i];
    if (!entry || entry.d) continue;

    state.byUrl.set(entry.u, i);
    if (entry.k === 'image' || entry.k === 'video')
      state.media.push(i);
  }
}

// ── Layout ──────────────────────────────────────────────────────────────────

function computeColumns() {
  const available = dom.scroller.clientWidth - PADDING() * 2;
  const stride = CARD_WIDTH() + GAP();
  return Math.max(1, Math.floor((available + GAP()) / stride));
}

/** Gives the scroll container the height the full folder would occupy. */
function applySizer() {
  state.columns = computeColumns();
  const rows = Math.ceil(state.total / state.columns);
  const height = rows * (CARD_HEIGHT() + GAP()) - GAP() + PADDING() * 2;
  dom.sizer.style.height = `${Math.max(0, height)}px`;
}

function card(entry, index) {
  const element = document.createElement(entry.d ? 'a' : 'div');
  element.className = 'card';
  element.dataset.index = String(index);

  if (entry.d) {
    element.href = joinPath(state.path, entry.n);
    element.addEventListener('click', (event) => {
      event.preventDefault();
      navigate(joinPath(state.path, entry.n));
    });
  }

  const thumb = document.createElement('div');
  thumb.className = 'thumb';

  if (entry.d) {
    thumb.textContent = '📁';
  } else if (entry.t) {
    const image = document.createElement('img');
    image.src = entry.t;
    image.alt = entry.n;
    image.loading = 'lazy';
    image.decoding = 'async';
    thumb.appendChild(image);
  } else if (entry.k === 'video') {
    thumb.textContent = '🎬';
  } else if (entry.k === 'image') {
    // An image with no thumbnail URL: a format the server cannot decode, HEIC
    // being the common one. Shown as a plain icon rather than a broken image.
    thumb.textContent = '🖼';
    thumb.classList.add('noprev');
  } else {
    thumb.textContent = '📄';
  }

  paintVideoBadge(thumb, entry);

  const info = document.createElement('div');
  info.className = 'info';

  const name = document.createElement('b');
  // textContent, never innerHTML: a filename is untrusted input.
  name.textContent = entry.n;

  const meta = document.createElement('small');
  meta.textContent = entry.d ? 'Folder' : humanSize(entry.s ?? 0);

  info.append(name, meta);
  element.append(thumb, info);

  if (!entry.d) {
    element.addEventListener('click', () => {
      if (entry.k === 'image' || entry.k === 'video')
        openOverlay(index);
      else
        window.location.href = `${entry.u}?download=1`;
    });
  }

  return element;
}

/** Renders the visible window, and requests any pages it needs. */
function render(force = false) {
  const rowHeight = CARD_HEIGHT() + GAP();
  const scrollTop = dom.scroller.scrollTop;
  const viewport = dom.scroller.clientHeight;

  const firstRow = Math.max(0, Math.floor((scrollTop - PADDING()) / rowHeight) - OVERSCAN_ROWS);
  const lastRow = Math.ceil((scrollTop + viewport - PADDING()) / rowHeight) + OVERSCAN_ROWS;

  const first = firstRow * state.columns;
  const last = Math.min(state.total - 1, lastRow * state.columns - 1);

  if (!force && first === state.renderedRange[0] && last === state.renderedRange[1])
    return;
  state.renderedRange = [first, last];

  // Any page overlapping the window, so scrolling into unloaded territory fetches
  // rather than showing blanks.
  for (let page = Math.floor(first / PAGE_SIZE); page <= Math.floor(Math.max(0, last) / PAGE_SIZE); page++)
    loadPage(page);

  dom.grid.replaceChildren();

  // The grid is absolutely positioned at the first visible row's offset; the sizer
  // behind it provides the scroll height for everything not rendered.
  dom.grid.style.position = 'absolute';
  dom.grid.style.top = `${firstRow * rowHeight}px`;
  dom.grid.style.left = '0';
  dom.grid.style.right = '0';

  const fragment = document.createDocumentFragment();
  for (let i = first; i <= last; i++) {
    const entry = state.entries[i];
    if (!entry) continue;        // Page still loading.
    fragment.appendChild(card(entry, i));
  }
  dom.grid.appendChild(fragment);

  if (state.total === 0) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = 'This folder is empty.';
    dom.grid.appendChild(empty);
  }
}

// ── Navigation ──────────────────────────────────────────────────────────────

function joinPath(base, name) {
  return (base.endsWith('/') ? base : `${base}/`) + name;
}

function parentPath(path) {
  if (path === '/') return null;
  const trimmed = path.replace(/\/+$/, '');
  const cut = trimmed.lastIndexOf('/');
  return cut <= 0 ? '/' : trimmed.slice(0, cut);
}

function updateChrome() {
  dom.crumbs.textContent = state.path;
  dom.count.textContent = state.total === 1 ? '1 item' : `${state.total} items`;
  const parent = parentPath(state.path);
  dom.back.hidden = parent === null;
  if (parent !== null) dom.back.href = parent;

  // Taken from the listing rather than a separate /api/session call: the server
  // already resolved it for this folder, so asking again would be a second round
  // trip for an answer we hold.
  const who = state.perm && state.perm.user;
  dom.whoami.hidden = !who;
  dom.signout.hidden = !who;
  if (who) {
    dom.whoami.textContent = state.perm.role
      ? `${who} (${state.perm.role})`
      : who;
  }
}

/** Ends the session server-side, then returns to the login page. */
async function signOut() {
  try {
    await fetch('/api/logout', { method: 'POST' });
  } catch {
    // Clearing the cookie is the server's job, so a failed call means the session
    // may still be live. Going to the login page anyway is the honest outcome:
    // staying on a folder view that no longer loads would be worse.
  }
  location.replace('/login');
}

function navigate(path, replace = false) {
  state.path = path || '/';
  state.total = 0;
  state.entries = [];
  state.requestedPages.clear();
  state.media = [];
  state.byUrl.clear();
  state.renderedRange = [-1, -1];

  // Leaving the folder stops the watching, not the work. There are no tiles left to
  // update here, and coming back adopts whatever is still running from the listing.
  stopVideoPolling();
  dom.scroller.scrollTop = 0;
  dom.grid.replaceChildren();

  const url = encodeURI(state.path);
  if (replace) history.replaceState({ path: state.path }, '', url);
  else history.pushState({ path: state.path }, '', url);

  updateChrome();
  loadPage(0);
}

// ── Video optimisation tracking ─────────────────────────────────────────────
//
// Optimising a video takes minutes, so the work outlives the dialog that asked for
// it — and the thing watching it therefore has to as well. One tracker, read by
// both the tiles and the overlay, because the alternative is a dialog that polls
// for itself: close it and the progress disappears, with nothing in the folder to
// say which file the machine is busy with.

/** How often the tracker asks the server how a job is getting on. */
const VIDEO_POLL_MS = 2000;

/**
 * Most files polled per tick.
 *
 * The server transcodes one at a time, so this is normally one or two. The cap is
 * for the background pass, which can queue a whole library — a request per queued
 * file every two seconds would be a burst that says nothing the queue depth of the
 * first few does not already say.
 */
const VIDEO_POLL_MAX = 8;

/** entry.u -> last status seen, for videos still queued or running. */
const videoJobs = new Map();
let videoPollTimer = null;

/** The status endpoint for a listing URL. */
function videoQuery(url) {
  return `/api/video?path=${encodeURIComponent(decodeURIComponent(url))}`;
}

function stopVideoPolling() {
  if (videoPollTimer) {
    clearTimeout(videoPollTimer);
    videoPollTimer = null;
  }
  videoJobs.clear();
}

/** Starts or stops watching @p url, depending on whether its job is still live. */
function watchVideo(url, status) {
  if (status && (status.state === 'queued' || status.state === 'running')) {
    videoJobs.set(url, status);
    if (!videoPollTimer)
      videoPollTimer = setTimeout(pollVideoJobs, VIDEO_POLL_MS);
  } else {
    videoJobs.delete(url);
  }
}

/**
 * Picks up work the server is already doing.
 *
 * The listing carries each video's state, so a tile shows progress even when the
 * dialog that started it was closed, when the page was reloaded, or when the work
 * was started from somebody else's device.
 */
function adoptVideoJobs(entries) {
  for (const entry of entries) {
    if (!entry || entry.d || entry.k !== 'video') continue;
    if (entry.vs === 'queued' || entry.vs === 'running')
      watchVideo(entry.u, { state: entry.vs });
  }
}

/** One poll round. Reschedules itself while anything is still being worked on. */
async function pollVideoJobs() {
  videoPollTimer = null;

  const watched = [...videoJobs.keys()].slice(0, VIDEO_POLL_MAX);

  await Promise.all(watched.map(async (url) => {
    try {
      applyVideoStatus(url, await (await fetch(videoQuery(url))).json(), true);
    } catch {
      // Not worth a message. The next tick tries again, and the work is happening
      // on the server whether or not this page can see it.
    }
  }));

  if (videoJobs.size > 0 && !videoPollTimer)
    videoPollTimer = setTimeout(pollVideoJobs, VIDEO_POLL_MS);
}

/**
 * Records a status reply: updates the listing entry, repaints its tile, and — when
 * asked to — moves the overlay on if it happens to be open on that file.
 *
 * Every route into this — the listing, the POST that starts work, each poll — goes
 * through here, so a tile cannot end up disagreeing with the dialog.
 *
 * @param driveOverlay True for a reply to something the viewer asked for: a poll on
 *        work in flight, or the POST that started it. False for a first look, which
 *        playVideo() presents itself — otherwise merely asking about an unoptimised
 *        video would start playing the original nobody asked to play.
 */
function applyVideoStatus(url, status, driveOverlay = false) {
  if (!status) return;

  watchVideo(url, status);

  const index = state.byUrl.get(url);
  const entry = index === undefined ? null : state.entries[index];

  if (entry) {
    // Client-side fields, alongside the `vs` and `vo` the server sends: the listing
    // has no percentage in it, and render() has to be able to rebuild a tile from
    // the entry alone when it scrolls back into view.
    entry.vs = status.state;
    entry.vp = status.state === 'running' ? (status.percent ?? 0) : 0;
    entry.va = status.ahead ?? 0;
    if (status.state === 'ready' && status.url)
      entry.vo = status.url;

    repaintVideoBadge(index, entry);
  }

  const open = driveOverlay && !dom.overlay.hidden && state.current >= 0
    ? state.entries[state.current]
    : null;

  if (!open || open.u !== url || state.playingOriginal)
    return;

  if (status.state === 'ready' && status.url) {
    dom.ovMsg.textContent = 'Ready — starting…';
    startPlayback(open, status.url);
    return;
  }

  if (status.state === 'queued' || status.state === 'running') {
    offerOptimisation(open, status);
    return;
  }

  // A verdict instead of progress: assess() decided the original already streams
  // fine, or it cannot be transcoded at all. Either way the wait is over and the
  // original is what there is. Leaving "waiting to be optimised" on screen for a
  // job that will never report again would be a dead end.
  if (status.state === 'failed' && status.reason)
    statusMessage(`Could not optimise this video: ${status.reason}`);

  startPlayback(open, open.u);
}

/**
 * Puts the optimisation marker on a tile, or takes it off.
 *
 * Driven entirely from the entry, so a card rebuilt by the scroller looks the same
 * as one updated in place.
 */
function paintVideoBadge(thumb, entry) {
  thumb.querySelector('.vbadge')?.remove();
  thumb.querySelector('.vbar')?.remove();

  if (entry.d || entry.k !== 'video') return;

  const ready = entry.vs === 'ready' || !!entry.vo;
  const working = entry.vs === 'running' || entry.vs === 'queued';
  if (!ready && !working) return;

  const badge = document.createElement('span');
  badge.className = 'vbadge';

  if (ready) {
    badge.classList.add('vready');
    badge.textContent = '✓ HD';
    badge.title = 'Optimised — plays smoothly';
  } else if (entry.vs === 'running') {
    const percent = Math.max(0, Math.min(100, entry.vp ?? 0));

    badge.classList.add('vwork');
    badge.textContent = `${percent}%`;
    badge.title = 'Optimising now';

    const bar = document.createElement('span');
    bar.className = 'vbar';
    bar.style.width = `${percent}%`;
    thumb.appendChild(bar);
  } else {
    badge.classList.add('vwork');
    // Its place in the queue, counting itself, rather than a spinner: "3rd" is a
    // wait somebody can judge, and one job runs at a time so it is accurate.
    badge.textContent = entry.va > 0 ? `#${entry.va + 1}` : '…';
    badge.title = entry.va > 0
      ? `Waiting to be optimised — ${entry.va} ahead in the queue`
      : 'Waiting to be optimised';
  }

  thumb.appendChild(badge);
}

/** Repaints one rendered tile in place, without rebuilding the visible grid. */
function repaintVideoBadge(index, entry) {
  const thumb = dom.grid.querySelector(`.card[data-index="${index}"] .thumb`);
  if (thumb) paintVideoBadge(thumb, entry);
}

// ── Overlay ─────────────────────────────────────────────────────────────────

let pendingRetry = null;

function clearRetry() {
  if (pendingRetry) {
    clearTimeout(pendingRetry);
    pendingRetry = null;
  }
}

/**
 * Opens a preview. Videos are probed first: the server bounds how many transfers
 * run at once, so a busy server answers 503, and a bare <video> element would just
 * show a silent failure. Probing turns that into a visible wait.
 */
async function openOverlay(index) {
  const entry = state.entries[index];
  if (!entry) return;

  state.current = index;
  state.playingOriginal = false;
  clearRetry();

  dom.ovTitle.textContent = entry.n;
  dom.ovDl.href = `${entry.u}?download=1`;
  dom.ovDl.setAttribute('download', entry.n);

  const position = state.media.indexOf(index);
  dom.ovCount.textContent = position >= 0 ? `${position + 1}/${state.media.length}` : '';

  dom.ovImg.hidden = true;
  dom.ovVid.hidden = true;
  dom.ovMsg.hidden = true;
  dom.ovVid.pause();
  dom.ovVid.removeAttribute('src');
  dom.ovImg.removeAttribute('src');

  dom.overlay.hidden = false;

  if (entry.k === 'image') {
    // The medium preview when one exists, the original only as a fallback. This is
    // the difference between a few hundred kilobytes and several megabytes.
    dom.ovImg.src = entry.p || entry.u;
    dom.ovImg.hidden = false;
    return;
  }

  await playVideo(entry);
}

/**
 * Starts playback from a URL, after checking the server has a transfer slot free.
 *
 * The probe exists because the server bounds concurrent transfers, so a busy server
 * answers 503 and a bare <video> element would show a silent failure instead of a
 * visible wait.
 */
async function startPlayback(entry, url) {
  let response;
  try {
    response = await fetch(url, { method: 'HEAD' });
  } catch {
    dom.ovMsg.textContent = 'Cannot reach the server.';
    return;
  }

  if (response.status === 503) {
    const after = parseInt(response.headers.get('Retry-After') || '5', 10);
    dom.ovMsg.textContent =
      `Server busy — waiting for a free transfer slot (retrying in ${after}s)…`;
    pendingRetry = setTimeout(() => startPlayback(entry, url), after * 1000);
    return;
  }

  if (!response.ok) {
    dom.ovMsg.textContent = `Cannot play this file (${response.status}).`;
    return;
  }

  dom.ovMsg.hidden = true;
  dom.ovVid.src = url;
  dom.ovVid.hidden = false;
}

/**
 * Opens a video, preferring an optimised rendition and offering to make one.
 *
 * A phone shoots 4K HEVC at tens of megabits a second, which no home link carries in
 * real time — that is measured, not assumed, and it is why playback stutters on the
 * original however fast the server is. A 1080p rendition is roughly 1.3 Mbps, which
 * any link carries, and small enough that the browser keeps it so a second viewing
 * starts instantly.
 *
 * Making one costs minutes, so it is never started without being asked for.
 */
async function playVideo(entry) {
  dom.ovMsg.hidden = false;
  dom.ovMsg.textContent = 'Opening…';

  // Already optimised, and the listing said so — no extra round trip needed.
  if (entry.vo) {
    await startPlayback(entry, entry.vo);
    return;
  }

  let status = null;
  try {
    status = await (await fetch(videoQuery(entry.u))).json();
  } catch {
    // The status endpoint is an enhancement, not a dependency. If it cannot be
    // reached, the original is still playable and that is what matters.
  }

  if (!status || !status.available || status.state === 'notNeeded'
      || status.state === 'failed') {
    // Nothing to optimise, or it cannot be. Play the original and say why only when
    // there is something useful to say.
    if (status && status.state === 'failed' && status.reason)
      statusMessage(`Could not optimise this video: ${status.reason}`);

    await startPlayback(entry, entry.u);
    return;
  }

  if (status.state === 'ready' && status.url) {
    await startPlayback(entry, status.url);
    return;
  }

  // Queued, running, or not yet looked at. Offer the choice rather than deciding:
  // someone on a fast local network may well prefer the original immediately, and
  // someone on a phone over mobile data will not.
  //
  // Through the tracker, so that if this one is already being worked on its tile
  // starts showing progress from the moment the file was looked at. Tracking only —
  // what to show is decided here, by the call below.
  applyVideoStatus(entry.u, status);
  offerOptimisation(entry, status);
}

/**
 * Shows the "optimise or play anyway" choice.
 *
 * Rendering only. Progress arrives through applyVideoStatus(), which calls back in
 * here as each poll lands — so closing this dialog stops nothing, and the tile in
 * the folder behind it keeps counting up.
 */
function offerOptimisation(entry, status) {
  const running = status.state === 'running' || status.state === 'queued';

  dom.ovMsg.hidden = false;
  dom.ovMsg.innerHTML = '';

  const line = document.createElement('div');

  if (status.state === 'running') {
    line.textContent = `Optimising for smooth playback… ${status.percent ?? 0}%`;
  } else if (status.state === 'queued') {
    line.textContent = status.ahead > 0
      ? `Waiting to be optimised — ${status.ahead} ahead in the queue`
      : 'Waiting to be optimised…';
  } else {
    line.textContent =
      'This video is larger than most connections can play smoothly.';
  }

  dom.ovMsg.appendChild(line);

  const buttons = document.createElement('div');
  buttons.style.marginTop = '10px';
  buttons.style.display = 'flex';
  buttons.style.gap = '8px';
  buttons.style.justifyContent = 'center';
  buttons.style.flexWrap = 'wrap';

  if (!running) {
    const optimise = document.createElement('button');
    optimise.className = 'ov-btn';
    optimise.style.background = '#0ea5e9';
    optimise.style.color = '#fff';
    optimise.textContent = 'Optimise it';
    optimise.addEventListener('click', async () => {
      try {
        const next =
          await (await fetch(videoQuery(entry.u), { method: 'POST' })).json();
        applyVideoStatus(entry.u, next, true);
      } catch {
        statusMessage('Could not start optimising.');
      }
    });
    buttons.appendChild(optimise);
  }

  const anyway = document.createElement('button');
  anyway.className = 'ov-btn';
  anyway.style.background = '#374151';
  anyway.style.color = '#9ca3af';
  anyway.textContent = running ? 'Play the original meanwhile' : 'Play anyway';
  anyway.addEventListener('click', () => {
    clearRetry();

    // Remembered, so the rendition finishing a minute from now does not interrupt
    // what is already playing. The tile still turns green; this viewing does not
    // change under them.
    state.playingOriginal = true;
    startPlayback(entry, entry.u);
  });
  buttons.appendChild(anyway);

  dom.ovMsg.appendChild(buttons);
}

function closeOverlay() {
  // clearRetry() only drops a pending "server busy" retry. Optimisation tracking is
  // deliberately untouched: the work carries on, and its tile keeps reporting.
  clearRetry();
  dom.overlay.hidden = true;
  dom.ovVid.pause();
  dom.ovVid.removeAttribute('src');
  dom.ovVid.load();          // Stops buffering in the background.
  dom.ovImg.removeAttribute('src');
  state.current = -1;
}

function step(delta) {
  const position = state.media.indexOf(state.current);
  if (position < 0) return;
  const next = state.media[position + delta];
  if (next !== undefined) openOverlay(next);
}

// ── Wiring ──────────────────────────────────────────────────────────────────

dom.scroller.addEventListener('scroll', () => render(), { passive: true });

window.addEventListener('resize', () => {
  applySizer();
  render(true);
});

dom.back.addEventListener('click', (event) => {
  event.preventDefault();
  const parent = parentPath(state.path);
  if (parent !== null) navigate(parent);
});

dom.signout.addEventListener('click', (event) => {
  event.preventDefault();
  signOut();
});

document.getElementById('ov-close').addEventListener('click', closeOverlay);
document.getElementById('ov-prev').addEventListener('click', () => step(-1));
document.getElementById('ov-next').addEventListener('click', () => step(1));

dom.overlay.addEventListener('click', (event) => {
  if (event.target === dom.overlay) closeOverlay();
});

document.addEventListener('keydown', (event) => {
  if (dom.overlay.hidden) return;
  if (event.key === 'Escape') closeOverlay();
  else if (event.key === 'ArrowLeft') step(-1);
  else if (event.key === 'ArrowRight') step(1);
});

window.addEventListener('popstate', (event) => {
  navigate(event.state?.path || decodeURI(location.pathname), true);
});

navigate(decodeURI(location.pathname) || '/', true);
