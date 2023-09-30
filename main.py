#!/usr/bin/env python3
import asyncio
import datetime
import signal
import socket
import sys
import threading
import time
from dataclasses import dataclass
from pytdbot import Client, utils
from pytdbot.types import LogStreamFile, Update
import base64
import yaml
import argparse
import os
import lz4.frame
import logging
import select
import socket
import struct
from socketserver import ThreadingMixIn, TCPServer, StreamRequestHandler
from libtuntap.install.lib import pytuntap
print(f"tuntap v{pytuntap.tuntap_version()}")


MTU = 1500
SOCKS_VERSION = 5


@dataclass
class Config:
    @dataclass
    class TDConfig:
        files_directory: str
        token: str
        api_id: int
        api_hash: str
        database_encryption_key: str

    @dataclass
    class TUNConfig:
        ip: str
        name: str

    tdconfig: TDConfig
    tun: TUNConfig
    wrap_in_proxy: bool
    receive_from_user_id: int
    send_to_chat_id: int

    @staticmethod
    def from_yaml(path):
        with open(path, "r") as f:
            return Config(**yaml.safe_load(f))

    def load(self, path: str):
        with open(path, "r") as f:
            self.__dict__.update(yaml.safe_load(f))
        return self

    def save(self, path: str):
        with open(path, "w") as f:
            yaml.dump(self.__dict__, f)
        return self


async def async_main():
    # logging.basicConfig(level=logging.DEBUG)

    parser = argparse.ArgumentParser(description="TDLib example")
    parser.add_argument("config", type=str, help="Path to config file")
    args = parser.parse_args()

    config = Config.from_yaml(args.config)
    signal.signal(signal.SIGHUP, lambda signum, frame: config.load(args.config))

    tc = Client(
        lib_path="./td/install/lib/libtdjson.so",  # Path to TDjson shared library
        td_log=LogStreamFile("tdlib.log"),  # Set TDLib log file path
        td_verbosity=2,  # TDLib verbosity level
        loop=asyncio.get_event_loop(),  # asyncio loop to use
        **config.tdconfig
    )

    # Add TUN device
    tun_device = pytuntap.Tun()
    print(123, config.tun)
    tun_device.name = config.tun["name"]
    tun_device.mtu = MTU
    tun_device.up()
    tun_device.ip(config.tun["ip"], 24)
    tun_device.nonblocking(True)
    tun_fd = tun_device.native_handle

    stats = {
        "idle": 0,
        "read": 0,
        "sent": 0,
        "received": 0,
        "write": 0,
        "notfound": 0,
        "error": 0,
    }

    # c_context = lz4.frame.create_compression_context()
    # d_context = lz4.frame.create_decompression_context()

    @tc.on_updateNewMessage()
    async def receive_message(c: Client, message: Update):
        # print(f'Received message: {message}')
        stats["received"] += 1

        if message.from_id != config.receive_from_user_id:
            return

        packet = message.text
        if not packet:
            return
        if not packet.startswith("#iot "):
            return
        packet = packet[5:]
        # packet = base_gram.decode(packet)
        packet = base64.b64decode(packet)
        # packet, b, e = lz4.frame.decompress_chunk(d_context, packet)
        # print(f"Received packet ({len(packet)} bytes): {packet}")
        os.write(tun_fd, packet)
        stats["write"] += 1
        # await c.deleteMessages(message.chat_id, [message.message_id], True)
        # stats["delete"] += 1

    @tc.on_updateAuthorizationState()
    async def auth(c: Client, update: Update):
        # print(f'Authorization state: {update["authorization_state"]}')
        if update["authorization_state"]["@type"] == "authorizationStateReady":
            me = await c.getMe()
            print(f'Ready! My ID is {me["id"]}. Sending welcome message to {config.send_to_chat_id}...')
            result = await c.sendTextMessage(config.send_to_chat_id, f'Ready! Current time is {datetime.datetime.now()}, host is {socket.gethostname()}')
            print(result)

            # for i, chunk in enumerate(chunked_chars):
            #     result = await c.sendTextMessage(me["id"], f'{debug_chars_prefix} {i:05} {chunk}')
            #     print(i, result['content']['text']['text'], chunk)
            #     # exit()


    # Run the client
    # tc.run()

    async def loop():
        while tc.is_running:
            try:
                # await asyncio.sleep(1)
                # continue
                #
                # packet = f'#count {stats["idle"]}'
                # result = await tc.sendTextMessage(config.send_to_chat_id, packet)
                # if result["@type"] != "error":
                #     stats["sent"] += 1
                # else:
                #     print(result)
                # await asyncio.sleep(1)
                # continue


                packet = os.read(tun_fd, MTU)
                stats["read"] += 1
                # print(f"Sending packet ({len(packet)} bytes): {packet}")
                # packet = lz4.frame.compress_begin(c_context) + packet + lz4.frame.compress_flush(c_context)
                # print(f"After compression: {len(packet)} bytes")
                # packet = base_gram.encode(packet)
                packet = base64.b64encode(packet).decode('utf-8')
                # print(f"After encoding: {len(packet)} bytes")
                packet = f"#iot {packet}"
                result = await tc.sendTextMessage(config.send_to_chat_id, packet)
                if result["@type"] != "error":
                    stats["sent"] += 1
                else:
                    stats.setdefault(result["code"], 0)
                    stats[result["code"]] += 1

                # async def send_packet():
                #     result = await tc.sendTextMessage(config.send_to_chat_id, packet)
                #     if result["@type"] != "error":
                #         stats["sent"] += 1
                # tc.loop.create_task(send_packet())
            except BlockingIOError:
                pass
            except Exception as e:
                stats["error"] += 1
                print(e)
                break
            finally:
                await asyncio.sleep(0)
                stats["idle"] += 1
        print("Stopped!")

    async def statistics():
        while tc.is_running:
            print(f'Statistics: {stats}')
            await asyncio.sleep(2)

    print("Starting...")
    await tc.start(login=False)

    # # TODO: not working before login
    # print("Getting proxies...")
    # proxies = await tc.getProxies()
    # print(f'Proxies: {proxies}')
    # for proxy in proxies["proxies"]:
    #     await tc.removeProxy(proxy["id"])
    #
    # if config.wrap_in_proxy:
    #     print("Using proxy...")
    #     await tc.addProxy("127.0.0.1", 4090, True, type={
    #         "@type": "proxyTypeSocks5",
    #         # "username": "username",
    #         # "password": "password",
    #     })

    async def add_proxy():
        proxies = await tc.getProxies()
        print(f'Proxies: {proxies}')
        for proxy in proxies["proxies"]:
            await tc.removeProxy(proxy["id"])

        print("Proxies removed!")

        # if config.wrap_in_proxy:
        #     await tc.addProxy("127.0.0.1", 4090, True, type={
        #         "@type": "proxyTypeSocks5",
        #         # "username": "username",
        #         # "password": "password",
        #     })

    tc.loop.create_task(add_proxy())

    print("Logging in...")
    await tc.login()

    print("Starting statistics...")
    asyncio.create_task(statistics())

    print("Started!")
    await loop()


if __name__ == '__main__':
    asyncio.run(async_main())
