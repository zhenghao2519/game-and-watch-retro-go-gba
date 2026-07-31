#!/usr/bin/env python3
import os, sys

fpath = os.environ.get('FILE')
if not fpath:
    print("ERROR: FILE env not set. Run: export FILE=$(python3 -c \"import littlefs, os; print(os.path.join(os.path.dirname(littlefs.__file__), '__init__.py'))\")")
    sys.exit(1)
if not os.path.exists(fpath):
    print("ERROR: file not found:", fpath); sys.exit(1)

# read
with open(fpath, 'r', encoding='utf-8') as fh:
    s = fh.read()

# insert helper if missing
if "_decode_lfs_name" not in s:
    helper = (
        "\n\n# Helper to robustly decode raw bytes returned by the C extension.\n"
        "def _decode_lfs_name(raw: bytes) -> str:\n"
        "    if isinstance(raw, str):\n"
        "        return raw\n"
        "    if raw is None:\n"
        "        return \"\"\n"
        "    try:\n"
        "        return raw.decode('utf-8')\n"
        "    except UnicodeDecodeError:\n"
        "        try:\n"
        "            return raw.decode('gbk')\n"
        "        except Exception:\n"
        "            return raw.decode('utf-8', errors='surrogateescape')\n\n"
    )
    # try to insert after the context import if present
    idx = s.find("from .context import")
    if idx != -1:
        # find end of that import line
        end = s.find("\n", idx)
        if end != -1:
            s = s[:end+1] + helper + s[end+1:]
        else:
            s = helper + s
    else:
        s = helper + s

# find scandir start and end
start_token = "def scandir(self, path=\".\") -> Iterator[\"LFSStat\"]:"
start_idx = s.find(start_token)
if start_idx == -1:
    # try alternative signature (no type hint)
    start_token = "def scandir(self, path=\".\") -> Iterator[\"LFSStat\"]"
    start_idx = s.find(start_token)
if start_idx == -1:
    print("Warning: could not find scandir definition. Manual patch required.")
    sys.exit(0)

# find the end token (the lfs.dir_close line that ends the function)
end_token = "lfs.dir_close(self.fs, dh)"
end_idx = s.find(end_token, start_idx)
if end_idx == -1:
    print("Warning: could not find end of scandir (lfs.dir_close). Manual patch required.")
    sys.exit(0)

# find end of that line
end_line_idx = s.find("\n", end_idx)
if end_line_idx == -1:
    end_line_idx = len(s)

# build replacement implementation (indented to match file style)
replacement = (
    "def scandir(self, path=\".\") -> Iterator[\"LFSStat\"]:\n"
    "        \"\"\"List directory content\"\"\"\n"
    "        dh = lfs.dir_open(self.fs, path)\n"
    "        info = lfs.dir_read(self.fs, dh)\n"
    "        while info:\n"
    "            # Ensure name is a Python str with robust decoding\n"
    "            try:\n"
    "                raw_name = info.name\n"
    "                info.name = _decode_lfs_name(raw_name)\n"
    "            except Exception:\n"
    "                try:\n"
    "                    info.name = str(info.name)\n"
    "                except Exception:\n"
    "                    info.name = ''\n"
    "            if info.name not in [\".\", \"..\"]:\n"
    "                yield info\n"
    "            info = lfs.dir_read(self.fs, dh)\n"
    "        lfs.dir_close(self.fs, dh)\n"
)

# replace the block
new_s = s[:start_idx] + replacement + s[end_line_idx+1:]
# write back
with open(fpath, 'w', encoding='utf-8') as fh:
    fh.write(new_s)

print("Patched", fpath)
print("If anything goes wrong, restore from:", fpath + ".orig or .bak")
