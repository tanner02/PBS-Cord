#!/usr/bin/env python3
"""Embed the tile icon and CA bundle. The viewer HTML now lives in PBS's
own webui.h (payload/embed_assets.py). No downloads and no network access."""
from pathlib import Path
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
root=Path(__file__).resolve().parent.parent
args.out.mkdir(parents=True,exist_ok=True)
icon=(root/'web'/'icon0.png').read_bytes()
assets=('inline const unsigned char asset_icon0_png[] = {'+','.join(str(b) for b in icon)+'};\n'
        'inline constexpr unsigned long long asset_icon0_png_len = '+str(len(icon))+';\n')
(args.out/'assets.hpp').write_text('#pragma once\n'+assets,encoding='utf-8')
ca=(root/'vendor'/'ca-bundle.pem').read_text()
(args.out/'ca_bundle.hpp').write_text('#pragma once\ninline constexpr char PBSD_CA_BUNDLE[] = {'+','.join(str(b) for b in ca.encode())+',0};\n',encoding='utf-8')
