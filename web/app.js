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
  current: -1,
  columns: 1,
  renderedRange: [-1, -1],
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

  if (!response.ok) {
    state.requestedPages.delete(pageIndex);
    statusMessage(`Could not list this folder (${response.status}).`);
    return;
  }

  const data = await response.json();
  state.total = data.total;

  data.entries.forEach((entry, i) => { state.entries[offset + i] = entry; });

  rebuildMediaIndex();
  updateChrome();
  applySizer();
  render(true);
  statusMessage('');
}

/** Media entries, in listing order, for previous/next in the overlay. */
function rebuildMediaIndex() {
  state.media = [];
  for (let i = 0; i < state.entries.length; i++) {
    const entry = state.entries[i];
    if (entry && !entry.d && (entry.k === 'image' || entry.k === 'video'))
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
}

function navigate(path, replace = false) {
  state.path = path || '/';
  state.total = 0;
  state.entries = [];
  state.requestedPages.clear();
  state.media = [];
  state.renderedRange = [-1, -1];
  dom.scroller.scrollTop = 0;
  dom.grid.replaceChildren();

  const url = encodeURI(state.path);
  if (replace) history.replaceState({ path: state.path }, '', url);
  else history.pushState({ path: state.path }, '', url);

  updateChrome();
  loadPage(0);
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

async function playVideo(entry) {
  dom.ovMsg.hidden = false;
  dom.ovMsg.textContent = 'Opening…';

  let response;
  try {
    response = await fetch(entry.u, { method: 'HEAD' });
  } catch {
    dom.ovMsg.textContent = 'Cannot reach the server.';
    return;
  }

  if (response.status === 503) {
    const after = parseInt(response.headers.get('Retry-After') || '5', 10);
    dom.ovMsg.textContent =
      `Server busy — waiting for a free transfer slot (retrying in ${after}s)…`;
    pendingRetry = setTimeout(() => playVideo(entry), after * 1000);
    return;
  }

  if (!response.ok) {
    dom.ovMsg.textContent = `Cannot play this file (${response.status}).`;
    return;
  }

  dom.ovMsg.hidden = true;
  dom.ovVid.src = entry.u;
  dom.ovVid.hidden = false;
}

function closeOverlay() {
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
