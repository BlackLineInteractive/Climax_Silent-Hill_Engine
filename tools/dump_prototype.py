import sys, os, struct

def parse_metadata(meta_data):
    # Search for the 0x071C chunk (Ghost Rider / early SHO Container)
    offset_071c = meta_data.find(b'\x1C\x07\x00\x00')
    if offset_071c == -1:
        return []
    
    # Very rudimentary string extraction from metadata block
    strings = []
    # Skip standard chunk headers and search for printable ASCII sequences
    import re
    # Match typical Class names (CZone, etc) or Paths (z:\...) or Textures
    pattern = re.compile(b'[A-Za-z0-9_\\\\:-]{4,}')
    for match in pattern.finditer(meta_data[offset_071c:]):
        s = match.group().decode('ascii')
        if s.startswith('rwID_') or s == 'A2.0': continue
        strings.append(s)
        
    return strings

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 dump_prototype.py <path_to_GR.ARC> <out_dir>")
        sys.exit(1)
        
    arc_path = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    
    with open(arc_path, 'rb') as f:
        hdr = f.read(16)
        magic, meta_offset, arc_size, num_files = struct.unpack('<4I', hdr)
        
        print(f"[GR.ARC] Found Magic: {magic}, Files: {num_files}, Size: {arc_size}")
        
        toc_size = num_files * 16
        toc_data = f.read(toc_size)
        
        # Extract File 0 (Metadata)
        _, f0_off, f0_size, _ = struct.unpack('<4I', toc_data[0:16])
        f.seek(f0_off)
        meta_data = f.read(f0_size)
        
        print(f"[Metadata] Block Size: {f0_size} bytes")
        strings = parse_metadata(meta_data)
        
        with open(os.path.join(out_dir, "metadata_strings.txt"), 'w') as mf:
            for s in strings:
                mf.write(s + '\n')
                
        print(f"[Metadata] Dumped {len(strings)} strings to metadata_strings.txt")
        
        # Extract assets
        print("[Extractor] Dumping assets...")
        for i in range(1, num_files):
            name_idx, file_off, file_size, _ = struct.unpack('<4I', toc_data[i*16:(i+1)*16])
            f.seek(file_off)
            data = f.read(file_size)
            
            # Detect RenderWare payload embedded after property table
            ext = ".bin"
            if len(data) > 100:
                # Scan for RW chunk
                for offset in range(0, min(200, len(data) - 12), 4):
                    t, s, v = struct.unpack('<3I', data[offset:offset+12])
                    if v == 0x1C020065 or v == 0x1803FFFF: # RW 3.7.0.2 or 3.5.0.0
                        if t == 0x14: ext = ".atomic"
                        elif t == 0x10: ext = ".clump"
                        elif t == 0x01: ext = ".struct"
                        break
            
            # Use string from metadata if name_idx is a valid index, otherwise hex
            # (Note: name_idx doesn't exactly map 1:1 to string array without the hierarchy parser, using generic names for now)
            filename = f"asset_{i:04d}_id{name_idx}{ext}"
            
            with open(os.path.join(out_dir, filename), 'wb') as out_f:
                out_f.write(data)
                
        print(f"[Extractor] Successfully extracted {num_files - 1} assets to {out_dir}")

if __name__ == '__main__':
    main()
