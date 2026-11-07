// worker/src/index.js under Node: which file the site serves for an address.
// Run with `node --test tests/worker/*.test.mjs`.

import test from 'node:test';
import assert from 'node:assert/strict';
import { serveSite } from '../../worker/src/index.js';

// The asset binding as Cloudflare runs it with html_handling = "none": an
// exact path or 404, and never a redirect.
function assets(paths) {
    const asked = [];
    return {
        asked,
        async fetch(request) {
            const url = new URL(request.url);
            asked.push(url.pathname + url.search);
            return paths.includes(url.pathname)
                ? new Response(url.pathname, { status: 200 })
                : new Response('not found', { status: 404 });
        },
    };
}

const SITE = ['/index.html', '/configurator.html', '/privacy-policy.html', '/unistore/3dsync.unistore'];

async function get(path, a) {
    return serveSite(new Request('https://3dsync.vllni.dev' + path), a);
}

test('the home page is index.html', async () => {
    const a = assets(SITE);
    const r = await get('/', a);
    assert.equal(r.status, 200);
    assert.equal(await r.text(), '/index.html');
});

test('the OAuth redirect URI keeps its query and is not rewritten', async () => {
    const a = assets(SITE);
    const r = await get('/configurator.html?state=s&code=c', a);
    assert.equal(r.status, 200);
    assert.equal(await r.text(), '/configurator.html');
    assert.deepEqual(a.asked, ['/configurator.html?state=s&code=c']);
});

test('an address without .html finds the page, as on GitHub Pages', async () => {
    const a = assets(SITE);
    const r = await get('/privacy-policy?x=1', a);
    assert.equal(r.status, 200);
    assert.equal(await r.text(), '/privacy-policy.html');
    assert.equal(a.asked[1], '/privacy-policy.html?x=1');
});

test('a missing file stays 404 and gets no .html guess', async () => {
    const a = assets(SITE);
    assert.equal((await get('/unistore/missing.unistore', a)).status, 404);
    assert.deepEqual(a.asked, ['/unistore/missing.unistore']);
    assert.equal((await get('/nope', a)).status, 404);
});
