"""Read-only cleanup preview and invalid-request checks. Never confirms deletion."""
import json
from urllib.request import Request, ProxyHandler, build_opener
from urllib.error import HTTPError
from verify_device import discover

url, _ = discover()
base, key = url.split('/#', 1)
opener = build_opener(ProxyHandler({}))
headers = {'Authorization': 'Bearer ' + key, 'Content-Type': 'application/json'}
def state():
    with opener.open(Request(base + '/api/state', headers=headers), timeout=20) as response:
        return json.load(response)
def request(body, authenticated=True):
    with opener.open(Request(base + '/api/history/cleanup', data=json.dumps(body).encode(),
                             headers=headers if authenticated else {'Content-Type': 'application/json'}), timeout=20) as response:
        return json.load(response)
before = state()
preview = request({'action': 'preview', 'before': before['today']})
assert 'error' not in preview and preview['removed'] == 0
checks = [({'action': 'preview', 'before': '../other'}, True, 400),
          ({'action': 'preview', 'before': '2099-12-31'}, True, 400),
          ({'action': 'delete', 'before': before['today'], 'confirm': 'NAO', 'plan': ''}, True, 400),
          ({'action': 'preview', 'before': before['today']}, False, 401)]
for body, authenticated, expected in checks:
    try:
        request(body, authenticated)
    except HTTPError as error:
        assert error.code == expected, (body['action'], error.code)
    else:
        raise AssertionError('Invalid request accepted')
after = state()
assert after['storage']['ok'], after['storage']
assert after['report']['sample_count'] >= before['report']['sample_count']
print(json.dumps({'preview_only': preview, 'guards': 'PASS', 'storage': after['storage'],
                  'samples_before': before['report']['sample_count'], 'samples_after': after['report']['sample_count'],
                  'deletion_executed': False}, ensure_ascii=True, indent=2))
