"""Real source/export failure boundaries using synthetic private directories."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name):
 spec=importlib.util.spec_from_file_location(name,ROOT/'scripts'/f'{name}.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
exporter=module('export_public_source');identity=module('source_identity')
class PublicExportTests(unittest.TestCase):
 def setUp(self):
  base=ROOT/'private/public-export-tests';base.mkdir(mode=0o700,parents=True,exist_ok=True)
  self.tmp=tempfile.TemporaryDirectory(dir=base);self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
  self.addCleanup(patch.stopall);patch.object(exporter,'REQUIRED_FILES',{'README.md','main/app.c'}).start()
  self.write('README.md','Synthetic public source.\n');self.write('main/app.c','int main(void){return 0;}\n')
 def write(self,name,text):
  f=self.root/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text);return f
 def test_export_omits_git_private_history_and_keeps_real_bytes_modes(self):
  self.write('private/key.env','opaque secret');self.write('.git/config','private history');self.write('evidence/log.json','private record')
  f=self.write('scripts/entry.sh','#!/bin/sh\nexit 0\n');f.chmod(0o755)
  report=exporter.candidate(self.root);self.assertFalse(report['findings']);out=self.root/'private/export'
  exporter.export(self.root,out,report);self.assertFalse((out/'private').exists());self.assertFalse((out/'.git').exists());self.assertFalse((out/'evidence').exists())
  self.assertEqual((out/'scripts/entry.sh').read_bytes(),f.read_bytes());self.assertEqual((out/'scripts/entry.sh').stat().st_mode&0o777,0o755)
  self.assertFalse(json.loads((out/'PUBLIC_SOURCE_MANIFEST.json').read_text())['publication_authorized'])
 def test_approved_design_exports_without_other_design_materials(self):
  name='docs/design/admin-approved/index.html'
  reference=self.write(name,'<html>Approved synthetic reference.</html>\n')
  self.write('docs/design/admin-approved/private-notes.md','opaque design notes')
  self.write('docs/design/admin-approved/nested/index.html','opaque nested design')
  self.write('docs/design/unapproved/index.html','opaque draft')
  self.write('design/admin-approved/index.html','opaque unrelated design')
  with patch.object(exporter,'REQUIRED_FILES',{'README.md','main/app.c',name}):
   report=exporter.candidate(self.root);self.assertFalse(report['findings'])
   out=self.root/'private/export';exporter.export(self.root,out,report)
   self.assertEqual((out/name).read_bytes(),reference.read_bytes())
   self.assertEqual([x.relative_to(out/'docs/design').as_posix() for x in (out/'docs/design').rglob('*') if x.is_file()],['admin-approved/index.html'])
   self.assertFalse((out/'design').exists())
   reference.unlink()
   self.assertIn({'path':name,'category':'required_public_source_missing'},exporter.candidate(self.root)['findings'])
 def test_public_secret_rejects_without_disclosing_value(self):
  value='sk-'+'a'*32;self.write('README.md',value)
  report=exporter.candidate(self.root);self.assertTrue(report['findings']);self.assertNotIn(value,json.dumps(report['findings']))
  with self.assertRaises(ValueError):exporter.export(self.root,self.root/'private/export',report)
 def test_no_synthetic_prefix_or_folder_wide_secret_exception(self):
  value='synthetic-real-looking-'+('a'*32);self.write('tests/new_fixture.py',f'api_key="{value}"\n')
  self.assertTrue(exporter.candidate(self.root)['findings'])
 def test_unknown_binary_links_and_missing_required_source_reject(self):
  f=self.root/'main/new.pcm';f.write_bytes(b'new');self.assertTrue(exporter.candidate(self.root)['findings']);f.unlink()
  f.symlink_to(self.root/'README.md');self.assertTrue(exporter.candidate(self.root)['findings']);f.unlink()
  (self.root/'main/app.c').unlink();self.assertTrue(exporter.candidate(self.root)['findings'])
 def test_changed_source_after_scan_and_existing_destination_reject(self):
  report=exporter.candidate(self.root);self.write('main/app.c','changed\n');out=self.root/'private/export'
  with self.assertRaises(ValueError):exporter.export(self.root,out,report)
  self.assertFalse(out.exists());report=exporter.candidate(self.root);exporter.export(self.root,out,report)
  with self.assertRaises(ValueError):exporter.export(self.root,out,report)
 def test_own_git_and_archive_identity_do_not_inherit_private_parent(self):
  subprocess.run(['git','init','-q',str(self.root)],check=True)
  subprocess.run(['git','-C',str(self.root),'-c','user.name=Synthetic','-c','user.email=test@example.invalid','commit','--allow-empty','-qm','fixture'],check=True)
  commit=subprocess.check_output(['git','-C',str(self.root),'rev-parse','HEAD'],text=True).strip();self.assertEqual(identity.source_identity(self.root),commit)
  child=self.root/'private/child';(child/'main').mkdir(parents=True);(child/'main/app.c').write_text('int archive;\n')
  archive=identity.source_identity(child);self.assertTrue(archive.startswith('source-sha256:'));self.assertNotIn(commit,archive)
  (child/'README.md').write_text('docs only');self.assertEqual(identity.source_identity(child),archive)
  (child/'main/app.c').write_text('int changed;\n');self.assertNotEqual(identity.source_identity(child),archive)
if __name__=='__main__':unittest.main()
