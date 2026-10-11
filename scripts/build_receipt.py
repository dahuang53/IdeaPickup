#!/usr/bin/env python3
"""Small before/after receipt for the documented two-board build wrappers."""
import argparse,json
from pathlib import Path
from firmware_bundle import ROOT,PROFILES,build_inputs,require,digest
p=argparse.ArgumentParser();p.add_argument('phase',choices=['before','after']);p.add_argument('board',choices=PROFILES);a=p.parse_args()
folder=ROOT/PROFILES[a.board]['build'];pending=folder/'route1-build-before.json';receipt=folder/'route1-build.json'
current=build_inputs(ROOT,a.board)
if a.phase=='before':
 receipt.unlink(missing_ok=True);pending.write_text(json.dumps(current,sort_keys=True)+'\n')
else:
 require(json.loads(pending.read_text())==current,'build inputs changed; rerun build')
 current['artifacts']={str(p.relative_to(folder)):digest(p.read_bytes()) for p in [folder/'meow_inbox.bin',folder/'bootloader/bootloader.bin',folder/'partition_table/partition-table.bin',folder/'ota_data_initial.bin',folder/'srmodels/srmodels.bin']}
 receipt.write_text(json.dumps(current,sort_keys=True)+'\n');pending.unlink()
