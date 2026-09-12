const crypto = require('crypto');
const fs = require('fs');
const https = require('https');
const path = require('path');

const endpoint = process.env.R2_ENDPOINT;
const bucket = process.env.R2_BUCKET;
const accessKeyId = process.env.AWS_ACCESS_KEY_ID;
const secretAccessKey = process.env.AWS_SECRET_ACCESS_KEY;

if (!endpoint || !bucket || !accessKeyId || !secretAccessKey) {
  throw new Error('Missing R2_ENDPOINT, R2_BUCKET, AWS_ACCESS_KEY_ID, or AWS_SECRET_ACCESS_KEY.');
}

const releaseDir = process.env.MONTI_BROWSER_RESOURCE_DIR || 'resource-release';
const uploads = fs.readdirSync(releaseDir)
    // Both prefixes: the archive is published as Scout-Web-*, but a build
    // directory packaged before the rename still produces Monti-Browser-*.
    // A name that matches neither is skipped silently, which would upload the
    // manifest while leaving its url pointing at a file that was never sent.
    .filter((name) => /^(Scout-Web|Monti-Browser)-.+\.zip$/.test(name) || /^latest-.+\.json$/.test(name))
    .map((name) => ({
      file: path.join(releaseDir, name),
      key: `resources/browser/${name}`,
      contentType: name.endsWith('.json') ? 'application/json' : 'application/zip',
    }));

if (uploads.length === 0) {
  throw new Error(`No browser resource artifacts found in ${releaseDir}.`);
}

function hmac(key, value, encoding) {
  return crypto.createHmac('sha256', key).update(value, 'utf8').digest(encoding);
}

function sha256(value, encoding = 'hex') {
  return crypto.createHash('sha256').update(value).digest(encoding);
}

function signingKey(secret, dateStamp, region, service) {
  const kDate = hmac(`AWS4${secret}`, dateStamp);
  const kRegion = hmac(kDate, region);
  const kService = hmac(kRegion, service);
  return hmac(kService, 'aws4_request');
}

function amzDate(now) {
  return now.toISOString().replace(/[:-]|\.\d{3}/g, '');
}

function encodeKey(key) {
  return key.split('/').map(encodeURIComponent).join('/');
}

function putObject({file, key, contentType}) {
  const body = fs.readFileSync(file);
  const base = new URL(endpoint.replace(/\/$/, ''));
  const objectPath = `/${bucket}/${encodeKey(key)}`;
  const now = new Date();
  const xAmzDate = amzDate(now);
  const dateStamp = xAmzDate.slice(0, 8);
  const region = 'auto';
  const service = 's3';
  const payloadHash = sha256(body);
  const host = base.host;

  const canonicalHeaders = [
    `cache-control:public, max-age=60`,
    `content-type:${contentType}`,
    `host:${host}`,
    `x-amz-content-sha256:${payloadHash}`,
    `x-amz-date:${xAmzDate}`,
  ].join('\n') + '\n';
  const signedHeaders = 'cache-control;content-type;host;x-amz-content-sha256;x-amz-date';
  const canonicalRequest = [
    'PUT',
    objectPath,
    '',
    canonicalHeaders,
    signedHeaders,
    payloadHash,
  ].join('\n');
  const credentialScope = `${dateStamp}/${region}/${service}/aws4_request`;
  const stringToSign = [
    'AWS4-HMAC-SHA256',
    xAmzDate,
    credentialScope,
    sha256(canonicalRequest),
  ].join('\n');
  const signature = hmac(signingKey(secretAccessKey, dateStamp, region, service), stringToSign, 'hex');

  const headers = {
    Authorization: `AWS4-HMAC-SHA256 Credential=${accessKeyId}/${credentialScope}, SignedHeaders=${signedHeaders}, Signature=${signature}`,
    'Cache-Control': 'public, max-age=60',
    'Content-Length': body.length,
    'Content-Type': contentType,
    Host: host,
    'x-amz-content-sha256': payloadHash,
    'x-amz-date': xAmzDate,
  };

  return new Promise((resolve, reject) => {
    const req = https.request({
      protocol: base.protocol,
      hostname: base.hostname,
      port: base.port || 443,
      method: 'PUT',
      path: objectPath,
      headers,
    }, (res) => {
      let responseBody = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => {
        responseBody += chunk;
      });
      res.on('end', () => {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          console.log(`Uploaded ${file} -> ${key}`);
          resolve();
        } else {
          reject(new Error(`Upload failed for ${key}: HTTP ${res.statusCode} ${responseBody}`));
        }
      });
    });
    req.on('error', reject);
    req.end(body);
  });
}

(async () => {
  for (const upload of uploads) {
    await putObject(upload);
  }
})();
