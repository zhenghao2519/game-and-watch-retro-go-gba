from pathlib import Path
print(repr(__file__))
print(repr(Path(__file__).parent))
print(repr(Path('roms/test').parent / 'file'))
print(''.join([i if i.isalnum() else '_' for i in 'roms/md/伊苏3.md']))
