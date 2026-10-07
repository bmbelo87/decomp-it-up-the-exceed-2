import struct, zlib

with open('F:/14_px3/gamePC/STEP/101.STX','rb') as f:
    data = f.read()

# The zlib stream starts at 0x1F0
# The next zlib stream starts at 0x2F3
# So the first stream goes from 0x1F0 to 0x2F2 inclusive
stream = data[0x1F0:0x2F3]
print(f'Stream: {len(stream)} bytes')
print(f'First 4: {stream[:4].hex()}')
print(f'Last 8:  {stream[-8:].hex()}')

# The last 4 bytes should be the adler32 (big-endian)
adler_from_trailer = struct.unpack(">I", stream[-4:])[0]
print(f'Adler32 from trailer: 0x{adler_from_trailer:08X}')

# Now decompress the stream without last 4 bytes
result = zlib.decompress(stream)
print(f'Decompressed: {len(result)} bytes')

calc_adler = zlib.adler32(result) & 0xFFFFFFFF
print(f'Calculated adler32: 0x{calc_adler:08X}')
print(f'Match: {adler_from_trailer == calc_adler}')

# Actually, zlib.decompress already verified the adler. So:
print(f'zlib.adler32 check passed implicitly (decompress succeeded)')
