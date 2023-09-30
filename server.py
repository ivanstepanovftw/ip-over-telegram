import logging
import select
import socket
import struct
from socketserver import ThreadingMixIn, TCPServer, StreamRequestHandler

logging.basicConfig(level=logging.DEBUG)
SOCKS_VERSION = 5


class ThreadingTCPServer(ThreadingMixIn, TCPServer):
    pass


class SocksProxy(StreamRequestHandler):
    method = 0  # 2 to enable USERNAME/PASSWORD auth, 0 to disable
    username = "username"
    password = "password"

    def handle(self):
        logging.info('Accepting connection from %s:%s' % self.client_address)

        # greeting header
        # read and unpack 2 bytes from a client
        header = self.connection.recv(2)
        version, nmethods = struct.unpack("!BB", header)

        # socks 5
        assert version == SOCKS_VERSION
        assert nmethods > 0

        # get available methods
        methods = self.get_available_methods(nmethods)
        print("methods", methods)

        # accept only USERNAME/PASSWORD auth
        if self.method == 2 and 2 in set(methods):
            # send welcome message
            self.connection.sendall(struct.pack("!BB", SOCKS_VERSION, 2))
            if not self.verify_credentials():
                return
        elif self.method == 0 and 0 in set(methods):
            # send welcome message
            self.connection.sendall(struct.pack("!BB", SOCKS_VERSION, 0))
        else:
            # close connection
            self.server.close_request(self.request)
            return

        # request
        version, cmd, _, address_type = struct.unpack("!BBBB", self.connection.recv(4))
        assert version == SOCKS_VERSION

        if address_type == 1:  # IPv4
            address = socket.inet_ntoa(self.connection.recv(4))
        elif address_type == 3:  # Domain name
            domain_length = self.connection.recv(1)[0]
            address = self.connection.recv(domain_length)
            address = socket.gethostbyname(address)
        port = struct.unpack('!H', self.connection.recv(2))[0]

        # reply
        try:
            if cmd == 1:  # CONNECT
                logging.info(f'Connecting to {address}:{port}')
                remote = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                remote.connect((address, port))
                bind_address = remote.getsockname()
                logging.info(f'Connected to {address}:{port}')
            else:
                self.server.close_request(self.request)

            addr = struct.unpack("!I", socket.inet_aton(bind_address[0]))[0]
            port = bind_address[1]
            reply = struct.pack("!BBBBIH", SOCKS_VERSION, 0, 0, 1, addr, port)

        except Exception as err:
            logging.debug(err, exc_info=True)

            # return connection refused error
            reply = self.generate_failed_reply(address_type, 5)


        self.connection.sendall(reply)

        # establish data exchange
        if reply[1] == 0 and cmd == 1:
            self.exchange_loop(self.connection, remote)

        self.server.close_request(self.request)

    def get_available_methods(self, n):
        methods = []
        for i in range(n):
            methods.append(ord(self.connection.recv(1)))
        return methods

    def verify_credentials(self):
        version = ord(self.connection.recv(1))
        print("version", version)
        assert version == 1

        username_len = ord(self.connection.recv(1))
        print("username_len", username_len)
        username = self.connection.recv(username_len).decode('utf-8')
        print("username", username)

        password_len = ord(self.connection.recv(1))
        print("password_len", password_len)
        password = self.connection.recv(password_len).decode('utf-8')
        print("password", password)

        if username == self.username and password == self.password:
            # success, status = 0
            response = struct.pack("!BB", version, 0)
            self.connection.sendall(response)
            print("success")
            return True

        # failure, status != 0
        response = struct.pack("!BB", version, 0xFF)
        self.connection.sendall(response)
        self.server.close_request(self.request)
        print("failure")
        return False

    def generate_failed_reply(self, address_type, error_number):
        return struct.pack("!BBBBIH", SOCKS_VERSION, error_number, 0, address_type, 0, 0)

    def exchange_loop(self, client, remote):
        while True:
            # wait until client or remote is available for read
            r, w, e = select.select([client, remote], [], [])

            if client in r:
                data = client.recv(4096)
                if remote.send(data) <= 0:
                    break

            if remote in r:
                data = remote.recv(4096)
                if client.send(data) <= 0:
                    break


if __name__ == '__main__':
    PORT = 4090
    with ThreadingTCPServer(('127.0.0.1', PORT), SocksProxy) as server:
        print(f'Server listening on port {PORT}...')
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        print("Server shutting down")
        server.shutdown()
        server.server_close()
        print("Server shut down")
