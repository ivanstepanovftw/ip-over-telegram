# ip_over_telegram

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

4. Create config file
  Example of `/config.yaml`:
  ```yaml
  tdconfig:
    files_directory: ".td.db"    
    token: "+15555555555"
    api_id: 94575
    api_hash: "a3406de8d171bb422bb6ddf3bbd800e2"
    database_encryption_key: "1234echobot$"
  
  receive_from_user_id: 829534074
  send_to_chat_id: 829534074
  ```

### Run

0. You should have root permissions in order to run this program
1. On the server:
  ```shell
  sudo modprobe tun
  sudo python3 main.py config.yaml
  ```
2. On the client:
  ```shell
  sudo modprobe tun
  sudo python3 main.py config.yaml
  ```
3. Setup routes
  ```shell
  # Add a route for the TUN device
  ip route add 10.0.0.0/24 dev telegram_tun0
  
  # Redirect all traffic through the TUN device
  ip route change default via 10.0.0.1
  ```

4. **Enjoy!**
5. Stop
  ```shell
  ip route change default via eth0
  or
  ip route change default via wlp1s0
  ```



---
Server preparation:
```shell
echo 1 > /proc/sys/net/ipv4/ip_forward
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
iptables --policy FORWARD ACCEPT
```
```console
$ curl ifconfig.me
188.119.45.172%
$ ip route show dev telegram_tun0
10.0.0.0/24 proto kernel scope link src 10.0.0.1 
$ sudo iptables -t nat -A OUTPUT -p tcp --dport 4090 -j DNAT --to-destination 172.16.0.1
$ sudo iptables -t nat -L -n -v --line-numbers
...
Chain OUTPUT (policy ACCEPT 0 packets, 0 bytes)
num   pkts bytes target     prot opt in     out     source               destination         
1       10  1770 DOCKER     all  --  *      *       0.0.0.0/0           !127.0.0.0/8          ADDRTYPE match dst-type LOCAL
2        0     0 DNAT       tcp  --  *      *       0.0.0.0/0            0.0.0.0/0            tcp dpt:4090 to:172.16.0.1
...
$ # To remove: sudo iptables -t nat -D OUTPUT 2
$ sudo ip route
default via 172.16.0.1 dev wlp1s0 proto dhcp src 172.16.0.80 metric 600 
10.0.0.0/24 dev telegram_tun0 proto kernel scope link src 10.0.0.1 
10.0.85.2 dev outline-tun0 scope link src 10.0.85.1 linkdown 
172.16.0.0/24 dev wlp1s0 proto kernel scope link src 172.16.0.80 metric 600 
172.17.0.0/16 dev docker0 proto kernel scope link src 172.17.0.1 
$ sudo ip route del default via 172.16.0.1 dev wlp1s0
$ sudo ip route add default via 10.0.0.1 dev telegram_tun0
$ sudo ip route


$ sudo ip route add default via 10.0.0.1
$ sudo ip route change default via 172.16.0.1
```


```console
$ TELEGRAM_IP=149.154.167.41
$ curl ifconfig.me
188.119.45.172%
$ ip route show dev telegram_tun0
10.0.0.0/24 proto kernel scope link src 10.0.0.1 


sudo ip route change default via 10.0.0.1 dev telegram_tun0
sudo ip route add 149.154.167.41 via 172.16.0.1 dev wlp1s0

sudo ip route del default via 10.0.0.1 dev telegram_tun0
```
