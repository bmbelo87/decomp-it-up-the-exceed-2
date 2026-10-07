import struct, zlib

with open('F:/14_px3/gamePC/STEP/101.STX','rb') as f:
    data = f.read()

# Check where each zlib stream actually ends
for pos in [0x1F0, 0x2F3, 0x3F2, 0x65B, 0x75A, 0xA0A, 0xC98, 0xD97, 0xE96]:
    # Find the end of this zlib stream
    for end in range(pos + 10, min(pos + 500, len(data))):
        try:
            result = zlib.decompress(data[pos:end])
            # If it succeeded, we found the end
            print(f'Stream at 0x{pos:X}: {end-pos} compressed -> {len(result)} decompressed')
            trailer = data[pos+len(data[pos:end])-4:pos+len(data[pos:end])]
            if len(trailer) >= 4:
                ad = struct.unpack(">I", trailer)[0]
                calc = zlib.adler32(result) & 0xFFFFFFFF
                print(f'  Trailer adler: 0x{ad:08X} calc: 0x{calc:08X} match: {ad==calc}')
            break
        except:
            pass
