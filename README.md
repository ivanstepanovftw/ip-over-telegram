# IP over Telegram

This is a simple program that allows you to create a connection to the Internet through Telegram servers, taking advantage of the unlimited data offerings from mobile carriers.

## Building

1. Install prerequisites

  **Alpine Linux**:
  ```shell
  sudo apk install gcc g++ cmake ninja gperf python3 python3-dev openssl-dev
  ```

2. Build and install td and tuntap libraries
  ```shell
  ./build.sh
  ```

3. Install requirements
  ```shell
  python3 -m pip install -r requirements.txt
  ```

4. Create config files
  Example of `/config.client.yaml` (change `tdconfig.token` to your phone number):
  ```yaml
  tdconfig:
    files_directory: ".td"
    token: "+15555555555"
    api_id: 94575
    api_hash: "a3406de8d171bb422bb6ddf3bbd800e2"
    database_encryption_key: "1234echobot$"
  tun:
    name: "telegram_tun0"
    mtu: 1500
    ip: "10.0.0.2"

  cache_size: 1
  cache_flush_rate: 10
  #cache_size: 0
  #cache_flush_rate: 0

  wrap_in_proxy: false
  receive_from_user_id: 829534074
  send_to_chat_id: 829534074
  ```

  Copy config to `config.server.yaml`, but change TUN's device IP to `ip: "10.0.0.1"` for your internal server TUN device IP. Then rsync config to the server.
  ```shell
   rsync -avz -e ssh config.server.yaml user@company420:/p/ip_over_telegram
  ```

### Run

0. You should have root permissions in order to run this program.
1. On the server:
  ```shell
  sudo sysctl -w net.ipv4.ip_forward=1
  sudo iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
  sudo iptables --policy FORWARD ACCEPT
  sudo modprobe tun
  sudo python3 main.py config.server.yaml
  ```
2. On the client:
  ```shell
  sudo modprobe tun
  sudo python3 main.py config.client.yaml
  ```
3. Setup routes (on the client):
  ```shell
  TELEGRAM_IP_ADDRESSES=(149.154.167.41 149.154.167.51 95.161.76.100)
  DEFAULT_GATEWAY=$(ip route | grep default | awk '{print $3 " dev " $5}')

  # Add Telegram IP addresses to the exception for your default gateway
  for ip in $TELEGRAM_IP_ADDRESSES; do sudo ip route add $ip via $DEFAULT_GATEWAY; done
  
  # Make IP over Telegram as default gateway
  sudo ip route change default via 10.0.0.1 dev telegram_tun0
  ```
4. **Enjoy!**
5. Stop
  ```shell
  sudo ip route change default via $DEFAULT_GATEWAY
  ```

## Alternatives

* [Teletun](https://github.com/PiMaker/Teletun)

## License

Licensed under either of

* Apache License, Version 2.0, ([LICENSE-APACHE-2.0](LICENSE-APACHE-2.0) or http://www.apache.org/licenses/LICENSE-2.0)
* MIT license ([LICENSE-MIT](LICENSE-MIT) or http://opensource.org/licenses/MIT)

at your option.

## Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in the work by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without any
additional terms or conditions.
