// Keeps the app's own files so it opens at once and shows "Looking for the
// receiver" when the receiver is off. The API is never cached. Browsers only
// run this on a secure origin (https or localhost); over plain http on the
// LAN the app works the same, just without the offline shell (docs/app.md).
const CACHE = 'hifi-v1';
const FILES = ['./', 'index.html', 'app.js', 'app.css', 'manifest.webmanifest', 'icon.svg', 'icon-192.png'];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(FILES)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((ks) => Promise.all(ks.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== 'GET' || url.pathname.includes('/api/')) return;
  // Network first, so a firmware update's new app shows at once; the cache when offline.
  e.respondWith(fetch(e.request).then((r) => {
    const copy = r.clone();
    caches.open(CACHE).then((c) => c.put(e.request, copy));
    return r;
  }).catch(() => caches.match(e.request)));
});
