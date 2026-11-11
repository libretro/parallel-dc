#!/usr/bin/env python3
# A UPnP Internet gateway for tools/threads/upnp_test.sh, on this host: it
# answers SSDP searches (239.255.255.250:1900) with its description, served
# on the loopback interface with a WANIPConnection service, and writes each
# SOAP request's action and arguments to LOG, a line each:
#
#   AddPortMapping NewProtocol=TCP NewExternalPort=5000 ...
#
#   upnp_gateway.py LOG READY      READY is written once it listens
import re
import socket
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

log_path, ready_path = sys.argv[1], sys.argv[2]
lock = threading.Lock()

DESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<controlURL>/ctl/IPConn</controlURL>
</service></serviceList>
</device></deviceList></device></deviceList></device></root>"""

ENVELOPE = ('<?xml version="1.0"?><s:Envelope '
            'xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
            '<u:%sResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">'
            '%s</u:%sResponse></s:Body></s:Envelope>')


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, body, status=200):
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', 'text/xml')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == '/desc.xml':
            self.reply(DESC)
        else:
            self.reply('', 404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        action = self.headers.get('SOAPAction', '').strip('"').split('#')[-1]
        args = re.findall(r'<(New\w+)>([^<]*)</New\w+>', body)
        with lock:
            with open(log_path, 'a') as f:
                f.write(' '.join([action] + ['%s=%s' % a for a in args]) + '\n')
        out = ''
        if action == 'GetExternalIPAddress':
            out = '<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>'
        self.reply(ENVELOPE % (action, out, action))


http = HTTPServer(('127.0.0.1', 0), Handler)
location = 'http://127.0.0.1:%d/desc.xml' % http.server_address[1]
threading.Thread(target=http.serve_forever, daemon=True).start()

ssdp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
ssdp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ssdp.bind(('', 1900))
group = socket.inet_aton('239.255.255.250')
for iface in ('0.0.0.0', '127.0.0.1'):
    try:
        ssdp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                        group + socket.inet_aton(iface))
    except OSError:
        pass
open(ready_path, 'w').close()
while True:
    msg, addr = ssdp.recvfrom(2048)
    if msg.startswith(b'M-SEARCH') and b'ssdp:discover' in msg:
        ssdp.sendto(('HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\n'
                     'ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n'
                     'USN: uuid:upnp-test::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n'
                     'EXT:\r\nSERVER: test/1.0 UPnP/1.1 test/1.0\r\n'
                     'LOCATION: %s\r\n\r\n' % location).encode(), addr)
