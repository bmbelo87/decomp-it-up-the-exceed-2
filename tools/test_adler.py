import struct, zlib
with open('F:/14_px3/gamePC/STEP/101.STX','rb') as f:
    data = f.read()

result = zlib.decompress(data[0x1F0:])
print(f'Decompressed: {len(result)} bytes')

expected_adler = struct.unpack(">I", data[0x1F0+len(data[0x1F0:])-4:][:4])[0]
calculated_adler = zlib.adler32(result) & 0xFFFFFFFF
print(f'Expected adler32 from stream: 0x{expected_adler:08X}')
print(f'Calculated adler32: 0x{calculated_adler:08X}')
print(f'Match: {expected_adler == calculated_adler}')

with open('F:/14_px3/gamePC/STEP/101.decompressed', 'wb') as f:
    f.write(result)
print("Wrote 101.decompressed")
