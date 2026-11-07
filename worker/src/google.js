// Google sign-in for 3DSync's own OAuth client.
//
// A web-application client needs its secret for both calls a sign-in makes,
// the code exchange and every refresh, so with one client for everybody the
// secret cannot ship in the page or in 3DSync.ini.  POST /api/google/token is
// Google's token endpoint without the secret: the same fields in (form or
// JSON), Google's answer out, and the Worker adds client_id and client_secret
// from its secrets.  Nothing is logged or kept.
//
//   grant_type=authorization_code  the configurator, from this site's own page
//                                  only, PKCE required, redirect_uri exactly
//                                  this site's /configurator.html
//   grant_type=refresh_token       the 3DS, which sends no Origin; a page on
//                                  another site is refused
//
// GET /api/google/config hands the configurator the public settings.
//
// Secrets (set in the dashboard, which deploys keep): GOOGLE_CLIENT_ID,
// GOOGLE_CLIENT_SECRET, and for the Picker GOOGLE_APP_ID (the project number)
// and GOOGLE_PICKER_KEY.

export const GOOGLE_TOKEN_URL = 'https://oauth2.googleapis.com/token';

// A token request is a few hundred bytes; an authorization code and a PKCE
// verifier fit easily.
const MAX_BODY = 8 * 1024;

// What a caller gets back from Google's answer.  Anything else Google adds is
// dropped, so the endpoint never promises more than this.
const ANSWER_FIELDS = ['access_token', 'expires_in', 'refresh_token', 'scope', 'token_type',
    'id_token', 'error', 'error_description'];

export async function handleGoogle(request, env, fetchFn = fetch) {
    const url = new URL(request.url);
    if (url.pathname === '/api/google/config')
        return request.method === 'GET' ? config(env) : refuse(405, 'invalid_request', 'Use GET.');
    if (url.pathname === '/api/google/token')
        return request.method === 'POST' ? token(request, url, env, fetchFn)
            : refuse(405, 'invalid_request', 'Use POST.');
    return null;
}

function config(env) {
    return json(200, {
        clientId: env.GOOGLE_CLIENT_ID || null,
        appId: env.GOOGLE_APP_ID || null,
        pickerKey: env.GOOGLE_PICKER_KEY || null,
    }, { 'Cache-Control': 'public, max-age=300' });
}

async function token(request, url, env, fetchFn) {
    if (!env.GOOGLE_CLIENT_ID || !env.GOOGLE_CLIENT_SECRET)
        return refuse(503, 'temporarily_unavailable', 'Google sign-in is not set up on this site.');

    // Browsers send Origin on every POST.  Only this site's page may use the
    // endpoint from a browser; the 3DS sends none.
    const origin = request.headers.get('Origin');
    if (origin !== null && origin !== url.origin)
        return refuse(403, 'invalid_request', 'Requests from other sites are not accepted.');

    let fields;
    try {
        fields = await readFields(request);
    } catch (e) {
        return refuse(400, 'invalid_request', e.message);
    }

    if (fields.client_id && fields.client_id !== env.GOOGLE_CLIENT_ID)
        return refuse(400, 'invalid_client', 'This endpoint is for 3DSync\'s own client only.');

    const upstream = new URLSearchParams({
        client_id: env.GOOGLE_CLIENT_ID,
        client_secret: env.GOOGLE_CLIENT_SECRET,
    });
    switch (fields.grant_type) {
    case 'authorization_code': {
        if (origin === null)
            return refuse(403, 'invalid_request', 'A sign-in code is only exchanged for the configurator.');
        const configurator = new URL('/configurator.html', url).href;
        if (fields.redirect_uri !== configurator)
            return refuse(400, 'invalid_request', 'redirect_uri must be ' + configurator + '.');
        if (!fields.code || !fields.code_verifier)
            return refuse(400, 'invalid_request', 'code and code_verifier are required.');
        upstream.set('grant_type', 'authorization_code');
        upstream.set('code', fields.code);
        upstream.set('code_verifier', fields.code_verifier);
        upstream.set('redirect_uri', configurator);
        break;
    }
    case 'refresh_token':
        if (!fields.refresh_token)
            return refuse(400, 'invalid_request', 'refresh_token is required.');
        upstream.set('grant_type', 'refresh_token');
        upstream.set('refresh_token', fields.refresh_token);
        break;
    default:
        return refuse(400, 'unsupported_grant_type',
            'grant_type must be authorization_code or refresh_token.');
    }

    let answer;
    try {
        answer = await fetchFn(GOOGLE_TOKEN_URL, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: upstream.toString(),
        });
    } catch (e) {
        return refuse(502, 'temporarily_unavailable', 'Google could not be reached.');
    }

    let body;
    try {
        body = await answer.json();
    } catch (e) {
        return refuse(502, 'temporarily_unavailable', 'Google sent an answer that is not JSON.');
    }
    const out = {};
    for (const key of ANSWER_FIELDS)
        if (body && body[key] !== undefined)
            out[key] = body[key];
    return json(answer.status, out);
}

// The fields of a form or JSON body, as strings.
async function readFields(request) {
    const text = await request.text();
    if (text.length > MAX_BODY)
        throw new Error('The request is too large.');
    const type = (request.headers.get('Content-Type') || '').split(';')[0].trim().toLowerCase();
    const fields = {};
    if (type === 'application/json') {
        let parsed;
        try {
            parsed = JSON.parse(text);
        } catch (e) {
            throw new Error('The body is not valid JSON.');
        }
        if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed))
            throw new Error('The body must be a JSON object.');
        for (const [key, value] of Object.entries(parsed))
            if (typeof value === 'string')
                fields[key] = value;
    } else if (type === 'application/x-www-form-urlencoded') {
        for (const [key, value] of new URLSearchParams(text))
            fields[key] = value;
    } else {
        throw new Error('Send application/x-www-form-urlencoded or application/json.');
    }
    return fields;
}

// Errors in Google's own shape, so a caller reads ours and Google's alike.
function refuse(status, error, description) {
    return json(status, { error: error, error_description: description });
}

function json(status, body, headers = {}) {
    return new Response(JSON.stringify(body), {
        status: status,
        headers: Object.assign({
            'Content-Type': 'application/json; charset=utf-8',
            'Cache-Control': 'no-store',
        }, headers),
    });
}
