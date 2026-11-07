// worker/src/google.js under Node: the token endpoint for 3DSync's own Google
// client, with a stand-in for Google.  Run with `node --test tests/worker/*.test.mjs`.

import test from 'node:test';
import assert from 'node:assert/strict';
import { handleGoogle, GOOGLE_TOKEN_URL } from '../../worker/src/google.js';
import worker from '../../worker/src/index.js';

const SITE = 'https://3dsync.vllni.dev';
const ENV = {
    GOOGLE_CLIENT_ID: 'id.apps.googleusercontent.com',
    GOOGLE_CLIENT_SECRET: 'the-secret',
    GOOGLE_APP_ID: '123',
    GOOGLE_PICKER_KEY: 'picker-key',
};

// Google as the Worker sees it: records each call, answers with `reply`.
function google(reply = { status: 200, body: { access_token: 'at', expires_in: 3599, scope: 's',
    token_type: 'Bearer', refresh_token: 'rt', extra: 'dropped' } }) {
    const calls = [];
    const fetchFn = async (url, init) => {
        calls.push({ url, fields: Object.fromEntries(new URLSearchParams(init.body)) });
        if (reply instanceof Error)
            throw reply;
        return new Response(typeof reply.body === 'string' ? reply.body : JSON.stringify(reply.body),
            { status: reply.status });
    };
    return { calls, fetchFn };
}

function post(path, fields, { origin = SITE, type = 'form' } = {}) {
    const headers = {};
    if (origin)
        headers.Origin = origin;
    let body;
    if (type === 'json') {
        headers['Content-Type'] = 'application/json';
        body = JSON.stringify(fields);
    } else if (type === 'form') {
        headers['Content-Type'] = 'application/x-www-form-urlencoded';
        body = new URLSearchParams(fields).toString();
    } else {
        headers['Content-Type'] = type;
        body = String(fields);
    }
    return new Request(SITE + path, { method: 'POST', headers, body });
}

const SIGN_IN = { grant_type: 'authorization_code', code: 'c', code_verifier: 'v',
    redirect_uri: SITE + '/configurator.html' };

test('config hands out the public settings and never the secret', async () => {
    const r = await handleGoogle(new Request(SITE + '/api/google/config'), ENV);
    assert.equal(r.status, 200);
    const body = await r.json();
    assert.deepEqual(body, { clientId: ENV.GOOGLE_CLIENT_ID, appId: '123', pickerKey: 'picker-key' });
    assert.ok(!JSON.stringify(body).includes('the-secret'));
    const unset = await (await handleGoogle(new Request(SITE + '/api/google/config'), {})).json();
    assert.deepEqual(unset, { clientId: null, appId: null, pickerKey: null });
});

test('the configurator\'s sign-in code is exchanged with the secret added', async () => {
    const g = google();
    const r = await handleGoogle(post('/api/google/token', SIGN_IN, { type: 'json' }), ENV, g.fetchFn);
    assert.equal(r.status, 200);
    assert.equal(g.calls[0].url, GOOGLE_TOKEN_URL);
    assert.deepEqual(g.calls[0].fields, {
        client_id: ENV.GOOGLE_CLIENT_ID, client_secret: 'the-secret', grant_type: 'authorization_code',
        code: 'c', code_verifier: 'v', redirect_uri: SITE + '/configurator.html',
    });
    // Google's answer, minus anything the endpoint does not promise.
    assert.deepEqual(await r.json(), { access_token: 'at', expires_in: 3599, refresh_token: 'rt',
        scope: 's', token_type: 'Bearer' });
    assert.equal(r.headers.get('Cache-Control'), 'no-store');
});

test('a sign-in code is only exchanged for this site\'s page, with PKCE', async () => {
    const g = google();
    const cases = [
        [post('/api/google/token', SIGN_IN, { origin: null }), 403],
        [post('/api/google/token', SIGN_IN, { origin: 'https://evil.example' }), 403],
        [post('/api/google/token', { ...SIGN_IN, redirect_uri: 'https://evil.example/cb' }), 400],
        [post('/api/google/token', { ...SIGN_IN, redirect_uri: SITE + '/configurator' }), 400],
        [post('/api/google/token', { ...SIGN_IN, code_verifier: '' }), 400],
    ];
    for (const [request, status] of cases) {
        const r = await handleGoogle(request, ENV, g.fetchFn);
        assert.equal(r.status, status);
        assert.ok((await r.json()).error);
    }
    assert.equal(g.calls.length, 0);
});

test('the 3DS refreshes with Google\'s own form fields and no secret', async () => {
    const g = google({ status: 200, body: { access_token: 'new', expires_in: 3599, token_type: 'Bearer' } });
    const r = await handleGoogle(post('/api/google/token',
        { grant_type: 'refresh_token', refresh_token: 'rt', client_id: ENV.GOOGLE_CLIENT_ID },
        { origin: null }), ENV, g.fetchFn);
    assert.equal(r.status, 200);
    assert.deepEqual(g.calls[0].fields, { client_id: ENV.GOOGLE_CLIENT_ID, client_secret: 'the-secret',
        grant_type: 'refresh_token', refresh_token: 'rt' });
    assert.equal((await r.json()).access_token, 'new');
});

test('Google\'s refusal reaches the caller as Google sent it', async () => {
    const g = google({ status: 400, body: { error: 'invalid_grant', error_description: 'Token has been expired or revoked.' } });
    const r = await handleGoogle(post('/api/google/token',
        { grant_type: 'refresh_token', refresh_token: 'old' }, { origin: null }), ENV, g.fetchFn);
    assert.equal(r.status, 400);
    assert.deepEqual(await r.json(), { error: 'invalid_grant', error_description: 'Token has been expired or revoked.' });
});

test('everything else is refused before Google is asked', async () => {
    const g = google();
    const cases = [
        [post('/api/google/token', { grant_type: 'refresh_token', refresh_token: 'rt' },
            { origin: 'https://evil.example' }), 403, 'invalid_request'],
        [post('/api/google/token', { grant_type: 'refresh_token' }, { origin: null }), 400, 'invalid_request'],
        [post('/api/google/token', { grant_type: 'refresh_token', refresh_token: 'rt', client_id: 'other' },
            { origin: null }), 400, 'invalid_client'],
        [post('/api/google/token', { grant_type: 'password' }, { origin: null }), 400, 'unsupported_grant_type'],
        [post('/api/google/token', 'grant_type=refresh_token', { origin: null, type: 'text/plain' }), 400, 'invalid_request'],
        [post('/api/google/token', '{not json', { origin: null, type: 'application/json' }), 400, 'invalid_request'],
        [post('/api/google/token', 'x'.repeat(9000), { origin: null, type: 'application/x-www-form-urlencoded' }),
            400, 'invalid_request'],
        [new Request(SITE + '/api/google/token'), 405, 'invalid_request'],
    ];
    for (const [request, status, error] of cases) {
        const r = await handleGoogle(request, ENV, g.fetchFn);
        assert.equal(r.status, status);
        assert.equal((await r.json()).error, error);
    }
    assert.equal(g.calls.length, 0);
});

test('without the secret configured the endpoint says so instead of asking Google', async () => {
    const g = google();
    const r = await handleGoogle(post('/api/google/token',
        { grant_type: 'refresh_token', refresh_token: 'rt' }, { origin: null }),
        { GOOGLE_CLIENT_ID: ENV.GOOGLE_CLIENT_ID }, g.fetchFn);
    assert.equal(r.status, 503);
    assert.equal(g.calls.length, 0);
});

test('Google unreachable or answering nonsense is a 502, not a crash', async () => {
    const refresh = () => post('/api/google/token', { grant_type: 'refresh_token', refresh_token: 'rt' }, { origin: null });
    let r = await handleGoogle(refresh(), ENV, google(new Error('network')).fetchFn);
    assert.equal(r.status, 502);
    r = await handleGoogle(refresh(), ENV, google({ status: 500, body: '<html>oops' }).fetchFn);
    assert.equal(r.status, 502);
    assert.equal((await r.json()).error, 'temporarily_unavailable');
});

test('the Worker sends /api/ here and nowhere else', async () => {
    const assets = { fetch: async () => new Response('file', { status: 200 }) };
    let r = await worker.fetch(new Request(SITE + '/api/google/config'), { ...ENV, ASSETS: assets });
    assert.equal((await r.json()).clientId, ENV.GOOGLE_CLIENT_ID);
    r = await worker.fetch(new Request(SITE + '/api/nothing'), { ...ENV, ASSETS: assets });
    assert.equal(r.status, 404);
    r = await worker.fetch(new Request(SITE + '/'), { ...ENV, ASSETS: assets });
    assert.equal(await r.text(), 'file');
});
