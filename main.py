#!/usr/bin/env python3
import signal
import sys
import time
from dataclasses import dataclass
from pytdbot import Client, utils
from pytdbot.types import LogStreamFile, Update
import base64
import yaml
import argparse
import os

sys.path.append(os.path.join(os.path.dirname(os.path.realpath(__file__)), 'libtuntap', 'install', 'lib'))
import pytuntap
print(f"tuntap v{pytuntap.tuntap_version()}")


@dataclass
class Config:
    @dataclass
    class TDConfig:
        files_directory: str
        token: str
        api_id: int
        api_hash: str
        database_encryption_key: str
    tdconfig: TDConfig
    receive_from_user_id: int
    send_to_user_id: int

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


def main() -> None:
    parser = argparse.ArgumentParser(description="TDLib example")
    parser.add_argument("config", type=str, help="Path to config file")
    args = parser.parse_args()

    config = Config.from_yaml(args.config)
    signal.signal(signal.SIGHUP, lambda signum, frame: config.load(args.config))

    tc = Client(
        lib_path="./td/install/lib/libtdjson.so",  # Path to TDjson shared library
        td_log=LogStreamFile("tdlib.log"),  # Set TDLib log file path
        td_verbosity=2,  # TDLib verbosity level
        **config.tdconfig
    )

    # Add TUN device
    tun_device = pytuntap.Tun()
    tun_device.name = "telegram_tun0"
    tun_device.mtu = 1500
    tun_device.up()
    tun_device.ip("10.0.0.1", 24)
    tun_device.nonblocking(True)
    tun_fd = tun_device.native_handle

    @tc.on_updateMessageSendSucceeded()
    async def delete_message(c: Client, update: Update):
        await c.deleteMessages(update.chat_id, [update.message_id], True)

    stats = {
        "sent": 0,
        "received": 0,
    }

    @tc.on_updateNewMessage()
    async def receive_message(c: Client, message: Update):
        if message.from_id != config.receive_from_user_id:
            return

        if not message.text:
            return

        if not message.text.startswith("#iot "):
            return

        # send_text = lambda text: c.sendTextMessage(message.chat_id, text)
        # reply_text = lambda text: c.sendTextMessage(message.chat_id, text, reply_to_message_id=message.reply_to_message_id)
        # edit_text = lambda text: message.editMessageText(message.chat_id, message.id, text)
        # await reply_text("Hello, world!")

        # packet = message.text[5:].encode('utf-8')
        packet = base64.b64decode(message.text[5:].encode('utf-8'))
        print(f"Received packet: {packet}")
        os.write(tun_fd, packet)
        stats["received"] += 1

    @tc.on_updateAuthorizationState()
    async def auth(c: Client, update: Update):
        if update["authorization_state"]["@type"] == "authorizationStateReady":
            print(f"Ready! My ID is {(await c.getMe())['id']}")

    # Run the client
    # tc.run()

    async def loop():
        while tc.is_running:
            try:
                packet = os.read(tun_fd, 1500)
                # await tc.sendTextMessage(config.send_to_user_id, f"#iot {packet.decode('utf-8')}")
                await tc.sendTextMessage(config.send_to_user_id, f"#iot {base64.b64encode(packet).decode('utf-8')}")
                stats["sent"] += 1
                print(f"Stats: {stats}")
            except BlockingIOError:
                pass
            except Exception as e:
                print(e)
                break

    tc._register_signal_handlers()
    tc.loop.run_until_complete(tc.start(login=True))
    print("Started!")
    tc.loop.run_until_complete(loop())


if __name__ == '__main__':
    main()
