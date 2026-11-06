// Panel çevrimdışı açılsın: önce ağ, ağ yoksa önbellek
self.addEventListener('install', e => e.waitUntil(caches.open('kline-panel-1').then(c => c.add('panel.html'))));
self.addEventListener('fetch', e => e.request.method === 'GET' && e.respondWith(
  fetch(e.request).then(r => { const c = r.clone(); caches.open('kline-panel-1').then(k => k.put(e.request, c)); return r; })
    .catch(() => caches.match(e.request))));
