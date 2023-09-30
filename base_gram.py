import string
import math
import struct


DISALLOWED_CHARS = ['\x00', '\x01', '\x02', '\x03', '\x04', '\x05', '\x06', '\x07', '\x08', '\t', '\n', '\x0b', '\x0c', '\r', '\x0e', '\x0f', '\x10', '\x11', '\x12', '\x13', '\x14', '\x15', '\x16', '\x17', '\x18', '\x19', '\x1a', '\x1b', '\x1c', '\x1d', '\x1e', '\x1f', ' ']

# Creating custom alphabet which excludes the disallowed characters
ALLOWED_CHARS = [chr(i) for i in range(0x80) if chr(i) not in DISALLOWED_CHARS]

# ALLOWED_CHARS = [
# 	'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
# 	'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
# 	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
# 	'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
# 	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '!', '#', '$',
# 	'%', '&', '(', ')', '*', '+', ',', '.', '/', ':', ';', '<', '=',
# 	'>', '?', '@', '[', ']', '^', '_', '`', '{', '|', '}', '~', '"']

DECODE_TABLE = dict((v, k) for k,v in enumerate(ALLOWED_CHARS))
BASE = len(ALLOWED_CHARS)
print(f"Base {BASE}")
print(f"Allowed characters: {ALLOWED_CHARS}")


# BASE = 91
MASK_13_BITS = 8191  # 2^13 - 1
MASK_14_BITS = 16383 # 2^14 - 1
MASK_8_BITS = 255    # 2^8 - 1
SHIFT_13_BITS = 13
SHIFT_14_BITS = 14
SHIFT_8_BITS = 8


def encode(data: bytes) -> str:
    b = 0
    n = 0
    result = ''
    for count in range(len(data)):
        byte = data[count:count + 1]
        b |= struct.unpack('B', byte)[0] << n
        n += SHIFT_8_BITS
        if n > SHIFT_13_BITS:
            v = b & MASK_13_BITS
            if v > BASE - 3:  # 88 was 91 - 3
                b >>= SHIFT_13_BITS
                n -= SHIFT_13_BITS
            else:
                v = b & MASK_14_BITS
                b >>= SHIFT_14_BITS
                n -= SHIFT_14_BITS
            result += ALLOWED_CHARS[v % BASE] + ALLOWED_CHARS[v // BASE]
    if n:
        result += ALLOWED_CHARS[b % BASE]
        if n > SHIFT_8_BITS - 1 or b > BASE - 1:  # 7 was 8 - 1 and 90 was 91 - 1
            result += ALLOWED_CHARS[b // BASE]
    return result

def decode(encoded: str) -> bytearray:
    v = -1
    b = 0
    n = 0
    out = bytearray()
    for strletter in encoded:
        if not strletter in DECODE_TABLE:
            continue
        c = DECODE_TABLE[strletter]
        if v < 0:
            v = c
        else:
            v += c * BASE
            b |= v << n
            n += SHIFT_13_BITS if (v & MASK_13_BITS) > BASE - 3 else SHIFT_14_BITS
            while True:
                out += struct.pack('B', b & MASK_8_BITS)
                b >>= SHIFT_8_BITS
                n -= SHIFT_8_BITS
                if not n > SHIFT_8_BITS - 1:
                    break
            v = -1
    if v + 1:
        out += struct.pack('B', (b | v << n) & MASK_8_BITS)
    return out



def fuzz():
    for i in range(0x1_00_00):
        # Convert i to bytes
        a = i.to_bytes(2, byteorder='big')
        b = encode(a)
        c = decode(b)
        if a != c:
            print(f"Failed for {a}")
            print(f"a = {repr(a)}, b = {repr(b)}, c = {repr(c)}")
            exit(1)


def main():
    fuzz()

    a = b'abcdefghijk'
    b = encode(a)
    c = decode(b)
    print(a, b, c, sep='\n')


if __name__ == '__main__':
    main()
