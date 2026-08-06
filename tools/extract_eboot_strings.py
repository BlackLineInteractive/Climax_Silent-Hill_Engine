import sys, re

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from attrmap import Elf

def extract_strings(elf):
    strings = []
    # Class names usually start with C, or rwID, or are CamelCase
    pattern = re.compile(b'[A-Za-z0-9_]{5,}')
    
    for sec_name, (sa, so, ssz) in elf.sec.items():
        if sec_name in ('.rodata', '.data'):
            data = elf.d[so:so+ssz]
            for match in pattern.finditer(data):
                s = match.group().decode('ascii')
                # Filter for likely C++ classes (Starts with C followed by capital, or rwID_)
                if (s.startswith('C') and len(s) > 2 and s[1].isupper()) or s.startswith('rwID_') or 'Climax' in s or s.startswith('Game'):
                    strings.append(s)
    
    # Deduplicate and sort
    return sorted(list(set(strings)))

if __name__ == '__main__':
    path = sys.argv[1]
    elf = Elf(path)
    strings = extract_strings(elf)
    with open('/tmp/eboot_classes.txt', 'w') as f:
        for s in strings:
            f.write(s + '\n')
    print(f"Extracted {len(strings)} class-like strings to /tmp/eboot_classes.txt")
