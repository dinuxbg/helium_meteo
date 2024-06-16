#!/usr/bin/env python3

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Very simple HTTP server in python for logging data
# from a Helium meteo sensor.
# Usage::
#    ./server.py [<port>]

from http.server import BaseHTTPRequestHandler, HTTPServer
import logging
import meteo

class Server(BaseHTTPRequestHandler):
    def __init__(self, *args):
        self.meteo = meteo.Meteo()
        BaseHTTPRequestHandler.__init__(self, *args)

    def _set_response(self):
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()

    def do_GET(self):
        logging.debug("GET request,\nPath: %s\nHeaders:\n%s\n", str(self.path), str(self.headers))
        self._set_response()
        self.wfile.write("42".encode('utf-8'))

    def do_POST(self):
        self.send_response(200)
        self.end_headers()

        content_length = int(self.headers['Content-Length']) # <--- Gets the size of data
        post_data = self.rfile.read(content_length) # <--- Gets the data itself
        logging.debug("POST request,\nPath: %s\nHeaders:\n%s\n\nBody:\n%s\n",
                str(self.path), str(self.headers), post_data.decode('utf-8'))
        try:
            event = 'unknown'
            for arg in self.path.split('?'):
                arg_split = arg.split('=', 3)
                if len(arg_split) != 2:
                    continue
                path_key, path_val = arg_split
                if path_key == 'event':
                    event = path_val
                    break
            if event == 'up':
                self.meteo.record(post_data)
            else:
                print('Ignoring event ' + event)
        except Exception as e:
            print('Exception occurred with the following json: {}'.format(post_data))
            print('Exception: ' + str(e))
        logging.info('Received: json: {}'.format(post_data))

def run(server_class=HTTPServer, handler_class=Server, port=8085):
    logging.basicConfig(level=logging.INFO)
    server_address = ('', port)
    httpd = server_class(server_address, handler_class)
    logging.info('Starting httpd...\n')
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    httpd.server_close()
    logging.info('Stopping httpd...\n')

if __name__ == '__main__':
    from sys import argv

    if len(argv) == 2:
        run(port=int(argv[1]))
    else:
        run()
