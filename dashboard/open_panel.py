"""One-use local handoff to the ESP. No history, credential output or persistent server."""
from http.server import HTTPServer, BaseHTTPRequestHandler
from verify_device import discover

url,_=discover()
class Handoff(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_GET(self):
        if self.path!='/open':
            self.send_error(404);return
        self.send_response(302)
        self.send_header('Location',url)
        self.send_header('Cache-Control','no-store')
        self.send_header('Referrer-Policy','no-referrer')
        self.end_headers()

with HTTPServer(('127.0.0.1',0),Handoff) as server:
    server.timeout=45
    print('http://127.0.0.1:'+str(server.server_port)+'/open',flush=True)
    server.handle_request()
