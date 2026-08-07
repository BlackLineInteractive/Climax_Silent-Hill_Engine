#!/usr/bin/env python3
"""
merge_port_map.py  —  Merge SHO attribute tables into port_class_map.json.

Strategy:
  For every class in port_class_map.json:

  1. If sho_attribute_map.json has an entry that is NOT marked `inherited`:
       → replace the properties section with the SHO data (more accurate,
         because Origins extended many classes beyond their GR counterparts).

  2. If the SHO entry IS `inherited` (shared handler = base class vtable):
       → keep the existing GR data if present, because GR still gives us
         the correct semantic mapping for shared-engine classes.
       → if no GR data either, leave the slot empty and mark it.

  3. Emit the delta table: classes where SHO count != GR count, so the
     difference is visible in one place.

Usage:
    python3 tools/merge_port_map.py \\
        --map  docs/port_class_map.json \\
        --sho  docs/sho_attribute_map.json \\
        --out  docs/port_class_map.json        # overwrite in-place
"""
import argparse
import json
import sys
from pathlib import Path


# ─────────────────────────────────────────────────────────────────────────────
# Merge logic
# ─────────────────────────────────────────────────────────────────────────────

def merge(port_map: dict, sho_map: dict) -> tuple[dict, list]:
    """Returns (merged_map, delta_rows).

    delta_rows is a list of dicts describing every class where the property
    count changed (or was newly discovered).
    """
    classes = port_map['classes']
    deltas  = []

    for name, entry in classes.items():
        sho = sho_map.get(name)
        if sho is None:
            continue  # SHO extractor didn't reach this class

        inherited = sho.get('inherited', False)

        if inherited:
            # Keep GR data if we have it; SHO data is just the base class.
            # But record it so we know which base handler it fell back to.
            entry.setdefault('sho_handler_inherited', sho['address'])
            continue

        # Non-inherited → SHO data is authoritative for this class in Origins.
        old_count = len(entry.get('properties') or [])
        new_count = sho['count']

        # Build the merged property list.
        # GR may have store-type and setter-call info; SHO gives the real count.
        # Strategy: start from SHO properties (correct count), then enrich each
        # property with GR store info where the index matches.
        gr_props_by_idx = {}
        for p in (entry.get('properties') or []):
            gr_props_by_idx[p['index']] = p

        merged_props = []
        for p in sho['properties']:
            idx   = p['index']
            gr_p  = gr_props_by_idx.get(idx, {})
            # SHO gives us the case address in SHO's binary.
            # GR gives stores/calls (semantic meaning).
            merged_p = {
                'index':       idx,
                'case_sho':    p.get('case'),
                'case_gr':     gr_p.get('case'),
                'stores':      p.get('stores') or gr_p.get('stores') or [],
            }
            # Prefer SHO stores (they come from Origins itself), fall back to GR.
            if not merged_p['stores'] and gr_p.get('stores'):
                merged_p['stores'] = gr_p['stores']
                merged_p['stores_source'] = 'gr'
            elif merged_p['stores']:
                merged_p['stores_source'] = 'sho'
            merged_props.append(merged_p)

        entry['sho_address']  = sho['address']
        entry['sho_vtable']   = sho['vtable']
        entry['dispatch']     = sho['dispatch']   # SHO dispatch form
        entry['properties']   = merged_props
        entry['sho_count']    = new_count
        entry['gr_count']     = old_count if old_count else None

        if new_count != old_count:
            deltas.append({
                'class':     name,
                'gr_count':  old_count,
                'sho_count': new_count,
                'diff':      new_count - old_count,
                'dispatch':  sho['dispatch'],
                'handler':   sho['address'],
            })

    # Sort deltas: biggest diff first (most interesting for porting).
    deltas.sort(key=lambda r: abs(r['diff']), reverse=True)
    return port_map, deltas


def coverage_stats(port_map: dict, sho_map: dict) -> dict:
    classes  = port_map['classes']
    total    = len(classes)

    sho_hit  = sum(1 for n in classes if n in sho_map)
    sho_own  = sum(1 for n in classes
                   if n in sho_map and not sho_map[n].get('inherited'))
    sho_inh  = sho_hit - sho_own
    sho_miss = total - sho_hit

    with_props = sum(1 for e in classes.values() if e.get('properties'))
    fields  = sum(sum(1 for s in (p.get('stores') or []) if s.get('offset'))
                  for e in classes.values()
                  for p in (e.get('properties') or []))
    calls   = sum(sum(1 for s in (p.get('stores') or []) if s.get('calls'))
                  for e in classes.values()
                  for p in (e.get('properties') or []))

    return {
        'total':           total,
        'sho_own':         sho_own,
        'sho_inherited':   sho_inh,
        'sho_miss':        sho_miss,
        'with_properties': with_props,
        'field_stores':    fields,
        'setter_calls':    calls,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--map', '-m', default='docs/port_class_map.json',
                    help='Input/output port_class_map.json')
    ap.add_argument('--sho', '-s', default='docs/sho_attribute_map.json',
                    help='sho_attribute_map.json from sho_attrs.py')
    ap.add_argument('--out', '-o', default=None,
                    help='Output path (default: overwrite --map in-place)')
    ap.add_argument('--dry-run', action='store_true',
                    help='Print stats only, do not write')
    args = ap.parse_args()

    port_map = json.loads(Path(args.map).read_text())
    sho_map  = json.loads(Path(args.sho).read_text())

    print(f'port_class_map : {len(port_map["classes"])} classes')
    print(f'sho_attr_map   : {len(sho_map)} entries')

    # ── Before stats ──────────────────────────────────────────────────────────
    before = coverage_stats(port_map, sho_map)

    # ── Merge ─────────────────────────────────────────────────────────────────
    merged, deltas = merge(port_map, sho_map)

    # ── After stats ───────────────────────────────────────────────────────────
    after = coverage_stats(merged, sho_map)

    print(f'\n{"─"*60}')
    print(f'{"Metric":<30} {"Before":>8} {"After":>8}')
    print(f'{"─"*60}')
    for key in ('total','sho_own','sho_inherited','sho_miss',
                'with_properties','field_stores','setter_calls'):
        b = before.get(key, '–')
        a = after.get(key, '–')
        marker = ' ←' if a != b else ''
        print(f'  {key:<28} {b:>8} {a:>8}{marker}')
    print(f'{"─"*60}')

    # ── Delta table ───────────────────────────────────────────────────────────
    if deltas:
        print(f'\nProperty count changes ({len(deltas)} classes):')
        print(f'  {"Class":<32} {"GR":>5} {"SHO":>5} {"Diff":>6}  Dispatch')
        print(f'  {"─"*32}  {"─"*5}  {"─"*5}  {"─"*6}  {"─"*14}')
        for r in deltas:
            gr_s = str(r["gr_count"]) if r["gr_count"] else "none"
            print(f'  {r["class"]:<32} {gr_s:>5} {r["sho_count"]:>5} {r["diff"]:>+6}  {r["dispatch"]}')

    # ── Write ─────────────────────────────────────────────────────────────────
    if args.dry_run:
        print('\n[dry-run] Not written.')
        return

    out_path = args.out or args.map
    # Update metadata
    merged['source'] = {
        'sho': 'SLES_551.47',
        'gr':  'SLES_543.17',
        'note': 'SHO properties are authoritative for count; '
                'GR provides semantic names where available.',
    }

    Path(out_path).write_text(json.dumps(merged, indent=1))
    print(f'\nWrote {out_path}  ({Path(out_path).stat().st_size // 1024} KB)')


if __name__ == '__main__':
    main()
