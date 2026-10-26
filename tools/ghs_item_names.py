# Writes game\ghs_items.txt (id;name, English) for the cheat menu's item lists (F5): the English item names are the
# strings of text resource 33, leaf 51 in game\FILE.STM (NUL separated, Latin-1). Game text: the file stays local.
#   python tools\ghs_item_names.py [output file]
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghsarc

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dst = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'game', 'ghs_items.txt')
raw, top = ghsarc.load_file_stm(os.path.join(root, 'game', 'FILE.STM'))
off, size = top[33]
leaf = next(d for path, d in ghsarc.walk_leaves(raw[off:off + size], (33,)) if path == (33, 51))
rows = [(i, s.decode('latin-1').strip()) for i, s in enumerate(x for x in leaf.split(b'\x00') if x)]
rows = [(i, name) for i, name in rows if name and name not in ('? ? ?', 'NULL')]
with open(dst, 'w', encoding='utf-8', newline='\n') as f:
    for i, name in rows:
        f.write(f'{i};{name}\n')
print(f'{len(rows)} names -> {dst}')
