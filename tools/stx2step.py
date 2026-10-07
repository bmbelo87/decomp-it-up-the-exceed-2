#!/usr/bin/env python3
"""Convert Pump It Up STX step files to a simpler .STM binary format."""
import struct, zlib, os, sys

def parse_stx(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic = data[0:4]
    assert magic == b'STF4', f"Bad magic: {magic}"
    # Song name at 0x30 (may be shifted)
    name_start = 0x30
    song_name = data[name_start:name_start+64].split(b'\0')[0].decode('cp949', errors='replace').strip('\x00').strip()
    if not song_name:
        song_name = data[0x3C:0x7C].split(b'\0')[0].decode('cp949', errors='replace').strip('\x00').strip()
    
    # Find all zlib sections
    sections = []
    pos = 0
    while pos < len(data) - 4:
        if data[pos] == 0x78 and data[pos+1] in (0x9C, 0xDA):
            try:
                decomp = zlib.decompress(data[pos:])
                sections.append({
                    'pos': pos,
                    'data': decomp,
                    'bpm': struct.unpack_from('<f', decomp, 0)[0],
                    'type': struct.unpack_from('<I', decomp, 4)[0],
                    'subdiv': struct.unpack_from('<I', decomp, 8)[0],
                    'count': struct.unpack_from('<I', decomp, 12)[0],
                })
            except:
                pass
        pos += 1
    return {'name': song_name, 'sections': sections}

def extract_step_data(decomp_data):
    """Extract step rows from decompressed zlib data."""
    header_size = 16
    d = decomp_data
    
    # Find where actual data starts (skip header and marker section)
    data_start = header_size
    
    # Parse step data: 16-byte blocks, each containing 2 beat-halves
    # Each half = 5 panel bytes (L, D, U, R, C) + 3 padding bytes
    rows = []
    i = data_start
    while i + 16 <= len(d):
        # First half: panels[0..4], padding[5..7]
        half1 = list(d[i:i+5])
        # Second half: panels[8..12], padding[13..15]
        half2 = list(d[i+8:i+13])
        
        if max(half1) > 0 or max(half2) > 0:
            beat = len(rows)
            rows.append((beat, half1, half2))
        i += 16
    
    return rows

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + "/../STEP"

def convert_song(song_id):
    stx_path = f"{BASE}/{song_id}.STX"
    if not os.path.exists(stx_path):
        stx_path = f"F:/14_px3/gamePC/STEP/{song_id}.STX"
    if not os.path.exists(stx_path):
        return None
    
    song = parse_stx(stx_path)
    result = []
    result.append(f"# {song['name']} ({song_id})")
    result.append(f"# Sections: {len(song['sections'])}")
    
    # Group sections by (subdiv, count) to identify unique charts
    groups = {}
    for s in song['sections']:
        key = (s['subdiv'], s['count'])
        if key not in groups:
            groups[key] = []
        groups[key].append(s)
    
    for key, secs in groups.items():
        subdiv, count = key
        # Take section with most non-zero bytes
        main = max(secs, key=lambda s: sum(1 for b in s['data'][16:] if b != 0))
        nz = sum(1 for b in main['data'][16:] if b != 0)
        if nz < 4:
            continue
        
        rows = extract_step_data(main['data'])
        print(f"  {song_id}: BPM={main['bpm']:.1f} subdiv={subdiv} count={count} "
              f"nz={nz} data={len(main['data'])}B rows={len(rows)}")
        
        # Convert to simple text format for verification
        result.append(f"\n-- Chart: BPM={main['bpm']:.1f} subdiv={subdiv} --")
        for beat, h1, h2 in rows[:20]:
            p1 = ''.join(['X' if p else '.' for p in h1])
            p2 = ''.join(['X' if p else '.' for p in h2])
            result.append(f"  {beat:4d}: [{p1}] [{p2}]")
    
    return '\n'.join(result)

if __name__ == '__main__':
    songs = [
        "101","102","104","108","109","112",
        "202","203","204","205","212",
        "301","302","303","305","306","310","311","312","318",
        "401","402","403","404","405","413","414",
        "501","503","504",
        "701","703","704","705","711","712","714","719","721","730","735","736",
        "801","802","803","804","805","806","807","808","809","810","811","812",
        "813","814","815","816","817","818","819","820",
    ]
    for sid in songs:
        result = convert_song(sid)
        if result:
            outpath = f"F:/14_px3/gamePC/STEP/{sid}.txt"
            with open(outpath, 'w', encoding='utf-8') as f:
                f.write(result)
            print(f"Wrote {outpath}", flush=True)
